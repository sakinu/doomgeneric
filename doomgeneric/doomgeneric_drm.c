/* DOOM-owns-display backend, launched by the cellphone app via vfork.
 *
 * Layout on the 240x320 panel:
 *   game  : panel cols [0..199]  (dg_x_offset = 0, set below)
 *   strip : panel cols [200..239] -- reserved for buttons; for now it's the
 *           "exit" button (tap it to quit -> cellphone reclaims the display).
 *
 * Touch is read via tslib (same calibration as cellphone). Touches feed a key
 * queue that DG_GetKey drains -- that's the reserved touch->DOOM-input interface;
 * a game-area tap currently injects FIRE as a placeholder, easy to extend.
 *
 * Output is double-buffered: two dumb buffers, DOOM renders into the back one
 * and DG_DrawFrame page-flips to it at vblank (no tearing, zero-copy). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <linux/fb.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_fourcc.h>
#include <tslib.h>

#include "doomgeneric.h"
#include "doomkeys.h"

extern uint16_t * DG_ScreenBuffer;
extern int dg_x_offset;                  /* i_video.c: left margin of the game */
struct fb_fix_screeninfo finfo;          /* only .line_length is used by i_video.c */

#define PANEL_W   240
#define GAME_W    200                    /* game occupies cols [0..199] */
#define STRIP_X   GAME_W                 /* button strip starts here */

static int drm_fd = -1;
static struct tsdev * s_ts;
static volatile int s_quit;

/* ---- double buffering: two dumb buffers page-flipped at vblank ---- */
static uint32_t s_crtc_id, s_conn_id;
static drmModeModeInfo s_mode;
static uint32_t s_fb[2];
static uint8_t * s_map[2];
static uint32_t s_pitch;
static int s_back = 1;               /* DOOM renders into s_map[s_back] */
static int s_double;                 /* 1 = two buffers (page-flip), 0 = single */
static volatile int s_flip_pending;
static drmEventContext s_evctx;

/* Button strip color: since the panel buffer is now 8-bit palette indices
 * (DRM_FORMAT_C8), the strip's grey can't be an arbitrary RGB value anymore
 * -- it has to be *some* index into the one CLUT that DOOM's palette also
 * drives. DG_SetPalette() picks whichever index in the current palette is
 * closest to a neutral grey and repaints the strip with it. */
static uint8_t s_strip_idx;
static int s_strip_ready;

/* ---- touch -> DOOM key queue (reserved input interface) ---- */
#define KQ 32
static unsigned char kq_key[KQ];
static int           kq_pr[KQ];
static int           kq_head, kq_tail;

static void push_key(int pressed, unsigned char key)
{
    int n = (kq_tail + 1) % KQ;
    if(n == kq_head) return;             /* full, drop */
    kq_key[kq_tail] = key;
    kq_pr[kq_tail]  = pressed;
    kq_tail = n;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull;
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}

/* ---- per-frame timing (printed every 30 frames) ---- */
uint64_t g_rot_us;                   /* set by i_video.c: rotation/copy loop */
uint64_t g_pal_us;                   /* set by i_video.c: I_SetPalette / CLUT push */
static uint64_t g_vsync_us;          /* set by DG_DrawFrame: flip + vsync wait */

/* Map a touch (panel coords, calibrated) to actions. RESERVED INTERFACE: extend
 * the game-area branch with real on-screen controls / button hit-tests later. */
static void handle_touch(int x, int y, int pressed_edge)
{
    (void)y;
    if(!pressed_edge) return;
    if(x >= STRIP_X) {
        s_quit = 1;                      /* tap the strip -> exit DOOM */
    }
    else {
        /* placeholder game control: tap fires */
        push_key(1, KEY_FIRE);
        push_key(0, KEY_FIRE);
    }
}

static int s_was_down;

/* Drain pending samples. act=1 -> process press-edges; act=0 -> discard.
 * tslib opens its evdev fd in DG_Init, so the launch tap (and anything during
 * the multi-second WAD load) queues up. We pump_touch(0) once before the game
 * loop to throw that backlog away and sync s_was_down, otherwise the launching
 * tap on the Doom icon (right column, x>=200) replays as a strip tap -> instant
 * exit. Releases never act (handle_touch ignores them), so a finger still held
 * at loop start can't quit either -- it takes a fresh press. */
static void pump_touch(int act)
{
    if(!s_ts) return;
    struct ts_sample s;
    while(ts_read(s_ts, &s, 1) == 1) {
        int down = s.pressure > 0;
        if(act && down && !s_was_down) handle_touch(s.x, s.y, 1);
        s_was_down = down;
    }
}

/* Paint the reserved button strip [200..239] (DOOM never writes there).
 * Called at buffer creation and again whenever DG_SetPalette re-picks
 * s_strip_idx, since a palette change can move the "closest grey" index. */
static void draw_strip(uint8_t * buf, int stride_bytes)
{
    for(int row = 0; row < 320; row++)
        for(int col = STRIP_X; col < PANEL_W; col++)
            buf[row * stride_bytes + col] = s_strip_idx;
}

static void draw_strip_all(void)
{
    draw_strip(s_map[0], s_pitch);
    if(s_double) draw_strip(s_map[1], s_pitch);
}

/* backend hook, called by i_video.c's I_SetPalette: push 256 gamma-corrected
 * {r,g,b} triples into the LTDC's hardware CLUT. The index->RGB conversion
 * that used to run per-pixel in I_FinishUpdate now happens once per palette
 * change, in hardware, during scan-out. */
void DG_SetPalette(const uint8_t * rgb256x3)
{
    uint16_t red[256], green[256], blue[256];
    for(int i = 0; i < 256; i++) {
        red[i]   = (uint16_t)rgb256x3[i*3 + 0] << 8;
        green[i] = (uint16_t)rgb256x3[i*3 + 1] << 8;
        blue[i]  = (uint16_t)rgb256x3[i*3 + 2] << 8;
    }
    if(s_crtc_id) drmModeCrtcSetGamma(drm_fd, s_crtc_id, 256, red, green, blue);

    /* nearest palette entry to a neutral grey, for the button strip */
    const int target = 66;
    int best = 0, best_d = 1 << 30;
    for(int i = 0; i < 256; i++) {
        int dr = (int)rgb256x3[i*3 + 0] - target;
        int dg = (int)rgb256x3[i*3 + 1] - target;
        int db = (int)rgb256x3[i*3 + 2] - target;
        int d = dr*dr + dg*dg + db*db;
        if(d < best_d) { best_d = d; best = i; }
    }
    if(!s_strip_ready || (uint8_t)best != s_strip_idx) {
        s_strip_idx = (uint8_t)best;
        s_strip_ready = 1;
        draw_strip_all();
    }
}

static void page_flip_handler(int fd, unsigned int seq, unsigned int sec,
                              unsigned int usec, void * data)
{
    (void)fd; (void)seq; (void)sec; (void)usec;
    *(volatile int *)data = 0;           /* clears s_flip_pending */
}

/* Create one dumb C8 buffer (FB + mmap), filling s_fb[idx]/s_map[idx] and the
 * shared s_pitch. Each pixel is a raw 8-bit palette index; the LTDC's
 * hardware CLUT (see DG_SetPalette) converts index -> RGB during scan-out,
 * so there's no software color conversion anywhere in this path. */
static int create_dumb_buf(int idx, uint32_t w, uint32_t h)
{
    struct drm_mode_create_dumb creq;
    memset(&creq, 0, sizeof(creq));
    creq.width = w; creq.height = h; creq.bpp = 8;
    if(drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq)) { perror("create dumb"); return -1; }
    s_pitch = creq.pitch;

    uint32_t handles[4] = { creq.handle }, pitches[4] = { creq.pitch }, offsets[4] = { 0 };
    if(drmModeAddFB2(drm_fd, w, h, DRM_FORMAT_C8, handles, pitches, offsets, &s_fb[idx], 0)) {
        perror("addfb2"); return -1;
    }

    struct drm_mode_map_dumb mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = creq.handle;
    if(drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq)) { perror("map dumb"); return -1; }

    s_map[idx] = mmap(0, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, mreq.offset);
    if(s_map[idx] == MAP_FAILED) { perror("mmap"); return -1; }
    memset(s_map[idx], 0, creq.size);
    return 0;
}

void DG_Init(void)
{
    dg_x_offset = 0;                     /* left-align game to cols [0..199] */

    drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if(drm_fd < 0) drm_fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
    if(drm_fd < 0) { perror("open drm"); exit(1); }

    drmModeRes * res = drmModeGetResources(drm_fd);
    if(!res) { fprintf(stderr, "drmModeGetResources failed\n"); exit(1); }

    drmModeConnector * conn = NULL;
    for(int i = 0; i < res->count_connectors; i++) {
        drmModeConnector * c = drmModeGetConnector(drm_fd, res->connectors[i]);
        if(c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) { conn = c; break; }
        if(c) drmModeFreeConnector(c);
    }
    if(!conn) { fprintf(stderr, "no connected connector\n"); exit(1); }

    drmModeModeInfo mode = conn->modes[0];
    uint32_t conn_id = conn->connector_id;

    uint32_t crtc_id = res->crtcs[0];
    drmModeEncoder * enc = conn->encoder_id ? drmModeGetEncoder(drm_fd, conn->encoder_id) : NULL;
    if(enc) { crtc_id = enc->crtc_id; drmModeFreeEncoder(enc); }

    /* The primary buffer must succeed. The second enables tear-free page-flip
     * double buffering; on no-MMU it may fail to find a contiguous block under
     * memory pressure -- if so, degrade to single buffering (direct, can tear)
     * rather than dying. */
    if(create_dumb_buf(0, mode.hdisplay, mode.vdisplay)) exit(1);
#ifdef DOOM_FORCE_SINGLE
    s_double = 0;                        /* comparison build: force single buffer */
#else
    s_double = (create_dumb_buf(1, mode.hdisplay, mode.vdisplay) == 0);
#endif

    /* paint the strip with whatever index s_strip_idx defaults to (0); it gets
     * repainted with the real "closest to grey" index once DOOM's first
     * I_SetPalette call reaches DG_SetPalette below */
    draw_strip_all();

    /* show buffer 0; with double buffering DOOM renders the first frame into
     * buffer 1 (the back), otherwise straight into buffer 0 (the live scanout) */
    if(drmModeSetCrtc(drm_fd, crtc_id, s_fb[0], 0, 0, &conn_id, 1, &mode)) { perror("setcrtc"); exit(1); }

    s_crtc_id = crtc_id;
    s_conn_id = conn_id;
    s_mode    = mode;
    s_evctx.version = DRM_EVENT_CONTEXT_VERSION;
    s_evctx.page_flip_handler = page_flip_handler;

    s_back = s_double ? 1 : 0;
    DG_ScreenBuffer  = (uint16_t *)s_map[s_back];
    finfo.line_length = s_pitch;

    /* tslib: same env (TSLIB_CALIBFILE/CONFFILE) the cellphone uses */
    s_ts = ts_setup(NULL, 1);            /* non-blocking */
    if(!s_ts) fprintf(stderr, "ts_setup failed -- no touch (can't exit!)\n");

    printf("DOOM: %dx%d game[0..%d] strip[%d..%d] touch=%s buffers=%d\n",
           mode.hdisplay, mode.vdisplay, GAME_W - 1, STRIP_X, PANEL_W - 1,
           s_ts ? "on" : "OFF", s_double ? 2 : 1);
    fflush(stdout);
}

void DG_DrawFrame(void)
{
    uint64_t t0 = now_us();

    if(!s_double) { g_vsync_us = now_us() - t0; return; }  /* single: no flip/wait */

    /* DOOM just finished rendering into s_map[s_back]; show it at the next
     * vblank via a page flip (tear-free, no copy). */
    if(drmModePageFlip(drm_fd, s_crtc_id, s_fb[s_back], DRM_MODE_PAGE_FLIP_EVENT,
                       (void *)&s_flip_pending)) {
        /* flip unsupported/busy -> degrade to a direct modeset (shows the frame
         * but can tear). Still alternate buffers so rendering stays correct. */
        static int warned;
        if(!warned) { perror("DOOM: pageflip (falling back to setcrtc)"); warned = 1; }
        drmModeSetCrtc(drm_fd, s_crtc_id, s_fb[s_back], 0, 0, &s_conn_id, 1, &s_mode);
        s_back ^= 1;
        DG_ScreenBuffer = (uint16_t *)s_map[s_back];
        g_vsync_us = now_us() - t0;
        return;
    }
    s_flip_pending = 1;

    /* Wait for the flip to latch; keep touch responsive (and exitable). Bail
     * after ~500ms so a missing vblank event can't freeze DOOM. */
    struct pollfd pfd = { .fd = drm_fd, .events = POLLIN };
    int waited = 0;
    while(s_flip_pending && !s_quit) {
        int r = poll(&pfd, 1, 50);
        if(r > 0 && (pfd.revents & POLLIN)) drmHandleEvent(drm_fd, &s_evctx);
        else if(r == 0 && (waited += 50) >= 500) {
            static int warned;
            if(!warned) { printf("DOOM: no flip event in %dms, continuing\n", waited); warned = 1; }
            break;
        }
        pump_touch(1);
    }

    /* the flipped buffer is now the front; render the next frame into the other */
    s_back ^= 1;
    DG_ScreenBuffer = (uint16_t *)s_map[s_back];
    g_vsync_us = now_us() - t0;
}

void DG_SleepMs(uint32_t ms) { usleep(ms * 1000); }
uint32_t DG_GetTicksMs(void) { return (uint32_t)now_ms(); }
void DG_SetWindowTitle(const char * title) { (void)title; }

int DG_GetKey(int * pressed, unsigned char * key)
{
    if(kq_head == kq_tail) return 0;
    *pressed = kq_pr[kq_head];
    *key     = kq_key[kq_head];
    kq_head  = (kq_head + 1) % KQ;
    return 1;
}

int main(int argc, char ** argv)
{
    printf("=== DOOM (cellphone app, vfork'd) ===\n");
    doomgeneric_Create(argc, argv);
    pump_touch(0);                           /* discard the launch-tap backlog */

    uint64_t acc_game = 0, acc_rot = 0, acc_pal = 0, acc_vsync = 0, acc_tot = 0;
    int nf = 0;
    while(!s_quit) {
        pump_touch(1);
        g_rot_us = 0; g_pal_us = 0; g_vsync_us = 0;        /* reset; set inside the tick */
        uint64_t a = now_us();
        doomgeneric_Tick();
        uint64_t tot = now_us() - a;
        uint64_t known = g_rot_us + g_pal_us + g_vsync_us;
        uint64_t game = (tot > known) ? tot - known : 0;

        acc_game += game; acc_rot += g_rot_us; acc_pal += g_pal_us;
        acc_vsync += g_vsync_us; acc_tot += tot;
        if(++nf >= 30) {
            unsigned long fps10 = acc_tot ? (unsigned long)(10000000ull * nf / acc_tot) : 0;
            printf("DOOM[%s] /frame: game=%lu rot=%lu pal=%lu vsync=%lu total=%lu us  fps=%lu.%lu\n",
                   s_double ? "double" : "single",
                   (unsigned long)(acc_game / nf), (unsigned long)(acc_rot / nf),
                   (unsigned long)(acc_pal / nf), (unsigned long)(acc_vsync / nf),
                   (unsigned long)(acc_tot / nf), fps10 / 10, fps10 % 10);
            fflush(stdout);
            acc_game = acc_rot = acc_pal = acc_vsync = acc_tot = 0; nf = 0;
        }
    }
    printf("=== DOOM exit ===\n");
    return 0;
}
