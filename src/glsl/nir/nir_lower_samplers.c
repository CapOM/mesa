/*
 * Copyright (C) 2005-2007  Brian Paul   All Rights Reserved.
 * Copyright (C) 2008  VMware, Inc.   All Rights Reserved.
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "nir.h"
#include "nir_builder.h"
#include "../program.h"
#include "program/hash_table.h"
#include "ir_uniform.h"

#include "main/compiler.h"
#include "main/mtypes.h"
#include "program/prog_parameter.h"
#include "program/program.h"

/* Calculate the sampler index based on array indicies and also
 * calculate the base uniform location for struct members.
 */
static void
calc_sampler_offsets(nir_deref *tail, nir_tex_instr *instr,
                     unsigned *array_elements, nir_ssa_def **indirect,
                     nir_builder *b, unsigned *location)
{
   if (tail->child == NULL)
      return;

   switch (tail->child->deref_type) {
   case nir_deref_type_array: {
      nir_deref_array *deref_array = nir_deref_as_array(tail->child);

      assert(deref_array->deref_array_type != nir_deref_array_type_wildcard);

      calc_sampler_offsets(tail->child, instr, array_elements,
                           indirect, b, location);
      instr->sampler_index += deref_array->base_offset * *array_elements;

      if (deref_array->deref_array_type == nir_deref_array_type_indirect) {
         nir_ssa_def *mul =
            nir_imul(b, nir_imm_int(b, *array_elements),
                     nir_ssa_for_src(b, deref_array->indirect, 1));

         nir_instr_rewrite_src(&instr->instr, &deref_array->indirect,
                               NIR_SRC_INIT);

         if (*indirect) {
            *indirect = nir_iadd(b, *indirect, mul);
         } else {
            *indirect = mul;
         }
      }

      *array_elements *= glsl_get_length(tail->type);
       break;
   }

   case nir_deref_type_struct: {
      nir_deref_struct *deref_struct = nir_deref_as_struct(tail->child);
      *location += glsl_get_record_location_offset(tail->type, deref_struct->index);
      calc_sampler_offsets(tail->child, instr, array_elements,
                           indirect, b, location);
      break;
   }

   default:
      unreachable("Invalid deref type");
      break;
   }
}

static void
lower_sampler(nir_tex_instr *instr, const struct gl_shader_program *shader_program,
              gl_shader_stage stage, nir_builder *builder)
{
   if (instr->sampler == NULL)
      return;

   instr->sampler_index = 0;
   unsigned location = instr->sampler->var->data.location;
   unsigned array_elements = 1;
   nir_ssa_def *indirect = NULL;

   builder->cursor = nir_before_instr(&instr->instr);
   calc_sampler_offsets(&instr->sampler->deref, instr, &array_elements,
                        &indirect, builder, &location);

   if (indirect) {
      /* First, we have to resize the array of texture sources */
      nir_tex_src *new_srcs = rzalloc_array(instr, nir_tex_src,
                                            instr->num_srcs + 1);

      for (unsigned i = 0; i < instr->num_srcs; i++) {
         new_srcs[i].src_type = instr->src[i].src_type;
         nir_instr_move_src(&instr->instr, &new_srcs[i].src,
                            &instr->src[i].src);
      }

      ralloc_free(instr->src);
      instr->src = new_srcs;

      /* Now we can go ahead and move the source over to being a
       * first-class texture source.
       */
      instr->src[instr->num_srcs].src_type = nir_tex_src_sampler_offset;
      instr->num_srcs++;
      nir_instr_rewrite_src(&instr->instr,
                            &instr->src[instr->num_srcs - 1].src,
                            nir_src_for_ssa(indirect));

      instr->sampler_array_size = array_elements;
   }

   if (location > shader_program->NumUniformStorage - 1 ||
       !shader_program->UniformStorage[location].opaque[stage].active) {
      assert(!"cannot return a sampler");
      return;
   }

   instr->sampler_index +=
      shader_program->UniformStorage[location].opaque[stage].index;

   instr->sampler = NULL;
}

typedef struct {
   nir_builder builder;
   const struct gl_shader_program *shader_program;
   gl_shader_stage stage;
} lower_state;

static bool
lower_block_cb(nir_block *block, void *_state)
{
   lower_state *state = (lower_state *) _state;

   nir_foreach_instr(block, instr) {
      if (instr->type == nir_instr_type_tex) {
         nir_tex_instr *tex_instr = nir_instr_as_tex(instr);
         lower_sampler(tex_instr, state->shader_program, state->stage,
                       &state->builder);
      }
   }

   return true;
}

static void
lower_impl(nir_function_impl *impl, const struct gl_shader_program *shader_program,
           gl_shader_stage stage)
{
   lower_state state;

   nir_builder_init(&state.builder, impl);
   state.shader_program = shader_program;
   state.stage = stage;

   nir_foreach_block(impl, lower_block_cb, &state);
}

void
nir_lower_samplers(nir_shader *shader,
                   const struct gl_shader_program *shader_program)
{
   nir_foreach_overload(shader, overload) {
      if (overload->impl)
         lower_impl(overload->impl, shader_program, shader->stage);
   }
}
