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

#include "i_video.h"
#include "config.h"
#include "d_event.h"
#include "d_main.h"
#include "m_argv.h"
#include "v_video.h"
#include "z_zone.h"

#include "tables.h"
#include "doomkeys.h"

#include "doomgeneric.h"
#include "hu_stuff.h"
#include "i_swap.h"

#include <stdbool.h>
#include <ctype.h>
#include <gpuintrin.h>
#include <stdlib.h>
#include <stdio.h>

extern patch_t *hu_font[HU_FONTSIZE];

#if !defined(__AMDGPU__) && !defined(__NVPTX__)
#include <fcntl.h>
#include <sys/types.h>
#endif

#include <stdarg.h>

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
    int i, j, k;
    struct color c;
    uint32_t pix;
    uint16_t r, g, b;

    uint32_t thrd_id = __gpu_thread_id(0);
    uint32_t thrd_count = __gpu_num_threads(0);
    for (i = 0; i < in_pixels; i += thrd_count)
    {
        int local_id = i + thrd_id;
        if (local_id >= in_pixels)
            break;
        uint8_t *in_current = in + local_id;
        c = colors[*in_current]; /* R:8 G:8 B:8 format! */
        r = (uint16_t) (c.r >> (8 - s_Fb.red.length));
        g = (uint16_t) (c.g >> (8 - s_Fb.green.length));
        b = (uint16_t) (c.b >> (8 - s_Fb.blue.length));
        pix = r << s_Fb.red.offset;
        pix |= g << s_Fb.green.offset;
        pix |= b << s_Fb.blue.offset;

        uint32_t bytes_pp = s_Fb.bits_per_pixel / 8;
        uint8_t *out_current = out + local_id * fb_scaling * bytes_pp;
        for (k = 0; k < fb_scaling; k++)
        {
            for (j = 0; j < bytes_pp; j++)
            {
                *out_current = (pix >> (j * 8));
                out_current++;
            }
        }
    }
}

// Single-thread version: one thread processes an entire row.
// Used by the 2D-parallel I_FinishUpdate where each thread owns full rows.
void cmap_to_fb_single(uint8_t *out, uint8_t *in, int in_pixels)
{
    int i, j, k;
    struct color c;
    uint32_t pix;
    uint16_t r, g, b;
    uint32_t bytes_pp = s_Fb.bits_per_pixel / 8;

    for (i = 0; i < in_pixels; i++)
    {
        c = colors[in[i]];
        r = (uint16_t) (c.r >> (8 - s_Fb.red.length));
        g = (uint16_t) (c.g >> (8 - s_Fb.green.length));
        b = (uint16_t) (c.b >> (8 - s_Fb.blue.length));
        pix = r << s_Fb.red.offset;
        pix |= g << s_Fb.green.offset;
        pix |= b << s_Fb.blue.offset;

        uint8_t *out_current = out + i * fb_scaling * bytes_pp;
        for (k = 0; k < fb_scaling; k++)
        {
            for (j = 0; j < bytes_pp; j++)
            {
                *out_current = (pix >> (j * 8));
                out_current++;
            }
        }
    }
}

void I_InitGraphics (void)
{
    int i;

	memset(&s_Fb, 0, sizeof(struct FB_ScreenInfo));
	s_Fb.xres = DOOMGENERIC_RESX;
	s_Fb.yres = DOOMGENERIC_RESY;
	s_Fb.xres_virtual = s_Fb.xres;
	s_Fb.yres_virtual = s_Fb.yres;

#ifdef CMAP256

	s_Fb.bits_per_pixel = 8;

#else  // CMAP256

	s_Fb.bits_per_pixel = 32;

	s_Fb.blue.length = 8;
	s_Fb.green.length = 8;
	s_Fb.red.length = 8;
	s_Fb.transp.length = 8;

	s_Fb.blue.offset = 0;
	s_Fb.green.offset = 8;
	s_Fb.red.offset = 16;
	s_Fb.transp.offset = 24;
	
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

// ---------------------------------------------------------------------------
// On-screen performance overlay
// ---------------------------------------------------------------------------

int fps_overlay = 1;       // 0=off, 1=fps only, 2=advanced telemetry

static uint32_t fps_frame_count;
static uint32_t fps_last_time;
static uint32_t fps_last_tick_start;
static uint32_t fps_display_fps;
static uint32_t fps_display_frametime;  // in tenths of ms
static uint32_t fps_display_ticktime;   // in tenths of ms
static uint32_t fps_tick_accum;         // accumulated tick time for averaging
static uint32_t fps_tick_samples;

void I_FPS_TickStart(void)
{
    fps_last_tick_start = DG_GetTicksMs();
}

void I_FPS_TickEnd(void)
{
    if (fps_last_tick_start) {
        uint32_t elapsed = DG_GetTicksMs() - fps_last_tick_start;
        fps_tick_accum += elapsed;
        fps_tick_samples++;
    }
}

static void I_FPS_Update(void)
{
    uint32_t now = DG_GetTicksMs();

    fps_frame_count++;

    if (fps_last_time == 0) {
        fps_last_time = now;
        return;
    }

    uint32_t elapsed = now - fps_last_time;
    if (elapsed >= 500) {
        fps_display_fps = (fps_frame_count * 10000) / (elapsed * 10);
        fps_display_frametime = (elapsed * 10) / fps_frame_count;
        if (fps_tick_samples > 0)
            fps_display_ticktime = (fps_tick_accum * 10) / fps_tick_samples;
        else
            fps_display_ticktime = 0;
        fps_frame_count = 0;
        fps_last_time = now;
        fps_tick_accum = 0;
        fps_tick_samples = 0;
    }
}

static int I_FPS_StringWidth(const char *s)
{
    int w = 0;
    while (*s) {
        int c = toupper((int)*s++) - HU_FONTSTART;
        if (c < 0 || c >= HU_FONTSIZE)
            w += 4;
        else
            w += SHORT(hu_font[c]->width);
    }
    return w;
}

static void I_FPS_DrawString(int x, int y, const char *s)
{
    while (*s) {
        int c = toupper((int)*s++) - HU_FONTSTART;
        if (c < 0 || c >= HU_FONTSIZE) {
            x += 4;
            continue;
        }
        if (x + SHORT(hu_font[c]->width) > SCREENWIDTH)
            break;
        V_DrawPatchDirect(x, y, hu_font[c]);
        x += SHORT(hu_font[c]->width);
    }
}

void I_FPS_Drawer(void)
{
    if (!fps_overlay || !hu_font[0])
        return;

    I_FPS_Update();

    if (fps_display_fps == 0 && fps_display_frametime == 0)
        return;

    char buf[64];
    int y = 2;
    int x;

    snprintf(buf, sizeof(buf), "%u FPS", fps_display_fps);
    x = SCREENWIDTH - I_FPS_StringWidth(buf) - 2;
    I_FPS_DrawString(x, y, buf);

    if (fps_overlay >= 2) {
        y += 10;
        snprintf(buf, sizeof(buf), "%u.%u MS",
                 fps_display_frametime / 10, fps_display_frametime % 10);
        x = SCREENWIDTH - I_FPS_StringWidth(buf) - 2;
        I_FPS_DrawString(x, y, buf);

        y += 10;
        snprintf(buf, sizeof(buf), "TICK %u.%u MS",
                 fps_display_ticktime / 10, fps_display_ticktime % 10);
        x = SCREENWIDTH - I_FPS_StringWidth(buf) - 2;
        I_FPS_DrawString(x, y, buf);
    }
}

//
// I_FinishUpdate
//

void I_FinishUpdate ()
{
    int x_offset, x_offset_end;

    uint32_t bytes_pp = s_Fb.bits_per_pixel / 8;
    x_offset     = (((s_Fb.xres - (SCREENWIDTH  * fb_scaling)) * bytes_pp)) / 2;
    x_offset_end = ((s_Fb.xres - (SCREENWIDTH  * fb_scaling)) * bytes_pp) - x_offset;
    int out_stride = SCREENWIDTH * fb_scaling * bytes_pp + x_offset + x_offset_end;

    unsigned char *base_in  = (unsigned char *) I_VideoBuffer;
    unsigned char *base_out = (unsigned char *) DG_ScreenBuffer;

    int total_out_rows = SCREENHEIGHT * fb_scaling;
    uint32_t tid = __gpu_thread_id(0);
    uint32_t nthreads = __gpu_num_threads(0);

    for (int row = tid; row < total_out_rows; row += nthreads) {
        int src_y = row / fb_scaling;
        unsigned char *line_in = base_in + src_y * SCREENWIDTH;
        unsigned char *line_out = base_out + row * out_stride + x_offset;
        cmap_to_fb_single(line_out, line_in, SCREENWIDTH);
    }

    __gpu_sync_threads();
    if (tid == 0)
      DG_DrawFrame();
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

void I_SetPalette (byte* palette)
{
	int i;
	//col_t* c;

	//for (i = 0; i < 256; i++)
	//{
	//	c = (col_t*)palette;

	//	rgb565_palette[i] = GFX_RGB565(gammatable[usegamma][c->r],
	//								   gammatable[usegamma][c->g],
	//								   gammatable[usegamma][c->b]);

	//	palette += 3;
	//}
    

    /* performance boost:
     * map to the right pixel format over here! */

    for (i=0; i<256; ++i ) {
        colors[i].a = 0;
        colors[i].r = gammatable[usegamma][*palette++];
        colors[i].g = gammatable[usegamma][*palette++];
        colors[i].b = gammatable[usegamma][*palette++];
    }

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
