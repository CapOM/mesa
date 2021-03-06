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
#include "pipe/p_video_codec.h"

#include "state_tracker/drm_driver.h"

#include "util/u_memory.h"
#include "util/u_handle_table.h"
#include "util/u_rect.h"
#include "util/u_sampler.h"
#include "util/u_surface.h"

#include "vl/vl_compositor.h"
#include "vl/vl_video_buffer.h"
#include "vl/vl_winsys.h"

#include "va_private.h"

VAStatus
vlVaCreateSurfaces(VADriverContextP ctx, int width, int height, int format,
                   int num_surfaces, VASurfaceID *surfaces)
{
    return vlVaCreateSurfaces2(ctx, format, width, height, surfaces, num_surfaces,
                               NULL, 0);
}

VAStatus
vlVaDestroySurfaces(VADriverContextP ctx, VASurfaceID *surface_list, int num_surfaces)
{
   vlVaDriver *drv;
   int i;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   drv = VL_VA_DRIVER(ctx);
   for (i = 0; i < num_surfaces; ++i) {
      vlVaSurface *surf = handle_table_get(drv->htab, surface_list[i]);
      if (surf->buffer)
         surf->buffer->destroy(surf->buffer);
      if(surf->fence)
         drv->pipe->screen->fence_reference(drv->pipe->screen, &surf->fence, NULL);
      util_dynarray_fini(&surf->subpics);
      FREE(surf);
      handle_table_remove(drv->htab, surface_list[i]);
   }

   return VA_STATUS_SUCCESS;
}

VAStatus
vlVaSyncSurface(VADriverContextP ctx, VASurfaceID render_target)
{
   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   return VA_STATUS_SUCCESS;
}

VAStatus
vlVaQuerySurfaceStatus(VADriverContextP ctx, VASurfaceID render_target, VASurfaceStatus *status)
{
   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   return VA_STATUS_SUCCESS;
}

VAStatus
vlVaQuerySurfaceError(VADriverContextP ctx, VASurfaceID render_target, VAStatus error_status, void **error_info)
{
   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static void
upload_sampler(struct pipe_context *pipe, struct pipe_sampler_view *dst,
               const struct pipe_box *dst_box, const void *src, unsigned src_stride,
               unsigned src_x, unsigned src_y)
{
   struct pipe_transfer *transfer;
   void *map;

   map = pipe->transfer_map(pipe, dst->texture, 0, PIPE_TRANSFER_WRITE,
                            dst_box, &transfer);
   if (!map)
      return;

   util_copy_rect(map, dst->texture->format, transfer->stride, 0, 0,
                  dst_box->width, dst_box->height,
                  src, src_stride, src_x, src_y);

   pipe->transfer_unmap(pipe, transfer);
}

static VAStatus
vlVaPutSubpictures(vlVaSurface *surf, vlVaDriver *drv,
                   struct pipe_surface *surf_draw, struct u_rect *dirty_area,
                   struct u_rect *src_rect, struct u_rect *dst_rect)
{
   vlVaSubpicture *sub;
   int i;

   if (!(surf->subpics.data || surf->subpics.size))
      return VA_STATUS_SUCCESS;

   for (i = 0; i < surf->subpics.size/sizeof(vlVaSubpicture *); i++) {
      struct pipe_blend_state blend;
      void *blend_state;
      vlVaBuffer *buf;
      struct pipe_box box;
      struct u_rect *s, *d, sr, dr, c;
      int sw, sh, dw, dh;

      sub = ((vlVaSubpicture **)surf->subpics.data)[i];
      if (!sub)
         continue;

      buf = handle_table_get(drv->htab, sub->image->buf);
      if (!buf)
         return VA_STATUS_ERROR_INVALID_IMAGE;

      box.x = 0;
      box.y = 0;
      box.z = 0;
      box.width = sub->dst_rect.x1 - sub->dst_rect.x0;
      box.height = sub->dst_rect.y1 - sub->dst_rect.y0;
      box.depth = 1;

      s = &sub->src_rect;
      d = &sub->dst_rect;
      sw = s->x1 - s->x0;
      sh = s->y1 - s->y0;
      dw = d->x1 - d->x0;
      dh = d->y1 - d->y0;
      c.x0 = MAX2(d->x0, s->x0);
      c.y0 = MAX2(d->y0, s->y0);
      c.x1 = MIN2(d->x0 + dw, src_rect->x1);
      c.y1 = MIN2(d->y0 + dh, src_rect->y1);
      sr.x0 = s->x0 + (c.x0 - d->x0)*(sw/(float)dw);
      sr.y0 = s->y0 + (c.y0 - d->y0)*(sh/(float)dh);
      sr.x1 = s->x0 + (c.x1 - d->x0)*(sw/(float)dw);
      sr.y1 = s->y0 + (c.y1 - d->y0)*(sh/(float)dh);

      s = src_rect;
      d = dst_rect;
      sw = s->x1 - s->x0;
      sh = s->y1 - s->y0;
      dw = d->x1 - d->x0;
      dh = d->y1 - d->y0;
      dr.x0 = d->x0 + c.x0*(dw/(float)sw);
      dr.y0 = d->y0 + c.y0*(dh/(float)sh);
      dr.x1 = d->x0 + c.x1*(dw/(float)sw);
      dr.y1 = d->y0 + c.y1*(dh/(float)sh);

      memset(&blend, 0, sizeof(blend));
      blend.independent_blend_enable = 0;
      blend.rt[0].blend_enable = 1;
      blend.rt[0].rgb_src_factor = PIPE_BLENDFACTOR_SRC_ALPHA;
      blend.rt[0].rgb_dst_factor = PIPE_BLENDFACTOR_INV_SRC_ALPHA;
      blend.rt[0].alpha_src_factor = PIPE_BLENDFACTOR_ZERO;
      blend.rt[0].alpha_dst_factor = PIPE_BLENDFACTOR_ZERO;
      blend.rt[0].rgb_func = PIPE_BLEND_ADD;
      blend.rt[0].alpha_func = PIPE_BLEND_ADD;
      blend.rt[0].colormask = PIPE_MASK_RGBA;
      blend.logicop_enable = 0;
      blend.logicop_func = PIPE_LOGICOP_CLEAR;
      blend.dither = 0;
      blend_state = drv->pipe->create_blend_state(drv->pipe, &blend);

      vl_compositor_clear_layers(&drv->cstate);
      vl_compositor_set_layer_blend(&drv->cstate, 0, blend_state, false);
      upload_sampler(drv->pipe, sub->sampler, &box, buf->data,
                     sub->image->pitches[0], 0, 0);
      vl_compositor_set_rgba_layer(&drv->cstate, &drv->compositor, 0, sub->sampler,
                                   &sr, NULL, NULL);
      vl_compositor_set_layer_dst_area(&drv->cstate, 0, &dr);
      vl_compositor_render(&drv->cstate, &drv->compositor, surf_draw, dirty_area, false);
      drv->pipe->delete_blend_state(drv->pipe, blend_state);
   }

   return VA_STATUS_SUCCESS;
}

VAStatus
vlVaPutSurface(VADriverContextP ctx, VASurfaceID surface_id, void* draw, short srcx, short srcy,
               unsigned short srcw, unsigned short srch, short destx, short desty,
               unsigned short destw, unsigned short desth, VARectangle *cliprects,
               unsigned int number_cliprects,  unsigned int flags)
{
   vlVaDriver *drv;
   vlVaSurface *surf;
   struct pipe_screen *screen;
   struct pipe_resource *tex;
   struct pipe_surface surf_templ, *surf_draw;
   struct u_rect src_rect, *dirty_area;
   struct u_rect dst_rect = {destx, destx + destw, desty, desty + desth};
   VAStatus status;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   drv = VL_VA_DRIVER(ctx);
   surf = handle_table_get(drv->htab, surface_id);
   if (!surf)
      return VA_STATUS_ERROR_INVALID_SURFACE;

   screen = drv->pipe->screen;

   if(surf->fence) {
      screen->fence_finish(screen, surf->fence, PIPE_TIMEOUT_INFINITE);
      screen->fence_reference(screen, &surf->fence, NULL);
   }

   tex = vl_screen_texture_from_drawable(drv->vscreen, (Drawable)draw);
   if (!tex)
      return VA_STATUS_ERROR_INVALID_DISPLAY;

   dirty_area = vl_screen_get_dirty_area(drv->vscreen);

   memset(&surf_templ, 0, sizeof(surf_templ));
   surf_templ.format = tex->format;
   surf_draw = drv->pipe->create_surface(drv->pipe, tex, &surf_templ);
   if (!surf_draw) {
      pipe_resource_reference(&tex, NULL);
      return VA_STATUS_ERROR_INVALID_DISPLAY;
   }

   src_rect.x0 = srcx;
   src_rect.y0 = srcy;
   src_rect.x1 = srcw + srcx;
   src_rect.y1 = srch + srcy;

   vl_compositor_clear_layers(&drv->cstate);
   vl_compositor_set_buffer_layer(&drv->cstate, &drv->compositor, 0, surf->buffer, &src_rect, NULL, VL_COMPOSITOR_WEAVE);
   vl_compositor_set_layer_dst_area(&drv->cstate, 0, &dst_rect);
   vl_compositor_render(&drv->cstate, &drv->compositor, surf_draw, dirty_area, true);

   status = vlVaPutSubpictures(surf, drv, surf_draw, dirty_area, &src_rect, &dst_rect);
   if (status)
      return status;

   screen->flush_frontbuffer
   (
      screen, tex, 0, 0,
      vl_screen_get_private(drv->vscreen), NULL
   );

   screen->fence_reference(screen, &surf->fence, NULL);
   drv->pipe->flush(drv->pipe, &surf->fence, 0);

   pipe_resource_reference(&tex, NULL);
   pipe_surface_reference(&surf_draw, NULL);

   return VA_STATUS_SUCCESS;
}

VAStatus
vlVaLockSurface(VADriverContextP ctx, VASurfaceID surface, unsigned int *fourcc,
                unsigned int *luma_stride, unsigned int *chroma_u_stride, unsigned int *chroma_v_stride,
                unsigned int *luma_offset, unsigned int *chroma_u_offset, unsigned int *chroma_v_offset,
                unsigned int *buffer_name, void **buffer)
{
   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus
vlVaUnlockSurface(VADriverContextP ctx, VASurfaceID surface)
{
   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus
vlVaGetSurfaceAttributes(VADriverContextP ctx, VAConfigID config,
                         VASurfaceAttrib *attrib_list, unsigned int num_attribs)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED; /* DEPRECATED */
}

VAStatus
vlVaQuerySurfaceAttributes(VADriverContextP ctx, VAConfigID config,
                           VASurfaceAttrib *attrib_list, unsigned int *num_attribs)
{
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    vlVaDriver *drv = NULL;
    VASurfaceAttrib *attribs = NULL;
    struct pipe_screen *pscreen = NULL;
    int i = 0;

    if (config == VA_INVALID_ID)
        return VA_STATUS_ERROR_INVALID_CONFIG;

    if (!attrib_list && !num_attribs)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (attrib_list == NULL) {
        *num_attribs = VASurfaceAttribCount;
        return VA_STATUS_SUCCESS;
    }

    attribs = CALLOC(VASurfaceAttribCount, sizeof(VASurfaceAttrib));

    if (attribs == NULL)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    if (!ctx)
       return VA_STATUS_ERROR_INVALID_CONTEXT;

    drv = VL_VA_DRIVER(ctx);

    if (!drv)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    pscreen = VL_VA_PSCREEN(ctx);

    if (!pscreen)
       return VA_STATUS_ERROR_INVALID_CONTEXT;

    if (config == PIPE_VIDEO_PROFILE_UNKNOWN) {
       /* Assume VAEntrypointVideoProc for now. */
       attribs[i].type = VASurfaceAttribPixelFormat;
       attribs[i].value.type = VAGenericValueTypeInteger;
       attribs[i].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
       attribs[i].value.value.i = VA_FOURCC_BGRA;
       i++;

       attribs[i].type = VASurfaceAttribPixelFormat;
       attribs[i].value.type = VAGenericValueTypeInteger;
       attribs[i].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
       attribs[i].value.value.i = VA_FOURCC_RGBA;
       i++;
    } else {
       /* Assume VAEntrypointVLD for now. */
       attribs[i].type = VASurfaceAttribPixelFormat;
       attribs[i].value.type = VAGenericValueTypeInteger;
       attribs[i].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
       attribs[i].value.value.i = VA_FOURCC_NV12;
       i++;
    }

    attribs[i].type = VASurfaceAttribMemoryType;
    attribs[i].value.type = VAGenericValueTypeInteger;
    attribs[i].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
    attribs[i].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA |
        VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    i++;

    attribs[i].type = VASurfaceAttribExternalBufferDescriptor;
    attribs[i].value.type = VAGenericValueTypePointer;
    attribs[i].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[i].value.value.p = NULL; /* ignore */
    i++;

    attribs[i].type = VASurfaceAttribMaxWidth;
    attribs[i].value.type = VAGenericValueTypeInteger;
    attribs[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attribs[i].value.value.i = vl_video_buffer_max_size(pscreen);
    i++;

    attribs[i].type = VASurfaceAttribMaxHeight;
    attribs[i].value.type = VAGenericValueTypeInteger;
    attribs[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attribs[i].value.value.i = vl_video_buffer_max_size(pscreen);
    i++;

    if (i > *num_attribs) {
        *num_attribs = i;
        FREE(attribs);
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }

    *num_attribs = i;
    memcpy(attrib_list, attribs, i * sizeof(VASurfaceAttrib));
    FREE(attribs);

    return vaStatus;
}

static VAStatus
suface_from_external_memory(VADriverContextP ctx, vlVaSurface *surface,
                            VASurfaceAttribExternalBuffers *memory_attibute,
                            int index, VASurfaceID *surfaces,
                            struct pipe_video_buffer *templat)
{
    vlVaDriver *drv = NULL;
    struct pipe_screen *pscreen = NULL;
    struct pipe_resource *resource = NULL;
    struct pipe_resource res_templ;
    struct winsys_handle whandle;
    struct pipe_resource *resources[VL_NUM_COMPONENTS];

    if (!ctx)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pscreen = VL_VA_PSCREEN(ctx);
    drv = VL_VA_DRIVER(ctx);

    if (!memory_attibute || !memory_attibute->buffers || index > memory_attibute->num_buffers)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (surface->templat.width != memory_attibute->width ||
        surface->templat.height != memory_attibute->height ||
        memory_attibute->num_planes < 1)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    switch (memory_attibute->pixel_format) {
    case VA_FOURCC_RGBA:
    case VA_FOURCC_RGBX:
    case VA_FOURCC_BGRA:
    case VA_FOURCC_BGRX:
        if (memory_attibute->num_planes != 1)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        break;
    default:
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    memset(&res_templ, 0, sizeof(res_templ));
    res_templ.target = PIPE_TEXTURE_2D;
    res_templ.last_level = 0;
    res_templ.depth0 = 1;
    res_templ.array_size = 1;
    res_templ.width0 = memory_attibute->width;
    res_templ.height0 = memory_attibute->height;
    res_templ.format = surface->templat.buffer_format;
    res_templ.bind = PIPE_BIND_SAMPLER_VIEW;
    res_templ.usage = PIPE_USAGE_DEFAULT;

    memset(&whandle, 0, sizeof(struct winsys_handle));
    whandle.type = DRM_API_HANDLE_TYPE_FD;
    whandle.handle = memory_attibute->buffers[index];
    whandle.stride = memory_attibute->pitches[index];

    resource = pscreen->resource_from_handle(pscreen, &res_templ, &whandle);

    if (!resource)
       return VA_STATUS_ERROR_ALLOCATION_FAILED;

    memset(resources, 0, sizeof resources);
    resources[0] = resource;

    surface->buffer = vl_video_buffer_create_ex2(drv->pipe, templat, resources);
    if (!surface->buffer)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    util_dynarray_init(&surface->subpics);
    surfaces[index] = handle_table_add(drv->htab, surface);

    if (!surfaces[index])
      return VA_STATUS_ERROR_ALLOCATION_FAILED;

    return VA_STATUS_SUCCESS;
}

VAStatus
vlVaCreateSurfaces2(VADriverContextP ctx, unsigned int format,
                    unsigned int width, unsigned int height,
                    VASurfaceID *surfaces, unsigned int num_surfaces,
                    VASurfaceAttrib *attrib_list, unsigned int num_attribs)
{
    vlVaDriver *drv = NULL;
    VASurfaceAttribExternalBuffers *memory_attibute = NULL;
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    struct pipe_video_buffer templat = {};
    struct pipe_screen *pscreen = NULL;
    int memory_type = VA_SURFACE_ATTRIB_MEM_TYPE_VA;
    int expected_fourcc = 0;
    int i = 0;

    if (!ctx)
       return VA_STATUS_ERROR_INVALID_CONTEXT;

    if (!(width && height))
       return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;

    drv = VL_VA_DRIVER(ctx);
    pscreen = VL_VA_PSCREEN(ctx);

    for (i = 0; i < num_attribs && attrib_list; i++) {
        if ((attrib_list[i].type == VASurfaceAttribPixelFormat) &&
            (attrib_list[i].flags & VA_SURFACE_ATTRIB_SETTABLE)) {
            if (attrib_list[i].value.type != VAGenericValueTypeInteger)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            expected_fourcc = attrib_list[i].value.value.i;
        }

        if ((attrib_list[i].type == VASurfaceAttribMemoryType) &&
            (attrib_list[i].flags & VA_SURFACE_ATTRIB_SETTABLE)) {

            if (attrib_list[i].value.type != VAGenericValueTypeInteger)
                return VA_STATUS_ERROR_INVALID_PARAMETER;

            switch (attrib_list[i].value.value.i) {
                case VA_SURFACE_ATTRIB_MEM_TYPE_VA:
                case VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME:
                    memory_type = attrib_list[i].value.value.i;
                    break;
                default:{
                    return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;}
            }
        }

        if ((attrib_list[i].type == VASurfaceAttribExternalBufferDescriptor) &&
            (attrib_list[i].flags == VA_SURFACE_ATTRIB_SETTABLE)) {
            if (attrib_list[i].value.type != VAGenericValueTypePointer)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            memory_attibute = (VASurfaceAttribExternalBuffers *)attrib_list[i].value.value.p;
        }
    }

    /* support 420 & 422 & RGB32 format, 422 and RGB32 are only used
     * for post-processing (including color conversion) */
    if (VA_RT_FORMAT_YUV420 != format &&
        VA_RT_FORMAT_YUV422 != format &&
        VA_RT_FORMAT_YUV444 != format &&
        VA_RT_FORMAT_YUV411 != format &&
        VA_RT_FORMAT_YUV400 != format &&
        VA_RT_FORMAT_RGB32  != format) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    switch (memory_type) {
        case VA_SURFACE_ATTRIB_MEM_TYPE_VA:
            break;
        case VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME:
            expected_fourcc = memory_attibute->pixel_format;
            break;
        default:
            assert(0);
    }

    if (expected_fourcc) {
       templat.buffer_format = YCbCrToPipe(expected_fourcc);
       templat.interlaced = 0;
    } else {
        templat.buffer_format = pscreen->get_video_param
        (
           pscreen,
           PIPE_VIDEO_PROFILE_UNKNOWN,
           PIPE_VIDEO_ENTRYPOINT_BITSTREAM,
           PIPE_VIDEO_CAP_PREFERED_FORMAT
        );
        templat.interlaced = pscreen->get_video_param
        (
           pscreen,
           PIPE_VIDEO_PROFILE_UNKNOWN,
           PIPE_VIDEO_ENTRYPOINT_BITSTREAM,
           PIPE_VIDEO_CAP_PREFERS_INTERLACED
        );
    }

    templat.chroma_format = ChromaToPipe(format);
    templat.width = width;
    templat.height = height;

    memset(surfaces, VA_INVALID_ID, num_surfaces * sizeof(VASurfaceID));

    for (i = 0; i < num_surfaces; i++) {
        vlVaSurface *surf = CALLOC(1, sizeof(vlVaSurface));
        if (!surf)
            goto no_res;

        surf->templat = templat;

        switch (memory_type) {
            case VA_SURFACE_ATTRIB_MEM_TYPE_VA:
                surf->buffer = drv->pipe->create_video_buffer(drv->pipe, &templat);
                if (!surf->buffer)
                    goto no_res;
                util_dynarray_init(&surf->subpics);
                surfaces[i] = handle_table_add(drv->htab, surf);
                break;
            case VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME:
                vaStatus = suface_from_external_memory(ctx, surf, memory_attibute, i, surfaces, &templat);
                if (vaStatus != VA_STATUS_SUCCESS)
                  goto no_res;
                break;
            default:
                assert(0);
        }
    }

    return vaStatus;

no_res:
   for (i = 0; i < num_surfaces; i++) {
      if (surfaces[i] != VA_INVALID_ID)
         vlVaDestroySurfaces(ctx, surfaces, i);
   }

   return VA_STATUS_ERROR_ALLOCATION_FAILED;
}

VAStatus
vlVaQueryVideoProcFilters(VADriverContextP ctx, VAContextID context,
                          VAProcFilterType *filters, unsigned int *num_filters)
{
   unsigned int num = 0;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   if (!num_filters || !filters)
      return VA_STATUS_ERROR_INVALID_PARAMETER;

   filters[num++] = VAProcFilterNone;

   *num_filters = num;

   return VA_STATUS_SUCCESS;
}

VAStatus
vlVaQueryVideoProcFilterCaps(VADriverContextP ctx, VAContextID context,
                             VAProcFilterType type, void *filter_caps,
                             unsigned int *num_filter_caps)
{
   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAProcColorStandardType vpp_input_color_standards[VAProcColorStandardCount] = {
   VAProcColorStandardBT601
};

static VAProcColorStandardType vpp_output_color_standards[VAProcColorStandardCount] = {
   VAProcColorStandardBT601
};

VAStatus
vlVaQueryVideoProcPipelineCaps(VADriverContextP ctx, VAContextID context,
                               VABufferID *filters, unsigned int num_filters,
                               VAProcPipelineCaps *pipeline_cap)
{
   unsigned int i = 0;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   if (!pipeline_cap)
   return VA_STATUS_ERROR_INVALID_PARAMETER;

   if (num_filters && !filters)
      return VA_STATUS_ERROR_INVALID_PARAMETER;

   pipeline_cap->pipeline_flags = 0;
   pipeline_cap->filter_flags = 0;
   pipeline_cap->num_forward_references = 0;
   pipeline_cap->num_backward_references = 0;
   pipeline_cap->num_input_color_standards = 1;
   pipeline_cap->input_color_standards = vpp_input_color_standards;
   pipeline_cap->num_output_color_standards = 1;
   pipeline_cap->output_color_standards = vpp_output_color_standards;

   for (i = 0; i < num_filters; i++) {
      vlVaBuffer *buf = handle_table_get(VL_VA_DRIVER(ctx)->htab, filters[i]);

      if (!buf || buf->type >= VABufferTypeMax)
         return VA_STATUS_ERROR_INVALID_BUFFER;
   }

   return VA_STATUS_SUCCESS;
}
