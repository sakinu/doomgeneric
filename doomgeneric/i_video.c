// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// $Log:$
//
// DESCRIPTION:
//	DOOM graphics stuff for X11, UNIX.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: i_x.c,v 1.6 1997/02/03 22:45:10 b1 Exp $";

#include "config.h"
#include "v_video.h"
#include "m_argv.h"
#include "d_event.h"
#include "d_main.h"
#include "i_video.h"
#include "i_system.h"
#include "z_zone.h"

#include "tables.h"
#include "doomkeys.h"

#include "doomgeneric.h"

#include <stdbool.h>
#include <stdlib.h>

#include <fcntl.h>

#include <stdarg.h>

#include <sys/types.h>

//#define CMAP256

struct FB_BitField
{
	uint32_t offset;			/* beginning of bitfield	*/
	uint32_t length;			/* length of bitfield		*/
};

struct FB_ScreenInfo
{
	uint32_t xres;			/* visible resolution		*/
	uint32_t yres;
	uint32_t xres_virtual;		/* virtual resolution		*/
	uint32_t yres_virtual;

	uint32_t bits_per_pixel;		/* guess what			*/
	
							/* >1 = FOURCC			*/
	struct FB_BitField red;		/* bitfield in s_Fb mem if true color, */
	struct FB_BitField green;	/* else only length is significant */
	struct FB_BitField blue;
	struct FB_BitField transp;	/* transparency			*/
};

static struct FB_ScreenInfo s_Fb;
int fb_scaling = 1;
int usemouse = 0;


#ifdef CMAP256

boolean palette_changed;
struct color colors[256];

#else  // CMAP256

static struct color colors[256];


#endif  // CMAP256

void I_GetEvent(void);

// The screen buffer; this is modified to draw things to the screen

byte *I_VideoBuffer = NULL;

// If true, game is running as a screensaver

boolean screensaver_mode = false;

// Flag indicating whether the screen is currently visible:
// when the screen isnt visible, don't render the screen

boolean screenvisible;

// Mouse acceleration
//
// This emulates some of the behavior of DOS mouse drivers by increasing
// the speed when the mouse is moved fast.
//
// The mouse input values are input directly to the game, but when
// the values exceed the value of mouse_threshold, they are multiplied
// by mouse_acceleration to increase the speed.

float mouse_acceleration = 2.0;
int mouse_threshold = 10;

// Gamma correction level to use

int usegamma = 0;

typedef struct
{
	byte r;
	byte g;
	byte b;
} col_t;

// Palette converted to RGB565

static uint16_t rgb565_palette[256];

void cmap_to_rgb565(uint16_t * out, uint8_t * in, int in_pixels)
{
    int i, j;
    struct color c;
    uint16_t r, g, b;

    for (i = 0; i < in_pixels; i++)
    {
        c = colors[*in]; 
        r = ((uint16_t)(c.r >> 3)) << 11;
        g = ((uint16_t)(c.g >> 2)) << 5;
        b = ((uint16_t)(c.b >> 3)) << 0;
        *out = (r | g | b);

        in++;
        for (j = 0; j < fb_scaling; j++) {
            out++;
        }
    }
}

void cmap_to_fb(uint8_t *out, uint8_t *in, int in_pixels)
{
    int i, k;
    struct color c;
    uint32_t pix;

    for (i = 0; i < in_pixels; i++)
    {
        c = colors[*in];  // R:8 G:8 B:8

        if (s_Fb.bits_per_pixel == 16)
        {
            // RGB565 packing
            uint16_t p = ((c.r & 0xF8) << 8) |
                         ((c.g & 0xFC) << 3) |
                         (c.b >> 3);

#ifdef SYS_BIG_ENDIAN
            p = swapeLE16(p); // can't use SHORT() because this needs to stay unsigned
#endif
            for (k = 0; k < fb_scaling; k++) {
                *(uint16_t *)out = p;
                out += 2;
            }
        }
        else if (s_Fb.bits_per_pixel == 32)
        {
            // Assuming RGBA8888
            pix = (c.r << s_Fb.red.offset) |
                  (c.g << s_Fb.green.offset) |
                  (c.b << s_Fb.blue.offset);

#ifdef SYS_BIG_ENDIAN
            pix = swapLE32(pix);
#endif
            for (k = 0; k < fb_scaling; k++) {
                *(uint32_t *)out = pix;
                out += 4;
            }
        }
        else {
            // no clue how to convert this
            I_Error("No idea how to convert %d bpp pixels", s_Fb.bits_per_pixel);
        }

        in++;
    }
}

void I_InitGraphics (void)
{
    int i, gfxmodeparm;
    char *mode;

	memset(&s_Fb, 0, sizeof(struct FB_ScreenInfo));
	s_Fb.xres = DOOMGENERIC_RESX;
	s_Fb.yres = DOOMGENERIC_RESY;
	s_Fb.xres_virtual = s_Fb.xres;
	s_Fb.yres_virtual = s_Fb.yres;

#ifdef CMAP256

	s_Fb.bits_per_pixel = 8;

#else  // CMAP256

	gfxmodeparm = M_CheckParmWithArgs("-gfxmode", 1);

	if (gfxmodeparm) {
		mode = myargv[gfxmodeparm + 1];
	}
	else {
		// default to rgba8888 like the old behavior, for compatibility
		// maybe could warn here?
		mode = "rgba8888";
	}

	if (strcmp(mode, "rgba8888") == 0) {
		// default mode
		s_Fb.bits_per_pixel = 32;

		s_Fb.blue.length = 8;
		s_Fb.green.length = 8;
		s_Fb.red.length = 8;
		s_Fb.transp.length = 8;

		s_Fb.blue.offset = 0;
		s_Fb.green.offset = 8;
		s_Fb.red.offset = 16;
		s_Fb.transp.offset = 24;
	}

	else if (strcmp(mode, "rgb565") == 0) {
		s_Fb.bits_per_pixel = 16;

		s_Fb.blue.length = 5;
		s_Fb.green.length = 6;
		s_Fb.red.length = 5;
		s_Fb.transp.length = 0;

		s_Fb.blue.offset = 11;
		s_Fb.green.offset = 5;
		s_Fb.red.offset = 0;
		s_Fb.transp.offset = 16;
	}
	else
		I_Error("Unknown gfxmode value: %s\n", mode);


#endif  // CMAP256

    printf("I_InitGraphics: framebuffer: x_res: %d, y_res: %d, x_virtual: %d, y_virtual: %d, bpp: %d\n",
            s_Fb.xres, s_Fb.yres, s_Fb.xres_virtual, s_Fb.yres_virtual, s_Fb.bits_per_pixel);

    printf("I_InitGraphics: framebuffer: RGBA: %d%d%d%d, red_off: %d, green_off: %d, blue_off: %d, transp_off: %d\n",
            s_Fb.red.length, s_Fb.green.length, s_Fb.blue.length, s_Fb.transp.length, s_Fb.red.offset, s_Fb.green.offset, s_Fb.blue.offset, s_Fb.transp.offset);

    printf("I_InitGraphics: DOOM screen size: w x h: %d x %d\n", SCREENWIDTH, SCREENHEIGHT);


    i = M_CheckParmWithArgs("-scaling", 1);
    if (i > 0) {
        i = atoi(myargv[i + 1]);
        fb_scaling = i;
        printf("I_InitGraphics: Scaling factor: %d\n", fb_scaling);
    } else {
        fb_scaling = s_Fb.xres / SCREENWIDTH;
        if (s_Fb.yres / SCREENHEIGHT < fb_scaling)
            fb_scaling = s_Fb.yres / SCREENHEIGHT;
        printf("I_InitGraphics: Auto-scaling factor: %d\n", fb_scaling);
    }


    /* Allocate screen to draw to */
	I_VideoBuffer = (byte*)Z_Malloc (SCREENWIDTH * SCREENHEIGHT, PU_STATIC, NULL);  // For DOOM to draw on

	screenvisible = true;

    extern void I_InitInput(void);
    I_InitInput();
}

void I_ShutdownGraphics (void)
{
	Z_Free (I_VideoBuffer);
}

void I_StartFrame (void)
{

}

void I_StartTic (void)
{
	I_GetEvent();
}

void I_UpdateNoBlit (void)
{
}

//
// I_FinishUpdate
//
#include <linux/fb.h>
#include <time.h>
extern struct fb_fix_screeninfo finfo;
extern void DG_DrawFrame(void);   /* backend present hook (page flip on DRM) */
extern uint64_t g_rot_us;         /* perf: rotation/copy loop time (us) */

int dg_x_offset = 20;   /* panel-column left margin of the rotated game (backend overrides) */

void I_FinishUpdate (void)
{
    uint8_t *line_in;
    uint8_t *line_out;
    line_in  = (uint8_t *) I_VideoBuffer;
    line_out = (uint8_t *) DG_ScreenBuffer;
    /* Rotate DOOM 320x200 -> 200x320 for the 240-wide portrait panel. Both axes
     * reversed vs. the naive map (the other 90-degree direction) so it isn't
     * upside-down. dg_x_offset = left margin in panel columns: 20 centres it
     * (standalone), 0 left-aligns it to [0..199] leaving [200..239] for buttons
     * (cellphone app). Backends set dg_x_offset in DG_Init.
     *
     * The panel buffer holds raw 8-bit palette indices (DRM_FORMAT_C8): the
     * LTDC's hardware CLUT does index -> RGB during scan-out (see I_SetPalette
     * / DG_SetPalette), so this loop only ever moves bytes -- no per-pixel
     * color conversion runs on the CPU. */
    const int stride = finfo.line_length;   /* bytes per panel row (1 byte/pixel) */
    struct timespec _ta, _tb;
    clock_gettime(CLOCK_MONOTONIC, &_ta);
    /* Iterate so the dumb-buffer WRITE is sequential (out[0..199] contiguous).
     * The 90-degree rotation otherwise strides writes by one panel row, which
     * kills write-combining on the WC-mapped framebuffer; here the strided
     * access lands on cached RAM (the read) instead. Output is identical.
     * Four input rows are grouped per outer step (was two, back when each
     * output pixel was a 2-byte RGB565 value) so each store is still a full
     * 32-bit write: four 1-byte palette indices now fill the word that two
     * 2-byte colors used to. */
    for (int i = 0; i < SCREENHEIGHT; i += 4) {
        uint32_t * line_in_ = (uint32_t *)(line_in + i * SCREENWIDTH);
        for (int j = 0; j << 2 < SCREENWIDTH; j++) {        // uint32 = 4*uint8
            /* Each *p_in load grabs 4 horizontally-adjacent source pixels
             * (row i+0..i+3) as one 32-bit word. Target is little-endian, so
             * the LOWEST-addressed pixel (col 4j+0) lands in the LOW byte of
             * the register, meaning the shift amount and the column offset
             * run opposite ways: inR_0 (>>24) is actually column 4j+3, and
             * inR_3 (no shift, just &255) is column 4j+0. That's why the
             * writes below pair suffix _3 with "col 4j+0" and suffix _0 with
             * "col 4j+3" -- reversed-looking, but it's what the load order
             * demands, not an off-by-one. */
            uint32_t *p_in = line_in_ + j;
            uint32_t in0_0 = (*p_in)>>24, in0_1 = (*p_in>>16)&255, in0_2 = (*p_in>>8)&255, in0_3 = (*p_in)&255;
            p_in += SCREENWIDTH >> 2;
            uint32_t in1_0 = (*p_in)>>24, in1_1 = (*p_in>>16)&255, in1_2 = (*p_in>>8)&255, in1_3 = (*p_in)&255;
            p_in += SCREENWIDTH >> 2;
            uint32_t in2_0 = (*p_in)>>24, in2_1 = (*p_in>>16)&255, in2_2 = (*p_in>>8)&255, in2_3 = (*p_in)&255;
            p_in += SCREENWIDTH >> 2;
            uint32_t in3_0 = (*p_in)>>24, in3_1 = (*p_in>>16)&255, in3_2 = (*p_in>>8)&255, in3_3 = (*p_in)&255;

            /* Each store below re-packs one byte from each of the 4 source
             * rows (one output column) into a single 32-bit word -- that's
             * the actual transpose step; the shift here just places row r's
             * byte at bit offset 8*r.
             *
             * It's tempting to fold the ">>N & 255" extraction above and the
             * "<<8*r" placement here into one combined shift per byte (e.g.
             * in1_1 = (*p_in >> 8) & 0xFF00 instead of ((*p_in>>16)&255)<<8)
             * -- the arithmetic is correct, but it doesn't help on this
             * target: ARM's barrel shifter already folds a "<<N" into the
             * very instruction that ORs it in (e.g. `orr r1, r1, r2, lsl
             * #24`), so the second shift here is already free. Pre-shifting
             * by hand instead loses the compiler's UBFX-based extraction
             * (shift+mask to 0..255 in one instruction) and measurably
             * produces MORE instructions, not fewer -- verified by compiling
             * both versions with this project's actual cross compiler
             * (arm-linux-gcc -O2 -mcpu=cortex-m4 -mthumb -mfdpic): the
             * current form is 43 instructions per inner-loop iteration (16
             * pixels), the "pre-shifted" rewrite is 47. Leave this as-is. */
            uint8_t * p_out = line_out + (SCREENWIDTH - 1 - 4*j) * stride + dg_x_offset + i;
            *(uint32_t *)(p_out            ) = (in3_3 << 24) | (in2_3 << 16) | (in1_3 << 8) | in0_3;  // col 4j+0
            *(uint32_t *)(p_out -   stride ) = (in3_2 << 24) | (in2_2 << 16) | (in1_2 << 8) | in0_2;  // col 4j+1
            *(uint32_t *)(p_out - 2*stride ) = (in3_1 << 24) | (in2_1 << 16) | (in1_1 << 8) | in0_1;  // col 4j+2
            *(uint32_t *)(p_out - 3*stride ) = (in3_0 << 24) | (in2_0 << 16) | (in1_0 << 8) | in0_0;  // col 4j+3
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &_tb);
    g_rot_us = (uint64_t)(_tb.tv_sec - _ta.tv_sec) * 1000000ull
             + (_tb.tv_nsec - _ta.tv_nsec) / 1000;

    DG_DrawFrame();          /* present the frame (page flip on the DRM backend) */
}

//
// I_ReadScreen
//
void I_ReadScreen (byte* scr)
{
    memcpy (scr, I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT);
}

//
// I_SetPalette
//
#define GFX_RGB565(r, g, b)			((((r & 0xF8) >> 3) << 11) | (((g & 0xFC) >> 2) << 5) | ((b & 0xF8) >> 3))
#define GFX_RGB565_R(color)			((0xF800 & color) >> 11)
#define GFX_RGB565_G(color)			((0x07E0 & color) >> 5)
#define GFX_RGB565_B(color)			(0x001F & color)

/* backend hook (doomgeneric_drm.c): push 256 gamma-corrected {r,g,b} triples
 * into the LTDC's hardware CLUT (via drmModeCrtcSetGamma). Index -> RGB then
 * happens entirely in hardware during scan-out -- I_FinishUpdate never has
 * to convert a pixel. */
extern void DG_SetPalette(const byte *rgb256x3);
extern uint64_t g_pal_us;   /* perf: I_SetPalette time, incl. the CLUT push (us) */

void I_SetPalette (byte* palette)
{
	int i;
	byte rgb256x3[256 * 3];
	struct timespec _pa, _pb;

    /* performance boost:
     * map to the right pixel format over here! */

    /* Same pattern as I_FinishUpdate's g_rot_us: time this function's own
     * work (palette/gamma build + the CLUT push), accumulated rather than
     * overwritten since I_SetPalette can fire more than once per tic (e.g.
     * a damage flash and a state-change reset landing in the same tic). */
    clock_gettime(CLOCK_MONOTONIC, &_pa);

    for (i=0; i<256; ++i ) {
        colors[i].a = 0;
        colors[i].r = gammatable[usegamma][*palette++];
        colors[i].g = gammatable[usegamma][*palette++];
        colors[i].b = gammatable[usegamma][*palette++];
        rgb256x3[i*3 + 0] = colors[i].r;
        rgb256x3[i*3 + 1] = colors[i].g;
        rgb256x3[i*3 + 2] = colors[i].b;
    }
    DG_SetPalette(rgb256x3);

    clock_gettime(CLOCK_MONOTONIC, &_pb);
    g_pal_us += (uint64_t)(_pb.tv_sec - _pa.tv_sec) * 1000000ull
              + (_pb.tv_nsec - _pa.tv_nsec) / 1000;

#ifdef CMAP256

    palette_changed = true;

#endif  // CMAP256
}

// Given an RGB value, find the closest matching palette index.

int I_GetPaletteIndex (int r, int g, int b)
{
    int best, best_diff, diff;
    int i;
    col_t color;

    printf("I_GetPaletteIndex\n");

    best = 0;
    best_diff = INT_MAX;

    for (i = 0; i < 256; ++i)
    {
    	color.r = GFX_RGB565_R(rgb565_palette[i]);
    	color.g = GFX_RGB565_G(rgb565_palette[i]);
    	color.b = GFX_RGB565_B(rgb565_palette[i]);

        diff = (r - color.r) * (r - color.r)
             + (g - color.g) * (g - color.g)
             + (b - color.b) * (b - color.b);

        if (diff < best_diff)
        {
            best = i;
            best_diff = diff;
        }

        if (diff == 0)
        {
            break;
        }
    }

    return best;
}

void I_BeginRead (void)
{
}

void I_EndRead (void)
{
}

void I_SetWindowTitle (char *title)
{
	DG_SetWindowTitle(title);
}

void I_GraphicsCheckCommandLine (void)
{
}

void I_SetGrabMouseCallback (grabmouse_callback_t func)
{
}

void I_EnableLoadingDisk(void)
{
}

void I_BindVideoVariables (void)
{
}

void I_DisplayFPSDots (boolean dots_on)
{
}

void I_CheckIsScreensaver (void)
{
}
