/**************************************************************************
 *
 * Copyright 2010 Thomas Balling Sørensen & Orasanu Lucian.
 * Copyright 2014 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include "pipe/p_screen.h"

#include "util/u_memory.h"
#include "util/u_handle_table.h"
#include "util/u_surface.h"
#include "util/u_video.h"

#include "vl/vl_winsys.h"

#include "va_private.h"

static const VAImageFormat formats[VL_VA_MAX_IMAGE_FORMATS] =
{
   {VA_FOURCC('N','V','1','2')},
   {VA_FOURCC('I','4','2','0')},
   {VA_FOURCC('Y','V','1','2')},
   {VA_FOURCC('Y','U','Y','V')},
   {VA_FOURCC('U','Y','V','Y')},
   {.fourcc = VA_FOURCC('B','G','R','A'), .byte_order = VA_LSB_FIRST, 32, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000},
   {.fourcc = VA_FOURCC('R','G','B','A'), .byte_order = VA_LSB_FIRST, 32, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000},
   {.fourcc = VA_FOURCC('B','G','R','X'), .byte_order = VA_LSB_FIRST, 32, 24, 0x00ff0000, 0x0000ff00, 0x000000ff, 0x00000000},
   {.fourcc = VA_FOURCC('R','G','B','X'), .byte_order = VA_LSB_FIRST, 32, 24, 0x000000ff, 0x0000ff00, 0x00ff0000, 0x00000000}
};

static void
vlVaVideoSurfaceSize(vlVaSurface *p_surf, int component,
                     unsigned *width, unsigned *height)
{
   *width = p_surf->templat.width;
   *height = p_surf->templat.height;

   if (component > 0) {
      if (p_surf->templat.chroma_format == PIPE_VIDEO_CHROMA_FORMAT_420) {
         *width /= 2;
         *height /= 2;
      } else if (p_surf->templat.chroma_format == PIPE_VIDEO_CHROMA_FORMAT_422)
         *width /= 2;
   }
   if (p_surf->templat.interlaced)
      *height /= 2;
}

VAStatus
vlVaQueryImageFormats(VADriverContextP ctx, VAImageFormat *format_list, int *num_formats)
{
   struct pipe_screen *pscreen;
   enum pipe_format format;
   int i;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   if (!(format_list && num_formats))
      return VA_STATUS_ERROR_INVALID_PARAMETER;

   *num_formats = 0;
   pscreen = VL_VA_PSCREEN(ctx);
   for (i = 0; i < VL_VA_MAX_IMAGE_FORMATS; ++i) {
      format = YCbCrToPipe(formats[i].fourcc);
      if (pscreen->is_video_format_supported(pscreen, format,
          PIPE_VIDEO_PROFILE_UNKNOWN,
          PIPE_VIDEO_ENTRYPOINT_BITSTREAM))
         format_list[(*num_formats)++] = formats[i];
   }

   return VA_STATUS_SUCCESS;
}

VAStatus
vlVaCreateImage(VADriverContextP ctx, VAImageFormat *format, int width, int height, VAImage *image)
{
   VAStatus status;
   vlVaDriver *drv;
   VAImage *img;
   int w, h;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   if (!(format && image && width && height))
      return VA_STATUS_ERROR_INVALID_PARAMETER;

   drv = VL_VA_DRIVER(ctx);

   img = CALLOC(1, sizeof(VAImage));
   if (!img)
      return VA_STATUS_ERROR_ALLOCATION_FAILED;
   img->image_id = handle_table_add(drv->htab, img);

   img->format = *format;
   img->width = width;
   img->height = height;
   w = align(width, 2);
   h = align(height, 2);

   switch (format->fourcc) {
   case VA_FOURCC('N','V','1','2'):
      img->num_planes = 2;
      img->pitches[0] = w;
      img->offsets[0] = 0;
      img->pitches[1] = w;
      img->offsets[1] = w * h;
      img->data_size  = w * h * 3 / 2;
      break;

   case VA_FOURCC('I','4','2','0'):
   case VA_FOURCC('Y','V','1','2'):
      img->num_planes = 3;
      img->pitches[0] = w;
      img->offsets[0] = 0;
      img->pitches[1] = w / 2;
      img->offsets[1] = w * h;
      img->pitches[2] = w / 2;
      img->offsets[2] = w * h * 5 / 4;
      img->data_size  = w * h * 3 / 2;
      break;

   case VA_FOURCC('U','Y','V','Y'):
   case VA_FOURCC('Y','U','Y','V'):
      img->num_planes = 1;
      img->pitches[0] = w * 2;
      img->offsets[0] = 0;
      img->data_size  = w * h * 2;
      break;

   case VA_FOURCC('B','G','R','A'):
   case VA_FOURCC('R','G','B','A'):
   case VA_FOURCC('B','G','R','X'):
   case VA_FOURCC('R','G','B','X'):
      img->num_planes = 1;
      img->pitches[0] = w * 4;
      img->offsets[0] = 0;
      img->data_size  = w * h * 4;
      break;

   default:
      return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
   }

   status =  vlVaCreateBuffer(ctx, 0, VAImageBufferType,
                           align(img->data_size, 16),
                           1, NULL, &img->buf);
   if (status != VA_STATUS_SUCCESS)
      return status;
   *image = *img;

   return status;
}

VAStatus
vlVaDeriveImage(VADriverContextP ctx, VASurfaceID surface, VAImage *image)
{
   vlVaDriver *drv = NULL;
   vlVaSurface *surf = NULL;
   vlVaBuffer *img_buf = NULL;
   VAImage *img = NULL;
   struct pipe_surface **surfaces = NULL;
   int w = 0;
   int h = 0;
   int i = 0;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   drv = VL_VA_DRIVER(ctx);

   surf = handle_table_get(drv->htab, surface);

   if (!surf || !surf->buffer)
      return VA_STATUS_ERROR_INVALID_SURFACE;

   img = CALLOC(1, sizeof(VAImage));
   if (!img)
      return VA_STATUS_ERROR_ALLOCATION_FAILED;

   img->format.fourcc = PipeToYCbCr(surf->buffer->buffer_format);
   img->buf = VA_INVALID_ID;
   img->width = surf->buffer->width;
   img->height = surf->buffer->height;
   img->num_palette_entries = 0;
   img->entry_bytes = 0;
   w = align(surf->buffer->width, 2);
   h = align(surf->buffer->height, 2);

   switch (img->format.fourcc) {
   case VA_FOURCC('N','V','1','2'):
      img->num_planes = 2;
      img->pitches[0] = w;
      img->offsets[0] = 0;
      img->pitches[1] = w;
      img->offsets[1] = w * h;
      img->data_size  = w * h * 3 / 2;
      break;

   case VA_FOURCC('I','4','2','0'):
   case VA_FOURCC('Y','V','1','2'):
      img->num_planes = 3;
      img->pitches[0] = w;
      img->offsets[0] = 0;
      img->pitches[1] = w / 2;
      img->offsets[1] = w * h;
      img->pitches[2] = w / 2;
      img->offsets[2] = w * h * 5 / 4;
      img->data_size  = w * h * 3 / 2;
      break;

   case VA_FOURCC('U','Y','V','Y'):
   case VA_FOURCC('Y','U','Y','V'):
      img->num_planes = 1;
      img->pitches[0] = w * 2;
      img->offsets[0] = 0;
      img->data_size  = w * h * 2;
      break;

   case VA_FOURCC('B','G','R','A'):
   case VA_FOURCC('R','G','B','A'):
   case VA_FOURCC('B','G','R','X'):
   case VA_FOURCC('R','G','B','X'):
      img->num_planes = 1;
      img->pitches[0] = w * 4;
      img->offsets[0] = 0;
      img->data_size  = w * h * 4;
      for (i = 0; i < VL_VA_MAX_IMAGE_FORMATS; ++i) {
         if (img->format.fourcc == formats[i].fourcc)
           img->format = formats[i];
      }
      break;

   default:
      return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
   }

   /* Only support 1 plane for now. */
   if (img->num_planes > 1)
      return VA_STATUS_ERROR_UNIMPLEMENTED;

   img_buf = CALLOC(1, sizeof(vlVaBuffer));
   if (!img_buf) {
      FREE(img);
      return VA_STATUS_ERROR_ALLOCATION_FAILED;
   }

   img->image_id = handle_table_add(drv->htab, img);

   img_buf->type = VAImageBufferType;
   img_buf->size = image->data_size;
   img_buf->num_elements = 1;
   img_buf->data = NULL;
   img_buf->fence = surf->fence;

   surfaces = surf->buffer->get_surfaces(surf->buffer);

   if (!surfaces || !surfaces[0]->texture) {
       FREE(img);
       FREE(img_buf);
       return VA_STATUS_ERROR_ALLOCATION_FAILED;
   }

   pipe_resource_reference(&img_buf->resource, surfaces[0]->texture);

   img->buf = handle_table_add(VL_VA_DRIVER(ctx)->htab, img_buf);

   *image = *img;

   return VA_STATUS_SUCCESS;
}

VAStatus
vlVaDestroyImage(VADriverContextP ctx, VAImageID image)
{
   VAImage  *vaimage;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   vaimage = handle_table_get(VL_VA_DRIVER(ctx)->htab, image);
   if (!vaimage)
      return VA_STATUS_ERROR_INVALID_IMAGE;

   handle_table_remove(VL_VA_DRIVER(ctx)->htab, image);
   FREE(vaimage);
   return vlVaDestroyBuffer(ctx, vaimage->buf);
}

VAStatus
vlVaSetImagePalette(VADriverContextP ctx, VAImageID image, unsigned char *palette)
{
   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus
vlVaGetImage(VADriverContextP ctx, VASurfaceID surface, int x, int y,
             unsigned int width, unsigned int height, VAImageID image)
{
   vlVaDriver *drv;
   vlVaSurface *surf;
   vlVaBuffer *img_buf;
   VAImage *vaimage;
   struct pipe_sampler_view **views;
   enum pipe_format format;
   bool convert = false;
   void *data[3];
   unsigned pitches[3], i, j;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   drv = VL_VA_DRIVER(ctx);

   surf = handle_table_get(drv->htab, surface);
   if (!surf || !surf->buffer)
      return VA_STATUS_ERROR_INVALID_SURFACE;

   vaimage = handle_table_get(drv->htab, image);
   if (!vaimage)
      return VA_STATUS_ERROR_INVALID_IMAGE;

   img_buf = handle_table_get(drv->htab, vaimage->buf);
   if (!img_buf)
      return VA_STATUS_ERROR_INVALID_BUFFER;

   format = YCbCrToPipe(vaimage->format.fourcc);
   if (format == PIPE_FORMAT_NONE)
      return VA_STATUS_ERROR_OPERATION_FAILED;

   if (format != surf->buffer->buffer_format) {
      /* support NV12 to YV12 conversion now only */
      if (format == PIPE_FORMAT_YV12 &&
          surf->buffer->buffer_format == PIPE_FORMAT_NV12)
         convert = true;
      else
         return VA_STATUS_ERROR_OPERATION_FAILED;
   }

   views = surf->buffer->get_sampler_view_planes(surf->buffer);
   if (!views)
      return VA_STATUS_ERROR_OPERATION_FAILED;

   for (i = 0; i < vaimage->num_planes; i++) {
      data[i] = img_buf->data + vaimage->offsets[i];
      pitches[i] = vaimage->pitches[i];
   }
   if (vaimage->format.fourcc == VA_FOURCC('I','4','2','0')) {
      void *tmp_d;
      unsigned tmp_p;
      tmp_d  = data[1];
      data[1] = data[2];
      data[2] = tmp_d;
      tmp_p = pitches[1];
      pitches[1] = pitches[2];
      pitches[2] = tmp_p;
   }

   for (i = 0; i < vaimage->num_planes; i++) {
      unsigned width, height;
      if (!views[i]) continue;
      vlVaVideoSurfaceSize(surf, i, &width, &height);
      for (j = 0; j < views[i]->texture->array_size; ++j) {
         struct pipe_box box = {0, 0, j, width, height, 1};
         struct pipe_transfer *transfer;
         uint8_t *map;
         map = drv->pipe->transfer_map(drv->pipe, views[i]->texture, 0,
                  PIPE_TRANSFER_READ, &box, &transfer);
         if (!map)
            return VA_STATUS_ERROR_OPERATION_FAILED;

         if (i == 1 && convert) {
            u_copy_nv12_to_yv12(data, pitches, i, j,
               transfer->stride, views[i]->texture->array_size,
               map, box.width, box.height);
         } else {
            util_copy_rect(data[i] + pitches[i] * j,
               views[i]->texture->format,
               pitches[i] * views[i]->texture->array_size, 0, 0,
               box.width, box.height, map, transfer->stride, 0, 0);
         }
         pipe_transfer_unmap(drv->pipe, transfer);
      }
   }

   return VA_STATUS_SUCCESS;
}

VAStatus
vlVaPutImage(VADriverContextP ctx, VASurfaceID surface, VAImageID image,
             int src_x, int src_y, unsigned int src_width, unsigned int src_height,
             int dest_x, int dest_y, unsigned int dest_width, unsigned int dest_height)
{
   vlVaDriver *drv;
   vlVaSurface *surf;
   vlVaBuffer *img_buf;
   VAImage *vaimage;
   struct pipe_sampler_view **views;
   enum pipe_format format;
   void *data[3];
   unsigned pitches[3], i, j;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   drv = VL_VA_DRIVER(ctx);

   surf = handle_table_get(drv->htab, surface);
   if (!surf || !surf->buffer)
      return VA_STATUS_ERROR_INVALID_SURFACE;

   vaimage = handle_table_get(drv->htab, image);
   if (!vaimage)
      return VA_STATUS_ERROR_INVALID_IMAGE;

   img_buf = handle_table_get(drv->htab, vaimage->buf);
   if (!img_buf)
      return VA_STATUS_ERROR_INVALID_BUFFER;

   if (img_buf->resource) {
      /* Attempting to transfer derived image to surface */
      return VA_STATUS_ERROR_UNIMPLEMENTED;
   }

   format = YCbCrToPipe(vaimage->format.fourcc);
   if (format == PIPE_FORMAT_NONE)
      return VA_STATUS_ERROR_OPERATION_FAILED;

   if (format != surf->buffer->buffer_format) {
      struct pipe_video_buffer *tmp_buf = NULL;
      enum pipe_format old_surf_format = surf->templat.buffer_format;

      surf->templat.buffer_format = format;
      tmp_buf = drv->pipe->create_video_buffer(drv->pipe, &surf->templat);

      if (!tmp_buf) {
          surf->templat.buffer_format = old_surf_format;
          return VA_STATUS_ERROR_ALLOCATION_FAILED;
      }

      if (surf->buffer)
         surf->buffer->destroy(surf->buffer);

      surf->buffer = tmp_buf;
   }

   views = surf->buffer->get_sampler_view_planes(surf->buffer);
   if (!views)
      return VA_STATUS_ERROR_OPERATION_FAILED;

   for (i = 0; i < vaimage->num_planes; i++) {
      data[i] = img_buf->data + vaimage->offsets[i];
      pitches[i] = vaimage->pitches[i];
   }
   if (vaimage->format.fourcc == VA_FOURCC('I','4','2','0')) {
      void *tmp_d;
      unsigned tmp_p;
      tmp_d  = data[1];
      data[1] = data[2];
      data[2] = tmp_d;
      tmp_p = pitches[1];
      pitches[1] = pitches[2];
      pitches[2] = tmp_p;
   }

   for (i = 0; i < vaimage->num_planes; ++i) {
      unsigned width, height;
      if (!views[i]) continue;
      vlVaVideoSurfaceSize(surf, i, &width, &height);
      for (j = 0; j < views[i]->texture->array_size; ++j) {
         struct pipe_box dst_box = {0, 0, j, width, height, 1};
         drv->pipe->transfer_inline_write(drv->pipe, views[i]->texture, 0,
            PIPE_TRANSFER_WRITE, &dst_box,
            data[i] + pitches[i] * j,
            pitches[i] * views[i]->texture->array_size, 0);
      }
   }

   return VA_STATUS_SUCCESS;
}
