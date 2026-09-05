/* Minimal headless backend + harness for Phase-A feasibility testing:
 * cross-compile doomgeneric with the uClinux/FDPIC toolchain, run N ticks on
 * the board, and report RAM usage. No display, no input. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/fb.h>

#include "doomgeneric.h"

extern uint16_t * DG_ScreenBuffer;   /* doomgeneric.c defines it (NULL) */

/* This doomgeneric is STM32-customized: i_video.c's I_FinishUpdate writes a
 * rotated RGB565 image into DG_ScreenBuffer using finfo.line_length. Provide it
 * (240-wide RGB565 framebuffer stride) so the core links and renders. */
struct fb_fix_screeninfo finfo;
#define FB_W 240
#define FB_H 320

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull;
}

static void print_mem(const char * tag)
{
    FILE * f = fopen("/proc/self/status", "r");
    if(!f) { printf("[mem %s] (no /proc)\n", tag); return; }
    char line[128];
    long vmrss = -1, vmdata = -1, vmsize = -1;
    while(fgets(line, sizeof(line), f)) {
        if(!strncmp(line, "VmRSS:", 6))  sscanf(line + 6,  "%ld", &vmrss);
        else if(!strncmp(line, "VmData:", 7)) sscanf(line + 7, "%ld", &vmdata);
        else if(!strncmp(line, "VmSize:", 7)) sscanf(line + 7, "%ld", &vmsize);
    }
    fclose(f);
    printf("[mem %s] VmSize=%ldKB VmRSS=%ldKB VmData=%ldKB\n", tag, vmsize, vmrss, vmdata);
    fflush(stdout);
}

void DG_Init(void)
{
    /* The backend owns the frame buffer. i_video.c writes a rotated RGB565
     * image of size ~FB_W*FB_H uint16 into it, so size for that. */
    finfo.line_length = FB_W * 2;            /* 240px * 2 bytes/px */
    DG_ScreenBuffer = malloc(FB_W * FB_H * 2);
    printf("[DG_Init] screenbuf=%p (%dx%d RGB565 = %dKB), stride=%d\n",
           (void *)DG_ScreenBuffer, FB_W, FB_H, FB_W * FB_H * 2 / 1024,
           (int)finfo.line_length);
    fflush(stdout);
}

static unsigned long s_frames;
void DG_DrawFrame(void)
{
    s_frames++;
    if((s_frames % 35) == 0) { printf("[frame %lu]\n", s_frames); fflush(stdout); }
}

void DG_SleepMs(uint32_t ms) { usleep(ms * 1000); }
uint32_t DG_GetTicksMs(void) { return (uint32_t)now_ms(); }
int  DG_GetKey(int * pressed, unsigned char * key) { (void)pressed; (void)key; return 0; }
void DG_SetWindowTitle(const char * title) { (void)title; }

int main(int argc, char ** argv)
{
    int ticks = 300;
    /* allow "min <N> [doom args...]" to override tick count */
    if(argc > 1 && argv[1][0] >= '0' && argv[1][0] <= '9') {
        ticks = atoi(argv[1]);
        argv[1] = argv[0];
        argc--; argv++;
    }
    printf("=== doomgeneric Phase-A test: %d ticks, res %dx%d ===\n",
           ticks, DOOMGENERIC_RESX, DOOMGENERIC_RESY);
    print_mem("start");

    uint64_t t0 = now_ms();
    doomgeneric_Create(argc, argv);   /* loads WAD + inits; returns */
    printf("[create done in %llu ms]\n", (unsigned long long)(now_ms() - t0));
    print_mem("after-create");

    uint64_t tf = now_ms();
    for(int i = 0; i < ticks; i++) doomgeneric_Tick();
    uint64_t dt = now_ms() - tf;
    printf("[%d ticks in %llu ms = %.1f fps]\n", ticks,
           (unsigned long long)dt, dt ? (1000.0 * ticks / dt) : 0.0);
    print_mem("after-ticks");
    printf("=== done ===\n");
    return 0;
}
