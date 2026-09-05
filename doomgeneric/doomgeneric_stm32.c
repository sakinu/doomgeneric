#include "doomkeys.h"
#include "doomgeneric.h"
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

extern unsigned long systick_cnt;
static int fb_fd = -1;
static uint16_t *fb_ptr = NULL;
static unsigned long fb_size = 0;
static struct fb_var_screeninfo vinfo;
struct fb_fix_screeninfo finfo;

extern uint16_t* DG_ScreenBuffer;

void DG_Init()
{
    printf("DG_Init\r\n");
    if ((fb_fd = open("/dev/fb0", O_RDWR)) < 0) {
        printf("DG_Init: Can not open /dev/fb0\n");
        exit(1);
    }

    
    ∂(fb_fd, FBIOGET_VSCREENINFO, &vinfo);
    ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo);

    fb_size = vinfo.xres * vinfo.yres * 2;
    printf("xres: %u, yres: %u\n", vinfo.xres, vinfo.yres);
    printf("line_length: %u\n", finfo.line_length);
    fb_ptr = (uint16_t *)mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);

    DG_ScreenBuffer = fb_ptr;
    if (fb_ptr == MAP_FAILED) {
        printf("DG_Init: mmap failed\n");
        close(fb_fd);
        exit(1);
    }
}

void DG_DrawFrame()
{
    // for (int i = 0; i < DOOMGENERIC_RESY / 2; i++) {
    //     for (int j = 0; j < DOOMGENERIC_RESX / 2; j++) {
    //         fb_ptr[j * finfo.line_length / 2 + (-40 + finfo.line_length / 2 - 1 - i)] = rgb888_to_rgb565(((uint32_t *)DG_ScreenBuffer)[i * 2 * DOOMGENERIC_RESX + j * 2]);
    //     }
    // }
    // printf("DG_DrawFrame not implemented\r\n");
    // while (1);
}

void DG_SleepMs(uint32_t ms)
{
    usleep(ms * 100);
}

uint32_t DG_GetTicksMs()
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;

    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int up = 0, down = 0, left = 0, right = 0, enter = 0, fire = 0;

int DG_GetKey(int* pressed, unsigned char* doomKey)
{
    // printf("DG_GetKey not implemented\n");
    return 0;
}

void DG_SetWindowTitle(const char * title)
{
    printf("DG_SetWindowTitle not implemented\n");
}

int main(int argc, char **argv)
{
    doomgeneric_Create(argc, argv);
    while(1)
    {
      doomgeneric_Tick(); 
    }

    return 0;
}
