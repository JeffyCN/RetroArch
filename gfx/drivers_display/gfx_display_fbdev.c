/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2019 - Jeffy Chen
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

#include <retro_miscellaneous.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include "../gfx_display.h"

#include "../../retroarch.h"
#include "../font_driver.h"

static const float *gfx_display_fbdev_get_default_vertices(void) { return NULL; }
static const float *gfx_display_fbdev_get_default_tex_coords(void) { return NULL; }
static void *gfx_display_fbdev_get_default_mvp(void *data) { return NULL; }

static void gfx_display_fbdev_blend_begin(void *data) { }
static void gfx_display_fbdev_blend_end(void *data) { }
static void gfx_display_fbdev_viewport(gfx_display_ctx_draw_t *draw,
      void *data) { }

static void gfx_display_fbdev_draw(gfx_display_ctx_draw_t *draw,
      void *data, unsigned video_width, unsigned video_height) { }

static void gfx_display_fbdev_draw_pipeline(gfx_display_ctx_draw_t *draw,
      void *data, unsigned video_width, unsigned video_height) { }

static void gfx_display_fbdev_restore_clear_color(void) { }

static void gfx_display_fbdev_clear_color(
      gfx_display_ctx_clearcolor_t *clearcolor,
      void *data)
{
   (void)clearcolor;
}

static bool gfx_display_fbdev_font_init_first(
      void **font_handle, void *video_data,
      const char *font_path, float font_size,
      bool is_threaded)
{
   font_data_t **handle = (font_data_t**)font_handle;
   *handle = font_driver_init_first(video_data,
         font_path, font_size, true,
         is_threaded,
         FONT_DRIVER_RENDER_FBDEV);
   return *handle;
}

gfx_display_ctx_driver_t gfx_display_ctx_fbdev = {
   gfx_display_fbdev_draw,
   gfx_display_fbdev_draw_pipeline,
   gfx_display_fbdev_viewport,
   gfx_display_fbdev_blend_begin,
   gfx_display_fbdev_blend_end,
   gfx_display_fbdev_restore_clear_color,
   gfx_display_fbdev_clear_color,
   gfx_display_fbdev_get_default_mvp,
   gfx_display_fbdev_get_default_vertices,
   gfx_display_fbdev_get_default_tex_coords,
   gfx_display_fbdev_font_init_first,
   GFX_VIDEO_DRIVER_FBDEV,
   "fbdev",
   true,
   NULL,
   NULL
};
