/*  RetroArch - A frontend for libretro.
 *  copyright (c) 2019 - Jeffy Chen
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef FBDEV_COMMON_H
#define FBDEV_COMMON_H

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include <linux/fb.h>
#include <gfx/scaler/scaler.h>
#include <gfx/scaler/pixconv.h>

#define FBDEV_LOG_TAG "[FBDEV]: "

#define FBDEV_LOG(...) RARCH_LOG(FBDEV_LOG_TAG __VA_ARGS__)
#define FBDEV_ERR(...) RARCH_ERR(FBDEV_LOG_TAG __VA_ARGS__)

#define FBDEV_DEBUG(TAG, ...) \
   if (FBDEV_DEBUG_##TAG) FBDEV_LOG(#TAG ": " __VA_ARGS__)

#define FBDEV_DEBUG_ENTER 0
#define FBDEV_DEBUG_EXIT 0
#define FBDEV_DEBUG_SCALE 0
#define FBDEV_DEBUG_FRAME 0
#define FBDEV_DEBUG_MSG 0
#define FBDEV_DEBUG_PERF 0
#define FBDEV_DEBUG_TEXT 0

#define FBDEV_MAX_PAGES 3

typedef struct fbdevb_page
{
   void *buf;
   unsigned offset;
   bool used;
} fbdev_page_t;

typedef struct fbdev_data
{
   int fd;
   struct fb_var_screeninfo vinfo;

   void *fbmem;
   unsigned fbmem_size;

   fbdev_page_t pages[FBDEV_MAX_PAGES];
   unsigned num_pages;

   fbdev_page_t *curr_page;
   fbdev_page_t *pending_page;
   fbdev_page_t *menu_page;
   fbdev_page_t *dummy_page;

   /* screen size and bytes per pixel */
   unsigned width, height, bpp;
   /* video size and bytes per pixel */
   unsigned video_width, video_height, video_bpp;
   unsigned page_size;

   struct scaler_ctx scaler;

#if FBDEV_DEBUG_PERF
   retro_time_t last_frame_time;
#endif

   bool sync;
   bool menu_active;
} fbdev_video_t;

#endif // FBDEV_COMMON_H
