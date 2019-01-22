/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
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

#include <math.h>
#include <stdlib.h>
#include <encodings/utf.h>
#include <string/stdstring.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include "../font_driver.h"
#include "../common/fbdev_common.h"

#include "../../configuration.h"
#include "../../verbosity.h"

#define FBDEV_COLOR_A(col) (((col) >> 24) & 0xff)
#define FBDEV_COLOR_R(col) (((col) >> 16) & 0xff)
#define FBDEV_COLOR_G(col) (((col) >>  8) & 0xff)
#define FBDEV_COLOR_B(col) (((col) >>  0) & 0xff)

#define FBDEV_COLOR_ARGB(a, r, g, b) \
   (((unsigned)(a) << 24) | ((r) << 16) | ((g) << 8) | ((b) << 0))

#define FBDEV_BLEND_CHANNEL(blend, c1, c2) \
   (((255 - (blend)) * (c1) + (blend) * (c2)) >> 8)

#define FBDEV_BLEND_ARGB8888(blend, color, r, g, b) \
   FBDEV_COLOR_ARGB(FBDEV_COLOR_A(color), \
         FBDEV_BLEND_CHANNEL(blend, FBDEV_COLOR_R(color), r), \
         FBDEV_BLEND_CHANNEL(blend, FBDEV_COLOR_G(color), g), \
         FBDEV_BLEND_CHANNEL(blend, FBDEV_COLOR_B(color), b))

typedef struct
{
   const font_renderer_driver_t *font_driver;
   void *font_data;
   struct font_atlas* atlas;
} fbdev_font_t;

static void *fbdev_init_font(void *data,
      const char *font_path, float font_size,
      bool is_threaded)
{
   fbdev_font_t *font = calloc(1, sizeof(*font));

   if (!font)
      return NULL;

   if (!font_renderer_create_default(
            &font->font_driver,
            &font->font_data, font_path, font_size))
   {
      RARCH_WARN("Couldn't initialize font renderer.\n");
      free(font);
      return NULL;
   }

   font->atlas = font->font_driver->get_atlas(font->font_data);

   return font;
}

static void fbdev_render_free_font(void *data, bool is_threaded)
{
   (void)is_threaded;
   fbdev_font_t *font = (fbdev_font_t*)data;

   if (!font)
      return;

   if (font->font_driver && font->font_data &&
         font->font_driver->free)
      font->font_driver->free(font->font_data);

   free(font);
}

static int fbdev_get_message_width(void *data, const char *msg,
      unsigned msg_len, float scale)
{
   fbdev_font_t *font = (fbdev_font_t*)data;

   unsigned i;
   int delta_x = 0;

   if (!font)
      return 0;

   for (i = 0; i < msg_len; i++)
   {
      const struct font_glyph *glyph;
      const char* msg_tmp = &msg[i];
      unsigned    code    = utf8_walk(&msg_tmp);
      unsigned    skip    = msg_tmp - &msg[i];

      if (skip > 1)
         i += skip - 1;

      glyph = font->font_driver->get_glyph(font->font_data, code);

      if (!glyph) /* Do something smarter here ... */
         glyph = font->font_driver->get_glyph(font->font_data, '?');

      if (!glyph)
         continue;

      delta_x += glyph->advance_x;
   }

   return delta_x * scale;
}

static inline void fbdev_blend_pixel(unsigned blend,
      void *dst, unsigned bpp, unsigned r, unsigned g, unsigned b)
{
   unsigned color;
   unsigned *ptr;

   if (!blend)
      return;

   switch (bpp) {
      case 2:
         if (blend == 255) {
            color = FBDEV_COLOR_ARGB(0xff, r, g, b);
         } else {
            conv_rgb565_argb8888(&color, dst, 1, 1, 4, 2);
            color = FBDEV_BLEND_ARGB8888(blend, color, r, g, b);
         }
         conv_argb8888_rgb565(dst, &color, 1, 1, 2, 4);
         break;
      case 4:
         ptr = (unsigned *)dst;
         if (blend == 255)
            *ptr = FBDEV_COLOR_ARGB(0xff, r, g, b);
         else
            *ptr = FBDEV_BLEND_ARGB8888(blend, *ptr, r, g, b);
         break;
      default:
         return;
   }

   return;
}

static void fbdev_blend_glyph(fbdev_video_t *fbdev,
      const uint8_t *src, unsigned color,
      unsigned width, unsigned height,
      unsigned x, unsigned y)
{
   unsigned i, j;
   unsigned r, g, b;
   unsigned dst_pitch = fbdev->width * fbdev->bpp;
   void *dst, *p;

   if (fbdev->pending_page)
      dst = fbdev->pending_page->buf + (y * fbdev->width + x) * fbdev->bpp;
   else
      dst = fbdev->dummy_page->buf + (y * fbdev->width + x) * fbdev->bpp;

   for (i = 0; i < height; i++, src += width, dst += dst_pitch)
      for (j = 0, p = dst; j < width; j++, p += fbdev->bpp)
         fbdev_blend_pixel(src[j], p, fbdev->bpp,
               FONT_COLOR_GET_RED(color),
               FONT_COLOR_GET_GREEN(color),
               FONT_COLOR_GET_BLUE(color));
}

void fbdev_gfx_draw_text(
      fbdev_video_t *fbdev,
      fbdev_font_t *font,
      const struct font_atlas *atlas, void *font_data,
      const char *msg, float scale, unsigned color, int x, int y)
{
   int max_width, max_height;

   if (!fbdev || !font || !atlas || !font_data ||
         string_is_empty(msg))
      return;

   FBDEV_DEBUG(TEXT, "Draw text at (%d,%d): %s\n", x, y, msg);

   for (; *msg; msg++)
   {
      int i, j;
      int base_x, base_y;
      int glyph_width, glyph_height;
      const uint8_t *src;
      uint8_t *buf;
      const struct font_glyph *glyph =
         font->font_driver->get_glyph(font_data, (uint8_t)*msg);

      if (!glyph) /* Do something smarter here ... */
         glyph = font->font_driver->get_glyph(font_data, '?');

      if (!glyph)
         continue;

      base_x = x + glyph->draw_offset_x * scale;
      base_y = y + glyph->draw_offset_y * scale;

      max_width = fbdev->width - base_x;
      max_height = fbdev->height - base_y;
      if (max_width <= 0 || max_height <= 0)
         continue;

      glyph_width = glyph->width * scale;
      glyph_height = glyph->height * scale;

      // Scale glyph buffer
      buf = malloc(glyph_width * glyph_height);
      src = atlas->buffer + glyph->atlas_offset_x +
         glyph->atlas_offset_y * atlas->width;
      for (i = 0; i < glyph_height; i++) {
         for (j = 0; j < glyph_width; j++) {
            int offset = (i * atlas->width + j) / scale;
            buf[i * glyph_width + j] = src[offset];
         }
      }
      src = buf;

      if (base_x < 0)
      {
         src -= base_x;
         glyph_width += base_x;
         base_x = 0;
      }

      if (base_y < 0)
      {
         src -= base_y * (int)atlas->width;
         glyph_height += base_y;
         base_y = 0;
      }

      if (glyph_width > max_width)
         glyph_width = max_width;
      if (glyph_height > max_height)
         glyph_height = max_height;

      fbdev_blend_glyph(fbdev, src, color,
            glyph_width, glyph_height, base_x, base_y);

      free(buf);

      x += glyph->advance_x * scale;
      y += glyph->advance_y * scale;
   }
}

static void fbdev_render_line(
      fbdev_video_t *fbdev,
      fbdev_font_t *font, const char *msg, unsigned msg_len,
      float scale, const unsigned int color, float pos_x,
      float pos_y,
      unsigned width, unsigned height, unsigned text_align)
{
   unsigned i;
   int x            = roundf(pos_x * width);
   int y            = roundf((1.0 - pos_y) * height);

   switch (text_align)
   {
      case TEXT_ALIGN_RIGHT:
         x -= fbdev_get_message_width(font, msg, msg_len, scale);
         break;

      case TEXT_ALIGN_CENTER:
         x -= fbdev_get_message_width(font, msg, msg_len, scale) / 2;
         break;
      default:
         break;
   }

   fbdev_gfx_draw_text(fbdev, font, font->atlas,
         font->font_data, msg, scale, color, x, y);
}

static void fbdev_render_message(
      fbdev_video_t *fbdev,
      fbdev_font_t *font, const char *msg, float scale,
      const unsigned int color, float pos_x, float pos_y,
      unsigned width, unsigned height, unsigned text_align)
{
   struct font_line_metrics *line_metrics = NULL;
   int lines = 0;
   float line_height;

   /* If font line metrics are not supported just draw as usual */
   if (!font->font_driver->get_line_metrics ||
       !font->font_driver->get_line_metrics(font->font_data, &line_metrics))
   {
      fbdev_render_line(fbdev, font, msg, strlen(msg),
            scale, color, pos_x, pos_y,
            width, height, text_align);
      return;
   }

   line_height = scale / line_metrics->height;

   for (;;)
   {
      const char* delim = strchr(msg, '\n');

      /* Draw the line */
      if (delim)
      {
         unsigned msg_len = delim - msg;
         fbdev_render_line(fbdev, font, msg, msg_len,
               scale, color, pos_x, pos_y - (float)lines * line_height,
               width, height, text_align);
         msg += msg_len + 1;
         lines++;
      }
      else
      {
         unsigned msg_len = strlen(msg);
         fbdev_render_line(fbdev, font, msg, msg_len,
               scale, color, pos_x, pos_y - (float)lines * line_height,
               width, height, text_align);
         break;
      }
   }
}

static void fbdev_render_msg(
      void *userdata,
      void *data, const char *msg,
      const struct font_params *params)
{
   float x, y, scale, drop_mod, drop_alpha;
   int drop_x, drop_y;
   enum text_alignment text_align;
   unsigned color, color_dark, r, g, b,
            alpha, r_dark, g_dark, b_dark, alpha_dark;
   fbdev_font_t *font = (fbdev_font_t*)data;
   fbdev_video_t *fbdev = (fbdev_video_t*)userdata;
   unsigned width = fbdev->width;
   unsigned height = fbdev->height;
   settings_t *settings             = config_get_ptr();
   float video_msg_pos_x            = settings->floats.video_msg_pos_x;
   float video_msg_pos_y            = settings->floats.video_msg_pos_y;
   float video_msg_color_r          = settings->floats.video_msg_color_r;
   float video_msg_color_g          = settings->floats.video_msg_color_g;
   float video_msg_color_b          = settings->floats.video_msg_color_b;

   if (!font || string_is_empty(msg))
      return;

   if (params)
   {
      x              = params->x;
      y              = params->y;
      scale          = params->scale;
      text_align     = params->text_align;
      drop_x         = params->drop_x;
      drop_y         = params->drop_y;
      drop_mod       = params->drop_mod;
      drop_alpha     = params->drop_alpha;

      r              = FONT_COLOR_GET_RED(params->color);
      g              = FONT_COLOR_GET_GREEN(params->color);
      b              = FONT_COLOR_GET_BLUE(params->color);
      alpha          = FONT_COLOR_GET_ALPHA(params->color);
      color          = params->color;
   }
   else
   {
      x              = video_msg_pos_x;
      y              = video_msg_pos_y;
      scale          = 1.0f;
      text_align     = TEXT_ALIGN_LEFT;

      r              = (video_msg_color_r * 255);
      g              = (video_msg_color_g * 255);
      b              = (video_msg_color_b * 255);
      alpha          = 255;
      color          = FONT_COLOR_RGBA(r, g, b, alpha);

      drop_x         = -2;
      drop_y         = -2;
      drop_mod       = 0.3f;
      drop_alpha     = 1.0f;
   }

   if (drop_x || drop_y)
   {
      r_dark         = r * drop_mod;
      g_dark         = g * drop_mod;
      b_dark         = b * drop_mod;
      alpha_dark     = alpha * drop_alpha;
      color_dark     = FONT_COLOR_RGBA(r_dark, g_dark, b_dark, alpha_dark);

      fbdev_render_message(fbdev, font, msg, scale, color_dark,
            x + scale * drop_x / width, y +
            scale * drop_y / height,
            width, height, text_align);
   }

   fbdev_render_message(fbdev, font, msg, scale,
         color, x, y,
         width, height, text_align);
}

static const struct font_glyph *fbdev_font_get_glyph(
      void *data, uint32_t code)
{
   fbdev_font_t *font = (fbdev_font_t*)data;

   if (!font || !font->font_driver)
      return NULL;

   if (!font->font_driver->ident)
      return NULL;

   return font->font_driver->get_glyph((void*)font->font_driver, code);
}

static bool fbdev_font_get_line_metrics(void* data, struct font_line_metrics **metrics)
{
   fbdev_font_t* font = (fbdev_font_t*)data;

   if (!font || !font->font_driver || !font->font_data)
      return -1;

   return font->font_driver->get_line_metrics(font->font_data, metrics);
}

font_renderer_t fbdev_font =
{
   fbdev_init_font,
   fbdev_render_free_font,
   fbdev_render_msg,
   "fbdevfont",
   fbdev_font_get_glyph,
   NULL,                     /* bind_block */
   NULL,                     /* flush */
   fbdev_get_message_width,
   fbdev_font_get_line_metrics
};
