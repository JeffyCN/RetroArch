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

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#ifdef HAVE_MENU
#include "../../menu/menu_driver.h"
#endif

#include <retro_inline.h>
#include <retro_assert.h>
#include <features/features_cpu.h>
#include <gfx/video_frame.h>
#include <string/stdstring.h>

#include "../font_driver.h"
#include "../common/fbdev_common.h"
#include "../../configuration.h"
#include "../../driver.h"
#include "../../frontend/frontend_driver.h"
#include "../../retroarch.h"
#include "../../verbosity.h"

#define FBDEV_KEEP_FBDEV_PARAMS
#define FBDEV_SCALE_WITH_ASPECT
//#define FBDEV_MAX_YRES_VIRTUAL 640
//#define FBDEV_PREFER_BIG_PAGE

static void print_vinfo(struct fb_var_screeninfo *vinfo)
{
   FBDEV_LOG("\txres: %d\n", vinfo->xres);
   FBDEV_LOG("\tyres: %d\n", vinfo->yres);
   FBDEV_LOG("\txres_virtual: %d\n", vinfo->xres_virtual);
   FBDEV_LOG("\tyres_virtual: %d\n", vinfo->yres_virtual);
   FBDEV_LOG("\txoffset: %d\n", vinfo->xoffset);
   FBDEV_LOG("\tyoffset: %d\n", vinfo->yoffset);
   FBDEV_LOG("\tbits_per_pixel: %d\n", vinfo->bits_per_pixel);
   FBDEV_LOG("\tgrayscale: %d\n", vinfo->grayscale);
   FBDEV_LOG("\tnonstd: %d\n", vinfo->nonstd);
   FBDEV_LOG("\tactivate: %d\n", vinfo->activate);
   FBDEV_LOG("\theight: %d\n", vinfo->height);
   FBDEV_LOG("\twidth: %d\n", vinfo->width);
   FBDEV_LOG("\taccel_flags: %d\n", vinfo->accel_flags);
   FBDEV_LOG("\tpixclock: %d\n", vinfo->pixclock);
   FBDEV_LOG("\tleft_margin: %d\n", vinfo->left_margin);
   FBDEV_LOG("\tright_margin: %d\n", vinfo->right_margin);
   FBDEV_LOG("\tupper_margin: %d\n", vinfo->upper_margin);
   FBDEV_LOG("\tlower_margin: %d\n", vinfo->lower_margin);
   FBDEV_LOG("\thsync_len: %d\n", vinfo->hsync_len);
   FBDEV_LOG("\tvsync_len: %d\n", vinfo->vsync_len);
   FBDEV_LOG("\tsync: %d\n", vinfo->sync);
   FBDEV_LOG("\tvmode: %d\n", vinfo->vmode);
   FBDEV_LOG("\tred: %d/%d\n", vinfo->red.length, vinfo->red.offset);
   FBDEV_LOG("\tgreen: %d/%d\n", vinfo->green.length, vinfo->green.offset);
   FBDEV_LOG("\tblue: %d/%d\n", vinfo->blue.length, vinfo->blue.offset);
   FBDEV_LOG("\talpha: %d/%d\n", vinfo->transp.length, vinfo->transp.offset);
}

static void fbdev_deinit(fbdev_video_t *fbdev)
{
   FBDEV_DEBUG(ENTER);

   if (fbdev->dummy_page) {
      free(fbdev->dummy_page);
      fbdev->dummy_page = NULL;
   }

   FBDEV_DEBUG(EXIT);
}

static int fbdev_init(fbdev_video_t *fbdev)
{
   FBDEV_DEBUG(ENTER);

   struct fb_var_screeninfo vinfo;
   unsigned width = fbdev->video_width;
   unsigned height = fbdev->video_height;
   unsigned bpp = fbdev->video_bpp;
   int i, page_height;

   if (ioctl(fbdev->fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
      FBDEV_ERR("Failed to do FBIOGET_VSCREENINFO\n");
      return -1;
   }

   FBDEV_LOG("Printing original vinfo:\n");
   print_vinfo(&vinfo);

#ifdef FBDEV_KEEP_FBDEV_PARAMS
   width = vinfo.xres;
   height = vinfo.yres;
   bpp = (vinfo.bits_per_pixel + 7) / 8;
#endif

   vinfo.activate = FB_ACTIVATE_NOW;
   vinfo.accel_flags = 0;
   vinfo.xres = width;
   vinfo.yres = height;
   vinfo.xres_virtual = width;
   vinfo.xoffset = 0;
   vinfo.yoffset = 0;
   vinfo.red.length = vinfo.red.offset = 0;
   vinfo.green.length = vinfo.green.offset = 0;
   vinfo.blue.length = vinfo.blue.offset = 0;
   vinfo.transp.length = vinfo.transp.offset = 0;

   fbdev->bpp = bpp;

retry_auto:
   vinfo.yres_virtual = fbdev->fbmem_size / width / fbdev->bpp;

#ifdef FBDEV_MAX_YRES_VIRTUAL
   if (vinfo.yres_virtual > FBDEV_MAX_YRES_VIRTUAL)
      vinfo.yres_virtual = FBDEV_MAX_YRES_VIRTUAL;
#endif

   for (i = FBDEV_MAX_PAGES; i > 0; i--) {
      page_height = vinfo.yres_virtual / i;
      if (page_height >= height)
         break;
   }

retry_fixed:
   vinfo.bits_per_pixel = fbdev->bpp * 8;

   fbdev->num_pages = FBDEV_MAX_PAGES;
   while (fbdev->num_pages > 0) {
#ifndef FBDEV_PREFER_BIG_PAGE
      if (fbdev->num_pages == 1 && page_height > height) {
         // Prefer dual buffers with smaller size than a single big buffer
         fbdev->num_pages = 0;
         break;
      }
#endif

      vinfo.yres_virtual = fbdev->num_pages * page_height;

      FBDEV_LOG("Printing wanted vinfo:\n");
      print_vinfo(&vinfo);

      if (!ioctl(fbdev->fd, FBIOPUT_VSCREENINFO, &vinfo))
         break;

      fbdev->num_pages--;
      if (fbdev->num_pages)
         FBDEV_LOG("Failed to init vscreen, retry pages: %d\n",
                   fbdev->num_pages);
   }

   if (!fbdev->num_pages) {
      if (fbdev->bpp == bpp) {
         fbdev->bpp = bpp == 4 ? 2 : 4;
         FBDEV_LOG("Failed to init vscreen, retry bpp: %d\n", fbdev->bpp);

         if (page_height == height)
            goto retry_fixed; // Last retry with new bpp
         else
            goto retry_auto; // retry with new bpp and auto page_height
      }

      if (page_height == height) {
         // Nothing else to try...
         FBDEV_ERR("Failed to set vscreen info\n");
         return -1;
      }

      fbdev->bpp = bpp;
      page_height = height;
      fbdev->num_pages = FBDEV_MAX_PAGES;
      FBDEV_LOG("Failed to init vscreen, retry page_height: %d\n",
                page_height);
      goto retry_fixed; // retry with original bpp and height
   }

   FBDEV_LOG("Printing actual vinfo:\n");
   print_vinfo(&vinfo);

   fbdev->vinfo = vinfo;
   fbdev->width = vinfo.xres;
   fbdev->height = vinfo.yres;
   fbdev->bpp = (vinfo.bits_per_pixel + 7) / 8;

   if (fbdev->bpp != 2 && fbdev->bpp != 4) {
      FBDEV_ERR("Unsupported bpp: %d\n", fbdev->bpp);
      return -1;
   }

   fbdev->num_pages = vinfo.yres_virtual / page_height;
   if (!fbdev->num_pages) {
      FBDEV_ERR("Failed to alloc pages\n");
      return -1;
   }

   if (fbdev->num_pages > FBDEV_MAX_PAGES)
      fbdev->num_pages = FBDEV_MAX_PAGES;

   FBDEV_LOG("Init vscreen for pages %dx%d-%d x%d\n",
             fbdev->width, page_height, fbdev->bpp, fbdev->num_pages);

   fbdev->page_size = fbdev->width * fbdev->bpp * page_height;
   for (i = 0; i < fbdev->num_pages; i++) {
      fbdev->pages[i].buf = fbdev->fbmem + i * fbdev->page_size;
      fbdev->pages[i].offset = 0;
      fbdev->pages[i].used = false;
   }

   fbdev->dummy_page = calloc(1, sizeof(fbdev_page_t) + fbdev->page_size);
   if (!fbdev->dummy_page)
      return -1;

   fbdev->dummy_page->buf = (void *)fbdev->dummy_page + sizeof(fbdev_page_t);

   FBDEV_LOG("Inited screen %dx%d-%d\n",
         fbdev->width, fbdev->height, fbdev->bpp * 8);

   video_driver_set_size(fbdev->width, fbdev->height);

   fbdev->curr_page = NULL;
   fbdev->pending_page = NULL;
   fbdev->menu_page = NULL;

   FBDEV_DEBUG(EXIT);
   return 0;
}

static void fbdev_gfx_free(void *data)
{
   FBDEV_DEBUG(ENTER);

   fbdev_video_t *fbdev = data;
   if (!fbdev)
      return;

   font_driver_free_osd();
   scaler_ctx_gen_reset(&fbdev->scaler);

   fbdev_deinit(fbdev);

   if (fbdev->fbmem) {
      munmap(fbdev->fbmem, fbdev->fbmem_size);
      fbdev->fbmem = NULL;
   }

   if (fbdev->fd >= 0) {
      close(fbdev->fd);
      fbdev->fd = -1;
   }

   free(fbdev);

   FBDEV_DEBUG(EXIT);
}

static void *fbdev_gfx_init(const video_info_t *video,
      input_driver_t **input, void **input_data)
{
   FBDEV_DEBUG(ENTER);

   struct fb_fix_screeninfo finfo;
   struct retro_system_av_info *av_info;

   av_info = video_viewport_get_system_av_info();
   if (!av_info) {
      FBDEV_ERR("Failed to get av info\n");
      return NULL;
   }

   fbdev_video_t *fbdev = calloc(1, sizeof(fbdev_video_t));
   if (!fbdev)
      return NULL;

   fbdev->video_bpp = video->rgb32 ? 4 : 2;
   fbdev->video_width = av_info->geometry.base_width;
   fbdev->video_height = av_info->geometry.base_height;

   fbdev->fd = open("/dev/fb0", O_RDWR);
   if (fbdev->fd < 0) {
      FBDEV_ERR("Failed to open fbdev\n");
      goto err;
   }

   if (ioctl(fbdev->fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
      FBDEV_ERR("Failed to do FBIOGET_FSCREENINFO\n");
      goto err;
   }

   fbdev->fbmem_size = finfo.smem_len;
   fbdev->fbmem = mmap(NULL, fbdev->fbmem_size, PROT_WRITE,
         MAP_SHARED, fbdev->fd, 0);
   if (fbdev->fbmem == MAP_FAILED) {
      FBDEV_ERR("Failed to mmap(%d): -%d\n", fbdev->fbmem_size, errno);
      fbdev->fbmem = NULL;
      goto err;
   }

   // Clear original screen data
   memset(fbdev->fbmem, 0, fbdev->fbmem_size);

   if (fbdev_init(fbdev) < 0) {
      FBDEV_ERR("Failed to init fbdev\n");
      goto err;
   }

   if (video->font_enable)
   {
      font_driver_init_osd(fbdev,
            video,
            false,
            video->is_threaded,
            FONT_DRIVER_RENDER_FBDEV);
   }

   if (input && input_data)
      *input = NULL;

   frontend_driver_destroy_signal_handler_state();

   frontend_driver_install_signal_handler();

   FBDEV_LOG("Inited video %dx%d-%d\n",
         fbdev->video_width, fbdev->video_height, fbdev->video_bpp * 8);

   FBDEV_DEBUG(EXIT);
   return fbdev;

err:
   fbdev_gfx_free(fbdev);
   FBDEV_ERR("initialization failed\n");

   FBDEV_DEBUG(EXIT);
   return NULL;
}

static fbdev_page_t *fbdev_get_free_page(fbdev_video_t *fbdev)
{
   FBDEV_DEBUG(ENTER);

   fbdev_page_t *page = NULL;
   int i;
   for (i = 0; i < fbdev->num_pages; i++) {
      if (!fbdev->pages[i].used) {
         page = &fbdev->pages[i];
         break;
      }
   }

   if (!page) {
      if (fbdev->num_pages > 1)
         FBDEV_ERR("Failed to get free page, fallback to first page\n");

      page = &fbdev->pages[0];
   }

   page->used = true;
   page->offset = 0;

   FBDEV_DEBUG(EXIT);
   return page;
}

static fbdev_page_t *fbdev_get_page(fbdev_video_t *fbdev,
      const void *frame, unsigned width, unsigned height, unsigned pitch,
      enum scaler_pix_fmt format)
{
   FBDEV_DEBUG(ENTER);

#if FBDEV_DEBUG_PERF
   retro_time_t time;
#endif

   fbdev_page_t *page;
   struct scaler_ctx *scaler = &fbdev->scaler;

   page = fbdev_get_free_page(fbdev);
   if (!page) {
      FBDEV_DEBUG(SCALE, "Failed to find free page\n");
      return NULL;
   }

   FBDEV_DEBUG(SCALE, "Scale %p %dx%d(%d) to %p %dx%d\n",
         frame, width, height, pitch, page->buf, fbdev->width, fbdev->height);

   scaler->scaler_type = SCALER_TYPE_POINT;
   scaler->out_fmt = fbdev->bpp == 4 ? SCALER_FMT_ARGB8888 : SCALER_FMT_RGB565;

   float aspect = video_driver_get_aspect_ratio();
   unsigned new_width = fbdev->width;
   unsigned new_height = fbdev->height;
   unsigned new_pitch = fbdev->width * fbdev->bpp;
   unsigned x = 0, y = 0;
   void *dst = page->buf;

#ifdef FBDEV_SCALE_WITH_ASPECT
   int i;

   if (new_width > new_height * aspect) {
      new_width = new_height * aspect;
      x = (fbdev->width - new_width) / 2;
   } else {
      new_height = new_width / aspect;
      y = (fbdev->height - new_height) / 2;
   }

   memset(dst, 0, y * new_pitch);
   dst += y * new_pitch;
   memset(dst + new_height * new_pitch, 0, y * new_pitch);
   for (i = y; i < fbdev->height - y; i++) {
      memset(dst, 0, x * fbdev->bpp);
      memset(dst + new_pitch - x * fbdev->bpp, 0, x * fbdev->bpp);
      dst += new_pitch;
   }

   dst = page->buf + (y * fbdev->width + x) * fbdev->bpp;
#endif

#if FBDEV_DEBUG_PERF
   time = cpu_features_get_time_usec();
   FBDEV_DEBUG(PERF, "Before scale: %f ms\n",
         (time - fbdev->last_frame_time) / 1000.0);
#endif

   video_frame_scale(scaler, dst, frame, format,
         new_width, new_height, new_pitch,
         width, height, pitch);

#if FBDEV_DEBUG_PERF
   time = cpu_features_get_time_usec();
   FBDEV_DEBUG(PERF, "After scale: %f ms\n",
         (time - fbdev->last_frame_time) / 1000.0);
#endif

   FBDEV_DEBUG(EXIT);
   return page;
}

static inline void fbdev_wait_vsync(fbdev_video_t *fbdev)
{
   int arg = 0;
   ioctl(fbdev->fd, FBIO_WAITFORVSYNC, &arg);
}

static void fbdev_page_flip(fbdev_video_t *fbdev)
{
   FBDEV_DEBUG(ENTER);

   if (fbdev->sync)
      fbdev_wait_vsync(fbdev);

   if (!fbdev->pending_page) {
      FBDEV_DEBUG(FRAME, "Nothing to display\n");
      return;
   }

   unsigned pitch = fbdev->width * fbdev->bpp;
   unsigned offset = fbdev->pending_page->buf +
      fbdev->pending_page->offset - fbdev->fbmem;

   fbdev->vinfo.yoffset = offset / pitch;
   fbdev->vinfo.xoffset = offset & pitch;
   ioctl(fbdev->fd, FBIOPAN_DISPLAY, &fbdev->vinfo);

   FBDEV_DEBUG(FRAME, "Flip at %d,%d\n",
               fbdev->vinfo.xoffset, fbdev->vinfo.yoffset);

   if (fbdev->curr_page) {
      fbdev->curr_page->used = false;
      fbdev->curr_page = NULL;
   }

   fbdev->curr_page = fbdev->pending_page;
   fbdev->curr_page->used = true;

   fbdev->pending_page = NULL;

   FBDEV_DEBUG(EXIT);
}

static bool fbdev_gfx_frame(void *data, const void *frame, unsigned width,
      unsigned height, uint64_t frame_count, unsigned pitch, const char *msg,
      video_frame_info_t *video_info)
{
   FBDEV_DEBUG(ENTER);

   fbdev_video_t *fbdev = data;
   fbdev_page_t *page;

   FBDEV_DEBUG(FRAME, "New frame %p %dx%d(%d)\n", frame, width, height, pitch);

   if (!string_is_empty(msg))
      FBDEV_DEBUG(MSG, "New msg: %s\n", msg);

#if FBDEV_DEBUG_PERF
   retro_time_t frame_start = cpu_features_get_time_usec();
   retro_time_t time;

   FBDEV_DEBUG(PERF, "Last frame duration: %f ms\n",
         (frame_start - fbdev->last_frame_time) / 1000.0);
   fbdev->last_frame_time = frame_start;
#endif

   /* Check if neither menu nor core framebuffer is to be displayed. */
   if (!fbdev->menu_active && !frame)
      return true;

   if (width != fbdev->video_width || height != fbdev->video_height)
   {
      FBDEV_LOG("mode set (resolution changed by core)\n");

      fbdev_deinit(fbdev);
      fbdev_wait_vsync(fbdev);

      fbdev->video_width = width;
      fbdev->video_height = height;

      if (fbdev_init(fbdev) < 0) {
         FBDEV_ERR("Failed to reinit screen\n");
         return false;
      }
   }

#ifdef HAVE_MENU
   if (fbdev->menu_active) {
      unsigned page_size = fbdev->width * fbdev->height * fbdev->bpp;
      unsigned bg_color_argb = 0x104E8B; // DodgerBlue4
      int i;

      // Use dummy page to draw menu text
      if (fbdev->bpp == 2) {
         uint16_t *p = fbdev->dummy_page->buf;
         uint16_t bg_color;
         conv_argb8888_rgb565(&bg_color, &bg_color_argb, 1, 1, 2, 4);
         for (i = 0; i < fbdev->width * fbdev->height; i++)
            *p++ = bg_color;
      } else {
         uint32_t *p = fbdev->dummy_page->buf;
         for (i = 0; i < fbdev->width * fbdev->height; i++)
            *p++ = bg_color_argb;
      }

      menu_driver_frame(video_info);

#if FBDEV_DEBUG_PERF
      time = cpu_features_get_time_usec();
      FBDEV_DEBUG(PERF, "Menu frame ready: %f ms\n",
            (time - frame_start) / 1000.0);
#endif

      if (fbdev->menu_page) {
         fbdev->pending_page = fbdev->menu_page;
         FBDEV_DEBUG(FRAME, "Show menu page\n");
      } else {
         // Use dummy page when menu page not provided
         fbdev->pending_page = fbdev_get_free_page(fbdev);
         if (fbdev->pending_page) {
            memcpy(fbdev->pending_page->buf, fbdev->dummy_page->buf, page_size);
            FBDEV_DEBUG(FRAME, "Show dummy page\n");
         }
      }

#if FBDEV_DEBUG_PERF
      time = cpu_features_get_time_usec();
      FBDEV_DEBUG(PERF, "Menu ready: %f ms\n", (time - frame_start) / 1000.0);
#endif
   }
#endif

   if (fbdev->pending_page)
      goto done;

   fbdev->pending_page = fbdev_get_page(fbdev,
         frame, width, height, pitch,
         fbdev->video_bpp == 4 ? SCALER_FMT_ARGB8888 : SCALER_FMT_RGB565);
   if (!fbdev->pending_page) {
      FBDEV_ERR("Failed to display frame\n");
      return true;
   }

#if FBDEV_DEBUG_PERF
   time = cpu_features_get_time_usec();
   FBDEV_DEBUG(PERF, "Frame ready: %f ms\n", (time - frame_start) / 1000.0);
#endif

   if (video_info->statistics_show)
   {
      struct font_params *osd_params = (struct font_params*)
         &video_info->osd_stat_params;

      if (osd_params)
      {
         font_driver_render_msg(fbdev, video_info->stat_text,
               (const struct font_params*)osd_params, NULL);

#if FBDEV_DEBUG_PERF
         time = cpu_features_get_time_usec();
         FBDEV_DEBUG(PERF, "After render statistics msg: %f ms\n",
               (time - frame_start) / 1000.0);
#endif
      }
   }

done:
   if (!string_is_empty(msg)) {
      font_driver_render_msg(fbdev, msg, NULL, NULL);

#if FBDEV_DEBUG_PERF
      time = cpu_features_get_time_usec();
      FBDEV_DEBUG(PERF, "After render msg: %f ms\n",
            (time - frame_start) / 1000.0);
#endif

   }

#if FBDEV_DEBUG_PERF
   time = cpu_features_get_time_usec();
   FBDEV_DEBUG(PERF, "Before flip: %f ms\n", (time - frame_start) / 1000.0);
#endif

   fbdev_page_flip(fbdev);

#if FBDEV_DEBUG_PERF
   time = cpu_features_get_time_usec();
   FBDEV_DEBUG(PERF, "After flip: %f ms\n", (time - frame_start) / 1000.0);
#endif

   FBDEV_DEBUG(EXIT);
   return true;
}

static void fbdev_gfx_set_nonblock_state(void* data, bool toggle,
      bool a, unsigned b)
{
   fbdev_video_t *fbdev = data;

   if (fbdev)
      fbdev->sync = !toggle;
}

static bool fbdev_gfx_alive(void *data)
{
   (void)data;
   return !frontend_driver_get_signal_handler_state();
}

static bool fbdev_gfx_focus(void *data)
{
   (void)data;
   return true; /* fb device always has focus */
}

static bool fbdev_gfx_suppress_screensaver(void *data, bool enable)
{
   (void)data;
   (void)enable;

   return false;
}

static void fbdev_gfx_viewport_info(void *data, struct video_viewport *vp)
{
   fbdev_video_t *fbdev = data;

   if (!fbdev)
      return;

   vp->x = vp->y = 0;

   vp->width  = vp->full_width  = fbdev->video_width;
   vp->height = vp->full_height = fbdev->video_height;
}

static bool fbdev_gfx_read_viewport(void *data, uint8_t *buffer, bool is_idle)
{
   (void)data;
   (void)buffer;
   return true;
}

static float fbdev_get_refresh_rate(void *data)
{
   fbdev_video_t *fbdev = data;
   struct fb_var_screeninfo *vinfo = &fbdev->vinfo;

   float ret = 1000000.0f / vinfo->pixclock /
      (vinfo->xres + vinfo->left_margin + vinfo->right_margin + vinfo->hsync_len) * 1000000.0f /
      (vinfo->yres + vinfo->upper_margin + vinfo->lower_margin + vinfo->vsync_len);
   return ret;
}

static void fbdev_gfx_set_texture_frame(void *data,
      const void *frame, bool rgb32,
      unsigned width, unsigned height, float alpha)
{
   FBDEV_DEBUG(ENTER);

   (void) alpha;
   fbdev_video_t *fbdev = data;

   enum scaler_pix_fmt format =
      rgb32 ? SCALER_FMT_ARGB8888 : SCALER_FMT_RGBA4444;
   unsigned pitch = rgb32 ? width * 4 : width * 2;

   FBDEV_DEBUG(FRAME, "New menu %p %dx%d(%d)\n", frame, width, height, pitch);

   if (fbdev->menu_page) {
      fbdev->menu_page->used = false;
      fbdev->menu_page = NULL;
   }

   fbdev->menu_page = fbdev_get_page(fbdev,
         frame, width, height, pitch, format);

   FBDEV_DEBUG(EXIT);
}

static void fbdev_gfx_set_texture_enable(void *data,
      bool state, bool full_screen)
{
   FBDEV_DEBUG(ENTER);

   (void) full_screen;
   fbdev_video_t *fbdev = data;
   if (!fbdev)
      return;

   fbdev->menu_active = state;

   if (!state && fbdev->menu_page) {
      fbdev->menu_page->used = false;
      fbdev->menu_page = NULL;
   }

   FBDEV_DEBUG(EXIT);
}

static void fbdev_set_osd_msg(void *data,
      const char *msg,
      const void *params, void *font)
{
   FBDEV_DEBUG(ENTER);

   fbdev_video_t *fbdev = data;

   FBDEV_DEBUG(MSG, "New osd msg: %s\n", msg);

   font_driver_render_msg(fbdev, msg, params, font);

   FBDEV_DEBUG(EXIT);
}

static const video_poke_interface_t fbdev_gfx_poke_interface = {
   NULL, /* get_flags  */
   NULL, /* load_texture */
   NULL, /* unload_texture */
   NULL, /* set_video_mode */
   fbdev_get_refresh_rate,
   NULL, /* set_filtering */
   NULL, /* get_video_output_size */
   NULL, /* get_video_output_prev */
   NULL, /* get_video_output_next */
   NULL, /* get_current_framebuffer */
   NULL, /* get_proc_address */
   NULL, /* set_aspect_ratio */
   NULL, /* apply_state_changes */
   fbdev_gfx_set_texture_frame,
   fbdev_gfx_set_texture_enable,
   fbdev_set_osd_msg,
   NULL, /* show_mouse */
   NULL, /* grab_mouse_toggle */
   NULL, /* get_current_shader */
   NULL, /* get_current_software_framebuffer */
   NULL, /* get_hw_render_interface */
};

static void fbdev_gfx_get_poke_interface(void *data,
      const video_poke_interface_t **iface)
{
   FBDEV_DEBUG(ENTER);

   (void)data;
   *iface = &fbdev_gfx_poke_interface;

   FBDEV_DEBUG(EXIT);
}

video_driver_t video_fbdev = {
   fbdev_gfx_init,
   fbdev_gfx_frame,
   fbdev_gfx_set_nonblock_state,
   fbdev_gfx_alive,
   fbdev_gfx_focus,
   fbdev_gfx_suppress_screensaver,
   NULL, /* has_windowed */
   NULL, /* set_shader */
   fbdev_gfx_free,
   "fbdev",
   NULL, /* set_viewport */
   NULL, /* set_rotation */
   fbdev_gfx_viewport_info,
   fbdev_gfx_read_viewport,
   NULL, /* read_frame_raw */
#ifdef HAVE_OVERLAY
   NULL, /* overlay_interface */
#endif
#ifdef HAVE_VIDEO_LAYOUT
   NULL,
#endif
   fbdev_gfx_get_poke_interface
};
