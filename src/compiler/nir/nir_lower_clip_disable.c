/*
 * Copyright © 2020 Mike Blumenkrantz
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
 * 
 * Authors:
 *    Mike Blumenkrantz <michael.blumenkrantz@gmail.com>
 */

#include "nir.h"
#include "nir_builder.h"


/**
 * This pass uses the enabled clip planes from the rasterizer state to rewrite
 * vertex shader store operations and store an undef to the corresponding gl_ClipDistance[n]
 * value if the plane is disabled
 */

/* recursively nest if/else blocks until we get to an array index,
 * then overwrite it if that plane isn't enabled
 */
static void
recursive_if_chain(nir_builder *b, nir_deref_instr *deref, nir_ssa_def *value, unsigned clip_plane_enable, nir_ssa_def *index, unsigned start, unsigned end)
{
   if (start == end - 1) {
      /* store the original value again if the clip plane is enabled */
      if (clip_plane_enable & (1 << start))
         nir_store_deref(b, deref, value, 1 << start);
      else
         nir_store_deref(b, deref, nir_ssa_undef(b, 1, 32), 1 << start);
      return;
   }

   unsigned mid = start + (end - start) / 2;
   nir_push_if(b, nir_ilt(b, index, nir_imm_int(b, mid)));
   recursive_if_chain(b, deref, value, clip_plane_enable, index, start, mid);
   nir_push_else(b, NULL);
   recursive_if_chain(b, deref, value, clip_plane_enable, index, mid, end);
   nir_pop_if(b, NULL);
}

/* vulkan (and some drivers) provides no concept of enabling clip planes through api,
 * so we rewrite disabled clip planes to an undefined value in order to disable them
 */
static bool
lower_clip_plane_store(nir_intrinsic_instr *instr, unsigned clip_plane_enable, nir_builder *b)
{
   nir_variable *out;
   unsigned plane;

   if (instr->intrinsic != nir_intrinsic_store_deref)
      return false;

   nir_deref_instr *deref = nir_src_as_deref(instr->src[0]);

   out = nir_deref_instr_get_variable(deref);
   if ((out->data.location != VARYING_SLOT_CLIP_DIST0) ||
        out->data.mode != nir_var_shader_out)
      return false;

   b->cursor = nir_after_instr(&instr->instr);
   if (nir_src_is_const(deref->arr.index)) {
      /* storing using a constant index */
      plane = nir_src_as_uint(deref->arr.index);
      /* no need to make changes if the clip plane is enabled */
      if (clip_plane_enable & (1 << plane))
         return false;

      nir_store_deref(b, deref, nir_ssa_undef(b, 1, 32), 1 << plane);
   } else {
      /* storing using a variable index */
      nir_ssa_def *index = nir_ssa_for_src(b, deref->arr.index, 1);
      unsigned length = glsl_get_length(nir_deref_instr_parent(deref)->type);

      recursive_if_chain(b, deref, instr->src[1].ssa, clip_plane_enable, index, 0, length);
   }
   nir_instr_remove(&instr->instr);
   return true;
}

bool
nir_lower_clip_disable(nir_shader *shader, unsigned clip_plane_enable)
{
   bool progress = false;

   nir_foreach_shader_out_variable(var, shader) {
      if (var->data.location == VARYING_SLOT_CLIP_DIST0) {
         unsigned size = glsl_get_length(var->type);
         /* if currently-enabled planes match used planes then no-op */
         if (clip_plane_enable == (1u << size) - 1)
            return false;
      }
   }

   nir_foreach_function(function, shader) {
      if (function->impl) {
         nir_builder builder;
         nir_builder_init(&builder, function->impl);
         nir_foreach_block(block, function->impl) {
            nir_foreach_instr_safe(instr, block) {
               if (instr->type == nir_instr_type_intrinsic)
                  progress |= lower_clip_plane_store(nir_instr_as_intrinsic(instr),
                                                     clip_plane_enable,
                                                     &builder);
            }
         }

         nir_metadata_preserve(function->impl, nir_metadata_block_index | nir_metadata_dominance);
      }
   }

   return progress;
}
