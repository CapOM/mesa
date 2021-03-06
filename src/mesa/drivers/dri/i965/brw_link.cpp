/*
 * Copyright © 2015 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "main/macros.h"
#include "brw_context.h"
#include "brw_vs.h"
#include "brw_gs.h"
#include "brw_fs.h"
#include "brw_cfg.h"
#include "brw_nir.h"
#include "glsl/ir_optimization.h"
#include "glsl/glsl_parser_extras.h"
#include "main/shaderapi.h"

/**
 * Performs a compile of the shader stages even when we don't know
 * what non-orthogonal state will be set, in the hope that it reflects
 * the eventual NOS used, and thus allows us to produce link failures.
 */
static bool
brw_shader_precompile(struct gl_context *ctx,
                      struct gl_shader_program *sh_prog)
{
   struct gl_shader *vs = sh_prog->_LinkedShaders[MESA_SHADER_VERTEX];
   struct gl_shader *gs = sh_prog->_LinkedShaders[MESA_SHADER_GEOMETRY];
   struct gl_shader *fs = sh_prog->_LinkedShaders[MESA_SHADER_FRAGMENT];
   struct gl_shader *cs = sh_prog->_LinkedShaders[MESA_SHADER_COMPUTE];

   if (fs && !brw_fs_precompile(ctx, sh_prog, fs->Program))
      return false;

   if (gs && !brw_gs_precompile(ctx, sh_prog, gs->Program))
      return false;

   if (vs && !brw_vs_precompile(ctx, sh_prog, vs->Program))
      return false;

   if (cs && !brw_cs_precompile(ctx, sh_prog, cs->Program))
      return false;

   return true;
}

static void
brw_lower_packing_builtins(struct brw_context *brw,
                           gl_shader_stage shader_type,
                           exec_list *ir)
{
   int ops = LOWER_PACK_SNORM_2x16
           | LOWER_UNPACK_SNORM_2x16
           | LOWER_PACK_UNORM_2x16
           | LOWER_UNPACK_UNORM_2x16;

   if (is_scalar_shader_stage(brw->intelScreen->compiler, shader_type)) {
      ops |= LOWER_UNPACK_UNORM_4x8
           | LOWER_UNPACK_SNORM_4x8
           | LOWER_PACK_UNORM_4x8
           | LOWER_PACK_SNORM_4x8;
   }

   if (brw->gen >= 7) {
      /* Gen7 introduced the f32to16 and f16to32 instructions, which can be
       * used to execute packHalf2x16 and unpackHalf2x16. For AOS code, no
       * lowering is needed. For SOA code, the Half2x16 ops must be
       * scalarized.
       */
      if (is_scalar_shader_stage(brw->intelScreen->compiler, shader_type)) {
         ops |= LOWER_PACK_HALF_2x16_TO_SPLIT
             |  LOWER_UNPACK_HALF_2x16_TO_SPLIT;
      }
   } else {
      ops |= LOWER_PACK_HALF_2x16
          |  LOWER_UNPACK_HALF_2x16;
   }

   lower_packing_builtins(ir, ops);
}

static void
process_glsl_ir(gl_shader_stage stage,
                struct brw_context *brw,
                struct gl_shader_program *shader_prog,
                struct gl_shader *shader)
{
   struct gl_context *ctx = &brw->ctx;
   const struct gl_shader_compiler_options *options =
      &ctx->Const.ShaderCompilerOptions[shader->Stage];

   /* Temporary memory context for any new IR. */
   void *mem_ctx = ralloc_context(NULL);

   ralloc_adopt(mem_ctx, shader->ir);

   /* lower_packing_builtins() inserts arithmetic instructions, so it
    * must precede lower_instructions().
    */
   brw_lower_packing_builtins(brw, shader->Stage, shader->ir);
   do_mat_op_to_vec(shader->ir);
   const int bitfield_insert = brw->gen >= 7 ? BITFIELD_INSERT_TO_BFM_BFI : 0;
   lower_instructions(shader->ir,
                      MOD_TO_FLOOR |
                      DIV_TO_MUL_RCP |
                      SUB_TO_ADD_NEG |
                      EXP_TO_EXP2 |
                      LOG_TO_LOG2 |
                      bitfield_insert |
                      LDEXP_TO_ARITH |
                      CARRY_TO_ARITH |
                      BORROW_TO_ARITH);

   /* Pre-gen6 HW can only nest if-statements 16 deep.  Beyond this,
    * if-statements need to be flattened.
    */
   if (brw->gen < 6)
      lower_if_to_cond_assign(shader->ir, 16);

   do_lower_texture_projection(shader->ir);
   brw_lower_texture_gradients(brw, shader->ir);
   do_vec_index_to_cond_assign(shader->ir);
   lower_vector_insert(shader->ir, true);
   lower_offset_arrays(shader->ir);
   brw_do_lower_unnormalized_offset(shader->ir);
   lower_noise(shader->ir);
   lower_quadop_vector(shader->ir, false);

   bool lowered_variable_indexing =
      lower_variable_index_to_cond_assign((gl_shader_stage)stage,
                                          shader->ir,
                                          options->EmitNoIndirectInput,
                                          options->EmitNoIndirectOutput,
                                          options->EmitNoIndirectTemp,
                                          options->EmitNoIndirectUniform);

   if (unlikely(brw->perf_debug && lowered_variable_indexing)) {
      perf_debug("Unsupported form of variable indexing in %s; falling "
                 "back to very inefficient code generation\n",
                 _mesa_shader_stage_to_abbrev(shader->Stage));
   }

   lower_ubo_reference(shader, shader->ir);

   bool progress;
   do {
      progress = false;

      if (is_scalar_shader_stage(brw->intelScreen->compiler, shader->Stage)) {
         brw_do_channel_expressions(shader->ir);
         brw_do_vector_splitting(shader->ir);
      }

      progress = do_lower_jumps(shader->ir, true, true,
                                true, /* main return */
                                false, /* continue */
                                false /* loops */
                                ) || progress;

      progress = do_common_optimization(shader->ir, true, true,
                                        options, ctx->Const.NativeIntegers) || progress;
   } while (progress);

   validate_ir_tree(shader->ir);

   /* Now that we've finished altering the linked IR, reparent any live IR back
    * to the permanent memory context, and free the temporary one (discarding any
    * junk we optimized away).
    */
   reparent_ir(shader->ir, shader->ir);
   ralloc_free(mem_ctx);

   if (ctx->_Shader->Flags & GLSL_DUMP) {
      fprintf(stderr, "\n");
      fprintf(stderr, "GLSL IR for linked %s program %d:\n",
              _mesa_shader_stage_to_string(shader->Stage),
              shader_prog->Name);
      _mesa_print_ir(stderr, shader->ir, NULL);
      fprintf(stderr, "\n");
   }
}

GLboolean
brw_link_shader(struct gl_context *ctx, struct gl_shader_program *shProg)
{
   struct brw_context *brw = brw_context(ctx);
   const struct brw_compiler *compiler = brw->intelScreen->compiler;
   unsigned int stage;

   for (stage = 0; stage < ARRAY_SIZE(shProg->_LinkedShaders); stage++) {
      struct gl_shader *shader = shProg->_LinkedShaders[stage];
      if (!shader)
	 continue;

      struct gl_program *prog =
	 ctx->Driver.NewProgram(ctx, _mesa_shader_stage_to_program(stage),
                                shader->Name);
      if (!prog)
	return false;
      prog->Parameters = _mesa_new_parameter_list();

      _mesa_copy_linked_program_data((gl_shader_stage) stage, shProg, prog);

      process_glsl_ir((gl_shader_stage) stage, brw, shProg, shader);

      /* Make a pass over the IR to add state references for any built-in
       * uniforms that are used.  This has to be done now (during linking).
       * Code generation doesn't happen until the first time this shader is
       * used for rendering.  Waiting until then to generate the parameters is
       * too late.  At that point, the values for the built-in uniforms won't
       * get sent to the shader.
       */
      foreach_in_list(ir_instruction, node, shader->ir) {
	 ir_variable *var = node->as_variable();

	 if ((var == NULL) || (var->data.mode != ir_var_uniform)
	     || (strncmp(var->name, "gl_", 3) != 0))
	    continue;

	 const ir_state_slot *const slots = var->get_state_slots();
	 assert(slots != NULL);

	 for (unsigned int i = 0; i < var->get_num_state_slots(); i++) {
	    _mesa_add_state_reference(prog->Parameters,
				      (gl_state_index *) slots[i].tokens);
	 }
      }

      do_set_program_inouts(shader->ir, prog, shader->Stage);

      prog->SamplersUsed = shader->active_samplers;
      prog->ShadowSamplers = shader->shadow_samplers;
      _mesa_update_shader_textures_used(shProg, prog);

      _mesa_reference_program(ctx, &shader->Program, prog);

      brw_add_texrect_params(prog);

      prog->nir = brw_create_nir(brw, shProg, prog, (gl_shader_stage) stage,
                                 is_scalar_shader_stage(compiler, stage));

      _mesa_reference_program(ctx, &prog, NULL);
   }

   if ((ctx->_Shader->Flags & GLSL_DUMP) && shProg->Name != 0) {
      for (unsigned i = 0; i < shProg->NumShaders; i++) {
         const struct gl_shader *sh = shProg->Shaders[i];
         if (!sh)
            continue;

         fprintf(stderr, "GLSL %s shader %d source for linked program %d:\n",
                 _mesa_shader_stage_to_string(sh->Stage),
                 i, shProg->Name);
         fprintf(stderr, "%s", sh->Source);
         fprintf(stderr, "\n");
      }
   }

   if (brw->precompile && !brw_shader_precompile(ctx, shProg))
      return false;

   return true;
}
