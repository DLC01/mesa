/*
 * Copyright © Microsoft Corporation
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "dxil_signature.h"
#include "dxil_enums.h"
#include "dxil_module.h"

#include "glsl_types.h"
#include "nir_to_dxil.h"
#include "util/u_debug.h"

#include <string.h>


struct semantic_info {
   enum dxil_semantic_kind kind;
   char name[64];
   int index;
   enum dxil_prog_sig_comp_type comp_type;
   uint8_t sig_comp_type;
   int32_t start_row;
   int32_t rows;
   uint8_t start_col;
   uint8_t cols;
   uint8_t interpolation;
   uint8_t stream;
   const char *sysvalue_name;
};


static bool
is_depth_output(enum dxil_semantic_kind kind)
{
   return kind == DXIL_SEM_DEPTH || kind == DXIL_SEM_DEPTH_GE ||
          kind == DXIL_SEM_DEPTH_LE || kind == DXIL_SEM_STENCIL_REF;
}

static uint8_t
get_interpolation(nir_variable *var)
{
   if (var->data.patch)
      return DXIL_INTERP_UNDEFINED;

   if (glsl_type_is_integer(glsl_without_array_or_matrix(var->type)))
      return DXIL_INTERP_CONSTANT;

   if (var->data.sample) {
      if (var->data.location == VARYING_SLOT_POS)
         return DXIL_INTERP_LINEAR_NOPERSPECTIVE_SAMPLE;
      switch (var->data.interpolation) {
      case INTERP_MODE_NONE: return DXIL_INTERP_LINEAR_SAMPLE;
      case INTERP_MODE_FLAT: return DXIL_INTERP_CONSTANT;
      case INTERP_MODE_NOPERSPECTIVE: return DXIL_INTERP_LINEAR_NOPERSPECTIVE_SAMPLE;
      case INTERP_MODE_SMOOTH: return DXIL_INTERP_LINEAR_SAMPLE;
      }
   } else if (unlikely(var->data.centroid)) {
      if (var->data.location == VARYING_SLOT_POS)
         return DXIL_INTERP_LINEAR_NOPERSPECTIVE_CENTROID;
      switch (var->data.interpolation) {
      case INTERP_MODE_NONE: return DXIL_INTERP_LINEAR_CENTROID;
      case INTERP_MODE_FLAT: return DXIL_INTERP_CONSTANT;
      case INTERP_MODE_NOPERSPECTIVE: return DXIL_INTERP_LINEAR_NOPERSPECTIVE_CENTROID;
      case INTERP_MODE_SMOOTH: return DXIL_INTERP_LINEAR_CENTROID;
      }
   } else {
      if (var->data.location == VARYING_SLOT_POS)
         return DXIL_INTERP_LINEAR_NOPERSPECTIVE;
      switch (var->data.interpolation) {
      case INTERP_MODE_NONE: return DXIL_INTERP_LINEAR;
      case INTERP_MODE_FLAT: return DXIL_INTERP_CONSTANT;
      case INTERP_MODE_NOPERSPECTIVE: return DXIL_INTERP_LINEAR_NOPERSPECTIVE;
      case INTERP_MODE_SMOOTH: return DXIL_INTERP_LINEAR;
      }
   }

   return DXIL_INTERP_LINEAR;
}

static const char *
in_sysvalue_name(nir_variable *var)
{
   switch (var->data.location) {
   case VARYING_SLOT_POS:
      return "POS";
   case VARYING_SLOT_FACE:
      return "FACE";
   default:
      return "NONE";
   }
}

/*
 * The signatures are written into the stream in two pieces:
 * DxilProgramSignatureElement is a fixes size structure that gets dumped
 * to the stream in order of the registers and each contains an offset
 * to the semantic name string. Then these strings are dumped into the stream.
 */
static unsigned
get_additional_semantic_info(nir_shader *s, nir_variable *var, struct semantic_info *info,
                             unsigned next_row, unsigned clip_size)
{
   const struct glsl_type *type = var->type;
   if (nir_is_arrayed_io(var, s->info.stage))
      type = glsl_get_array_element(type);

   info->comp_type =
      dxil_get_prog_sig_comp_type(type);

   bool is_depth = is_depth_output(info->kind);
   info->sig_comp_type = glsl_type_is_struct(type) ?
      DXIL_COMP_TYPE_U32 : dxil_get_comp_type(type);
   bool is_gs_input = s->info.stage == MESA_SHADER_GEOMETRY &&
      (var->data.mode & (nir_var_shader_in | nir_var_system_value));

   info->stream = var->data.stream;
   info->rows = 1;
   if (info->kind == DXIL_SEM_TARGET) {
      info->start_row = info->index;
      info->cols = (uint8_t)glsl_get_components(type);
   } else if (is_depth ||
              (info->kind == DXIL_SEM_PRIMITIVE_ID && is_gs_input) ||
              info->kind == DXIL_SEM_COVERAGE ||
              info->kind == DXIL_SEM_SAMPLE_INDEX) {
      // This turns into a 'N/A' mask in the disassembly
      info->start_row = -1;
      info->cols = 1;
   } else if (info->kind == DXIL_SEM_TESS_FACTOR ||
              info->kind == DXIL_SEM_INSIDE_TESS_FACTOR) {
      assert(var->data.compact);
      info->start_row = next_row;
      info->rows = glsl_get_aoa_size(type);
      info->cols = 1;
      next_row += info->rows;
   } else if (var->data.compact) {
      if (var->data.location_frac) {
         info->start_row = next_row - 1;
      } else {
         info->start_row = next_row;
         next_row++;
      }

      assert(glsl_type_is_array(type) && info->kind == DXIL_SEM_CLIP_DISTANCE);
      unsigned num_floats = glsl_get_aoa_size(type);
      unsigned start_offset = (var->data.location - VARYING_SLOT_CLIP_DIST0) * 4 +
         var->data.location_frac;

      if (start_offset >= clip_size) {
         info->kind = DXIL_SEM_CULL_DISTANCE;
         snprintf(info->name, 64, "SV_CullDistance");
      }
      info->cols = num_floats;
      info->start_col = (uint8_t)var->data.location_frac;
   } else {
      info->start_row = next_row;
      info->rows = glsl_count_vec4_slots(type, false, false);
      if (glsl_type_is_array(type))
         type = glsl_get_array_element(type);
      next_row += info->rows;
      info->start_col = (uint8_t)var->data.location_frac;
      info->cols = MIN2(glsl_get_component_slots(type), 4);
   }

   return next_row;
}

typedef void (*semantic_info_proc)(nir_variable *var, struct semantic_info *info, gl_shader_stage stage, bool vulkan);

static void
get_semantic_vs_in_name(nir_variable *var, struct semantic_info *info, gl_shader_stage stage, bool vulkan)
{
   strcpy(info->name, "TEXCOORD");
   if (vulkan) {
      info->index = var->data.location >= VERT_ATTRIB_GENERIC0 ?
         var->data.location - VERT_ATTRIB_GENERIC0 :
         var->data.location;
   } else {
      info->index = var->data.driver_location;
   }
   info->kind = DXIL_SEM_ARBITRARY;
}

static void
get_semantic_sv_name(nir_variable *var, struct semantic_info *info, gl_shader_stage stage, bool _vulkan)
{
   switch (var->data.location) {
   case SYSTEM_VALUE_VERTEX_ID_ZERO_BASE:
      info->kind = DXIL_SEM_VERTEX_ID;
      break;
   case SYSTEM_VALUE_FRONT_FACE:
      info->kind = DXIL_SEM_IS_FRONT_FACE;
      break;
   case SYSTEM_VALUE_INSTANCE_ID:
      info->kind = DXIL_SEM_INSTANCE_ID;
      break;
   case SYSTEM_VALUE_PRIMITIVE_ID:
      info->kind = DXIL_SEM_PRIMITIVE_ID;
      break;
   case SYSTEM_VALUE_SAMPLE_ID:
      info->kind = DXIL_SEM_SAMPLE_INDEX;
      info->interpolation = get_interpolation(var);
      break;
   default:
      unreachable("unsupported system value");
   }
   strncpy(info->name, var->name, ARRAY_SIZE(info->name) - 1);
}

static void
get_semantic_ps_outname(nir_variable *var, struct semantic_info *info)
{
   info->kind = DXIL_SEM_INVALID;
   switch (var->data.location) {
   case FRAG_RESULT_COLOR:
      snprintf(info->name, 64, "%s", "SV_Target");
      info->index = var->data.index;
      info->kind = DXIL_SEM_TARGET;
      break;
   case FRAG_RESULT_DATA0:
   case FRAG_RESULT_DATA1:
   case FRAG_RESULT_DATA2:
   case FRAG_RESULT_DATA3:
   case FRAG_RESULT_DATA4:
   case FRAG_RESULT_DATA5:
   case FRAG_RESULT_DATA6:
   case FRAG_RESULT_DATA7:
      snprintf(info->name, 64, "%s", "SV_Target");
      info->index = var->data.location - FRAG_RESULT_DATA0;
      if (var->data.location == FRAG_RESULT_DATA0 &&
          var->data.index > 0)
         info->index = var->data.index;
      info->kind = DXIL_SEM_TARGET;
      break;
   case FRAG_RESULT_DEPTH:
      snprintf(info->name, 64, "%s", "SV_Depth");
      info->kind = DXIL_SEM_DEPTH;
      break;
   case FRAG_RESULT_STENCIL:
      snprintf(info->name, 64, "%s", "SV_StencilRef");
      info->kind = DXIL_SEM_STENCIL_REF; //??
      break;
   case FRAG_RESULT_SAMPLE_MASK:
      snprintf(info->name, 64, "%s", "SV_Coverage");
      info->kind = DXIL_SEM_COVERAGE; //??
      break;
   default:
      snprintf(info->name, 64, "%s", "UNDEFINED");
      break;
   }
}

static void
get_semantic_name(nir_variable *var, struct semantic_info *info,
                  const struct glsl_type *type, bool vulkan)
{
   info->kind = DXIL_SEM_INVALID;
   info->interpolation = get_interpolation(var);
   switch (var->data.location) {

   case VARYING_SLOT_POS:
      assert(glsl_get_components(type) == 4);
      snprintf(info->name, 64, "%s", "SV_Position");
      info->kind = DXIL_SEM_POSITION;
      break;

    case VARYING_SLOT_FACE:
      assert(glsl_get_components(var->type) == 1);
      snprintf(info->name, 64, "%s", "SV_IsFrontFace");
      info->kind = DXIL_SEM_IS_FRONT_FACE;
      break;

   case VARYING_SLOT_PRIMITIVE_ID:
     assert(glsl_get_components(var->type) == 1);
     snprintf(info->name, 64, "%s", "SV_PrimitiveID");
     info->kind = DXIL_SEM_PRIMITIVE_ID;
     break;

   case VARYING_SLOT_CLIP_DIST1:
      info->index = 1;
      FALLTHROUGH;
   case VARYING_SLOT_CLIP_DIST0:
      assert(var->data.location == VARYING_SLOT_CLIP_DIST1 || info->index == 0);
      snprintf(info->name, 64, "%s", "SV_ClipDistance");
      info->kind = DXIL_SEM_CLIP_DISTANCE;
      break;

   case VARYING_SLOT_TESS_LEVEL_INNER:
      assert(glsl_get_components(var->type) <= 2);
      snprintf(info->name, 64, "%s", "SV_InsideTessFactor");
      info->kind = DXIL_SEM_INSIDE_TESS_FACTOR;
      break;

   case VARYING_SLOT_TESS_LEVEL_OUTER:
      assert(glsl_get_components(var->type) <= 4);
      snprintf(info->name, 64, "%s", "SV_TessFactor");
      info->kind = DXIL_SEM_TESS_FACTOR;
      break;

   default: {
         info->index =  vulkan ?
            var->data.location - VARYING_SLOT_VAR0 :
            var->data.driver_location;
         strcpy(info->name, "TEXCOORD");
         info->kind = DXIL_SEM_ARBITRARY;
      }
   }
}

static void
get_semantic_in_name(nir_variable *var, struct semantic_info *info, gl_shader_stage stage, bool vulkan)
{
   const struct glsl_type *type = var->type;
   if (nir_is_arrayed_io(var, stage) &&
       glsl_type_is_array(type))
      type = glsl_get_array_element(type);

   get_semantic_name(var, info, type, vulkan);
   info->sysvalue_name = in_sysvalue_name(var);
}


static enum dxil_prog_sig_semantic
prog_semantic_from_kind(enum dxil_semantic_kind kind, unsigned num_vals, unsigned start_val)
{
   switch (kind) {
   case DXIL_SEM_ARBITRARY: return DXIL_PROG_SEM_UNDEFINED;
   case DXIL_SEM_VERTEX_ID: return DXIL_PROG_SEM_VERTEX_ID;
   case DXIL_SEM_INSTANCE_ID: return DXIL_PROG_SEM_INSTANCE_ID;
   case DXIL_SEM_POSITION: return DXIL_PROG_SEM_POSITION;
   case DXIL_SEM_COVERAGE: return DXIL_PROG_SEM_COVERAGE;
   case DXIL_SEM_INNER_COVERAGE: return DXIL_PROG_SEM_INNER_COVERAGE;
   case DXIL_SEM_PRIMITIVE_ID: return DXIL_PROG_SEM_PRIMITIVE_ID;
   case DXIL_SEM_SAMPLE_INDEX: return DXIL_PROG_SEM_SAMPLE_INDEX;
   case DXIL_SEM_IS_FRONT_FACE: return DXIL_PROG_SEM_IS_FRONTFACE;
   case DXIL_SEM_RENDERTARGET_ARRAY_INDEX: return DXIL_PROG_SEM_RENDERTARGET_ARRAY_INDEX;
   case DXIL_SEM_VIEWPORT_ARRAY_INDEX: return DXIL_PROG_SEM_VIEWPORT_ARRAY_INDEX;
   case DXIL_SEM_CLIP_DISTANCE: return DXIL_PROG_SEM_CLIP_DISTANCE;
   case DXIL_SEM_CULL_DISTANCE: return DXIL_PROG_SEM_CULL_DISTANCE;
   case DXIL_SEM_BARYCENTRICS: return DXIL_PROG_SEM_BARYCENTRICS;
   case DXIL_SEM_SHADING_RATE: return DXIL_PROG_SEM_SHADING_RATE;
   case DXIL_SEM_CULL_PRIMITIVE: return DXIL_PROG_SEM_CULL_PRIMITIVE;
   case DXIL_SEM_TARGET: return DXIL_PROG_SEM_TARGET;
   case DXIL_SEM_DEPTH: return DXIL_PROG_SEM_DEPTH;
   case DXIL_SEM_DEPTH_LE: return DXIL_PROG_SEM_DEPTH_LE;
   case DXIL_SEM_DEPTH_GE: return DXIL_PROG_SEM_DEPTH_GE;
   case DXIL_SEM_STENCIL_REF: return DXIL_PROG_SEM_STENCIL_REF;
   case DXIL_SEM_TESS_FACTOR:
      switch (num_vals) {
      case 4: return DXIL_PROG_SEM_FINAL_QUAD_EDGE_TESSFACTOR;
      case 3: return DXIL_PROG_SEM_FINAL_TRI_EDGE_TESSFACTOR;
      case 2: return start_val == 0 ?
         DXIL_PROG_SEM_FINAL_LINE_DENSITY_TESSFACTOR :
         DXIL_PROG_SEM_FINAL_LINE_DETAIL_TESSFACTOR;
      default:
         unreachable("Invalid row count for tess factor");
      }
   case DXIL_SEM_INSIDE_TESS_FACTOR:
      switch (num_vals) {
      case 2: return DXIL_PROG_SEM_FINAL_QUAD_INSIDE_EDGE_TESSFACTOR;
      case 1: return DXIL_PROG_SEM_FINAL_TRI_INSIDE_EDGE_TESSFACTOR;
      default:
         unreachable("Invalid row count for inner tess factor");
      }
   default:
       return DXIL_PROG_SEM_UNDEFINED;
   }
}

static
uint32_t
copy_semantic_name_to_string(struct _mesa_string_buffer *string_out, const char *name)
{
   /*  copy the semantic name */
   uint32_t retval = string_out->length;
   size_t name_len = strlen(name) + 1;
   _mesa_string_buffer_append_len(string_out, name, name_len);
   return retval;
}

static
uint32_t
append_semantic_index_to_table(struct dxil_psv_sem_index_table *table, uint32_t index,
                               uint32_t num_rows)
{
   for (unsigned i = 0; i < table->size; ++i) {
      unsigned j = 0;
      for (; j < num_rows && i + j < table->size; ++j)
         if (table->data[i + j] != index + j)
            break;
      if (j == num_rows)
         return i;
      else if (j > 0)
         i += j - 1;
   }
   uint32_t retval = table->size;
   assert(table->size + num_rows <= 80);
   for (unsigned i = 0; i < num_rows; ++i)
      table->data[table->size++] = index + i;
   return retval;
}

static const struct dxil_mdnode *
fill_SV_param_nodes(struct dxil_module *mod, unsigned record_id,
                    struct semantic_info *semantic) {

   const struct dxil_mdnode *SV_params_nodes[11];
   /* For this to always work we should use vectorize_io, but for FS out and VS in
    * this is not implemented globally */
   const struct dxil_mdnode *flattened_semantics[256];

   for (unsigned i = 0; i < semantic->rows; ++i)
      flattened_semantics[i] = dxil_get_metadata_int32(mod, semantic->index + i);

   SV_params_nodes[0] = dxil_get_metadata_int32(mod, (int)record_id); // Unique element ID
   SV_params_nodes[1] = dxil_get_metadata_string(mod, semantic->name); // Element name
   SV_params_nodes[2] = dxil_get_metadata_int8(mod, semantic->sig_comp_type); // Element type
   SV_params_nodes[3] = dxil_get_metadata_int8(mod, (int8_t)semantic->kind); // Effective system value
   SV_params_nodes[4] = dxil_get_metadata_node(mod, flattened_semantics,
                                               semantic->rows); // Semantic index vector
   SV_params_nodes[5] = dxil_get_metadata_int8(mod, semantic->interpolation); // Interpolation mode
   SV_params_nodes[6] = dxil_get_metadata_int32(mod, semantic->rows); // Number of rows
   SV_params_nodes[7] = dxil_get_metadata_int8(mod, semantic->cols); // Number of columns
   SV_params_nodes[8] = dxil_get_metadata_int32(mod, semantic->start_row); // Element packing start row
   SV_params_nodes[9] = dxil_get_metadata_int8(mod, semantic->start_col); // Element packing start column

   const struct dxil_mdnode *SV_metadata[2];
   unsigned num_metadata_nodes = 0;
   if (semantic->stream != 0) {
      SV_metadata[num_metadata_nodes++] = dxil_get_metadata_int32(mod, DXIL_SIGNATURE_ELEMENT_OUTPUT_STREAM);
      SV_metadata[num_metadata_nodes++] = dxil_get_metadata_int32(mod, semantic->stream);
   }

   SV_params_nodes[10] = num_metadata_nodes ? dxil_get_metadata_node(mod, SV_metadata, num_metadata_nodes) : NULL;

   return dxil_get_metadata_node(mod, SV_params_nodes, ARRAY_SIZE(SV_params_nodes));
}

static void
fill_signature_element(struct dxil_signature_element *elm,
                       struct semantic_info *semantic,
                       unsigned row)
{
   memset(elm, 0, sizeof(struct dxil_signature_element));
   elm->stream = semantic->stream;
   // elm->semantic_name_offset = 0;  // Offset needs to be filled out when writing
   elm->semantic_index = semantic->index + row;
   elm->system_value = (uint32_t) prog_semantic_from_kind(semantic->kind, semantic->rows, row);
   elm->comp_type = (uint32_t) semantic->comp_type;
   elm->reg = semantic->start_row + row;

   assert(semantic->cols + semantic->start_col <= 4);
   elm->mask = (uint8_t) (((1 << semantic->cols) - 1) << semantic->start_col);
   // elm->never_writes_mask = 0;
   elm->min_precision = DXIL_MIN_PREC_DEFAULT;
}

static bool
fill_psv_signature_element(struct dxil_psv_signature_element *psv_elm,
                           struct semantic_info *semantic, struct dxil_module *mod)
{
   memset(psv_elm, 0, sizeof(struct dxil_psv_signature_element));
   psv_elm->rows = semantic->rows;
   if (semantic->start_row >= 0) {
      assert(semantic->start_row < 256);
      psv_elm->start_row = semantic->start_row;
      psv_elm->cols_and_start = (1u << 6) | (semantic->start_col << 4) | semantic->cols;
   } else {
      /* The validation expects that the the start row is not egative
       * and apparently the extra bit in the cols_and_start indicates that the
       * row is meant literally, so don't set it in this case.
       * (Source of information: Comparing with the validation structures
       * created by dxcompiler)
       */
      psv_elm->start_row = 0;
      psv_elm->cols_and_start = (semantic->start_col << 4) | semantic->cols;
   }
   psv_elm->semantic_kind = (uint8_t)semantic->kind;
   psv_elm->component_type = semantic->comp_type; //`??
   psv_elm->interpolation_mode = semantic->interpolation;
   psv_elm->dynamic_mask_and_stream = (semantic->stream) << 4;
   if (semantic->kind == DXIL_SEM_ARBITRARY && strlen(semantic->name)) {
      psv_elm->semantic_name_offset =
            copy_semantic_name_to_string(mod->sem_string_table, semantic->name);

      /* TODO: clean up memory */
      if (psv_elm->semantic_name_offset == (uint32_t)-1)
         return false;
   }

   psv_elm->semantic_indexes_offset =
         append_semantic_index_to_table(&mod->sem_index_table, semantic->index, semantic->rows);

   return true;
}

static bool
fill_io_signature(struct dxil_module *mod, int id,
                  struct semantic_info *semantic,
                  const struct dxil_mdnode **io,
                  struct dxil_signature_element *elm,
                  struct dxil_psv_signature_element *psv_elm)
{

   *io = fill_SV_param_nodes(mod, id, semantic);
   for (unsigned i = 0; i < semantic->rows; ++i)
      fill_signature_element(&elm[i], semantic, i);
   return fill_psv_signature_element(psv_elm, semantic, mod);
}

static unsigned
get_input_signature_group(struct dxil_module *mod, const struct dxil_mdnode **inputs,
                          unsigned num_inputs,
                          nir_shader *s, nir_variable_mode modes,
                          semantic_info_proc get_semantics, unsigned *row_iter, bool vulkan,
                          unsigned input_clip_size)
{
   nir_foreach_variable_with_modes(var, s, modes) {
      if (var->data.patch)
         continue;

      struct semantic_info semantic = {0};
      get_semantics(var, &semantic, s->info.stage, vulkan);
      mod->inputs[num_inputs].sysvalue = semantic.sysvalue_name;
      *row_iter = get_additional_semantic_info(s, var, &semantic, *row_iter, input_clip_size);

      if (semantic.start_row >= 0) {
         for (unsigned i = 0; i < semantic.rows; ++i)
            mod->input_mappings[semantic.start_row + i] = num_inputs;
      }

      mod->inputs[num_inputs].name = ralloc_strdup(mod->ralloc_ctx,
                                                   semantic.name);
      mod->inputs[num_inputs].num_elements = semantic.rows;
      struct dxil_signature_element *elm = mod->inputs[num_inputs].elements;
      struct dxil_psv_signature_element *psv_elm = &mod->psv_inputs[num_inputs];

      if (!fill_io_signature(mod, num_inputs, &semantic,
                             &inputs[num_inputs], elm, psv_elm))
         return 0;

      mod->num_psv_inputs = MAX2(mod->num_psv_inputs,
                                 semantic.start_row + semantic.rows);

      ++num_inputs;
      assert(num_inputs < VARYING_SLOT_MAX);
   }
   return num_inputs;
}

static const struct dxil_mdnode *
get_input_signature(struct dxil_module *mod, nir_shader *s, bool vulkan, unsigned input_clip_size)
{
   if (s->info.stage == MESA_SHADER_KERNEL)
      return NULL;

   const struct dxil_mdnode *inputs[VARYING_SLOT_MAX];
   unsigned next_row = 0;

   mod->num_sig_inputs = get_input_signature_group(mod, inputs, 0,
                                                   s, nir_var_shader_in,
                                                   s->info.stage == MESA_SHADER_VERTEX ?
                                                      get_semantic_vs_in_name : get_semantic_in_name,
                                                   &next_row, vulkan, input_clip_size);

   mod->num_sig_inputs = get_input_signature_group(mod, inputs, mod->num_sig_inputs,
                                                   s, nir_var_system_value,
                                                   get_semantic_sv_name,
                                                   &next_row, vulkan, input_clip_size);

   if (!mod->num_sig_inputs && !mod->num_sig_inputs)
      return NULL;

   const struct dxil_mdnode *retval = mod->num_sig_inputs ?
         dxil_get_metadata_node(mod, inputs, mod->num_sig_inputs) : NULL;

   return retval;
}

static const char *out_sysvalue_name(nir_variable *var)
{
   switch (var->data.location) {
   case VARYING_SLOT_FACE:
      return "FACE";
   case VARYING_SLOT_POS:
      return "POS";
   case VARYING_SLOT_CLIP_DIST0:
   case VARYING_SLOT_CLIP_DIST1:
      return "CLIPDST";
   case VARYING_SLOT_PRIMITIVE_ID:
      return "PRIMID";
   default:
      return "NO";
   }
}

static const struct dxil_mdnode *
get_output_signature(struct dxil_module *mod, nir_shader *s, bool vulkan)
{
   const struct dxil_mdnode *outputs[VARYING_SLOT_MAX];
   unsigned num_outputs = 0;
   unsigned next_row = 0;
   nir_foreach_variable_with_modes(var, s, nir_var_shader_out) {
      struct semantic_info semantic = {0};
      if (var->data.patch)
         continue;

      if (s->info.stage == MESA_SHADER_FRAGMENT) {
         get_semantic_ps_outname(var, &semantic);
         mod->outputs[num_outputs].sysvalue = "TARGET";
      } else {
         const struct glsl_type *type = var->type;
         if (nir_is_arrayed_io(var, s->info.stage))
            type = glsl_get_array_element(type);
         get_semantic_name(var, &semantic, type, vulkan);
         mod->outputs[num_outputs].sysvalue = out_sysvalue_name(var);
      }
      next_row = get_additional_semantic_info(s, var, &semantic, next_row, s->info.clip_distance_array_size);

      mod->info.has_out_position |= semantic.kind== DXIL_SEM_POSITION;
      mod->info.has_out_depth |= semantic.kind == DXIL_SEM_DEPTH;

      mod->outputs[num_outputs].name = ralloc_strdup(mod->ralloc_ctx,
                                                     semantic.name);
      mod->outputs[num_outputs].num_elements = semantic.rows;
      struct dxil_signature_element *elm = mod->outputs[num_outputs].elements;

      struct dxil_psv_signature_element *psv_elm = &mod->psv_outputs[num_outputs];

      if (!fill_io_signature(mod, num_outputs, &semantic,
                             &outputs[num_outputs], elm, psv_elm))
         return NULL;

      /* This is fishy, logic suggests that the LHS should be 0xf, but from the
       * validation it needs to be 0xff */
      for (unsigned i = 0; i < mod->outputs[num_outputs].num_elements; ++i)
         elm[i].never_writes_mask = 0xff & ~elm[i].mask;

      ++num_outputs;

      mod->num_psv_outputs[semantic.stream] = MAX2(mod->num_psv_outputs[semantic.stream],
                                                   semantic.start_row + semantic.rows);

      assert(num_outputs < ARRAY_SIZE(outputs));
   }

   if (!num_outputs)
      return NULL;

   const struct dxil_mdnode *retval = dxil_get_metadata_node(mod, outputs, num_outputs);
   mod->num_sig_outputs = num_outputs;
   return retval;
}

static const char *
patch_sysvalue_name(nir_variable *var)
{
   switch (var->data.location) {
   case VARYING_SLOT_TESS_LEVEL_OUTER:
      switch (glsl_get_aoa_size(var->type)) {
      case 4:
         return "QUADEDGE";
      case 3:
         return "TRIEDGE";
      case 2:
         return var->data.location_frac == 0 ?
            "LINEDET" : "LINEDEN";
      default:
         unreachable("Unexpected outer tess factor array size");
      }
      break;
   case VARYING_SLOT_TESS_LEVEL_INNER:
      switch (glsl_get_aoa_size(var->type)) {
      case 2:
         return "QUADINT";
      case 1:
         return "TRIINT";
      default:
         unreachable("Unexpected inner tess factory array size");
      }
      break;
   default:
      return "NO";
   }
}

static const struct dxil_mdnode *
get_patch_const_signature(struct dxil_module *mod, nir_shader *s, bool vulkan)
{
   if (s->info.stage != MESA_SHADER_TESS_CTRL &&
       s->info.stage != MESA_SHADER_TESS_EVAL)
      return NULL;

   const struct dxil_mdnode *patch_consts[VARYING_SLOT_MAX];
   nir_variable_mode mode = s->info.stage == MESA_SHADER_TESS_CTRL ?
      nir_var_shader_out : nir_var_shader_in;
   unsigned num_consts = 0;
   unsigned next_row = 0;
   nir_foreach_variable_with_modes(var, s, mode) {
      struct semantic_info semantic = {0};
      if (!var->data.patch)
         continue;

      const struct glsl_type *type = var->type;
      get_semantic_name(var, &semantic, type, vulkan);

      mod->patch_consts[num_consts].sysvalue = patch_sysvalue_name(var);
      next_row = get_additional_semantic_info(s, var, &semantic, next_row, 0);

      mod->patch_consts[num_consts].name = ralloc_strdup(mod->ralloc_ctx,
                                                         semantic.name);
      mod->patch_consts[num_consts].num_elements = semantic.rows;
      struct dxil_signature_element *elm = mod->patch_consts[num_consts].elements;

      struct dxil_psv_signature_element *psv_elm = &mod->psv_patch_consts[num_consts];

      if (!fill_io_signature(mod, num_consts, &semantic,
                             &patch_consts[num_consts], elm, psv_elm))
         return NULL;

      /* This is fishy, logic suggests that the LHS should be 0xf, but from the
       * validation it needs to be 0xff */
      if (mode == nir_var_shader_out)
         for (unsigned i = 0; i < mod->patch_consts[num_consts].num_elements; ++i)
            elm[i].never_writes_mask = 0xff & ~elm[i].mask;

      ++num_consts;

      mod->num_psv_patch_consts = MAX2(mod->num_psv_patch_consts,
                                       semantic.start_row + semantic.rows);

      assert(num_consts < ARRAY_SIZE(patch_consts));
   }

   if (!num_consts)
      return NULL;

   const struct dxil_mdnode *retval = dxil_get_metadata_node(mod, patch_consts, num_consts);
   mod->num_sig_patch_consts = num_consts;
   return retval;
}

const struct dxil_mdnode *
get_signatures(struct dxil_module *mod, nir_shader *s, bool vulkan, unsigned input_clip_size)
{
   /* DXC does the same: Add an empty string before everything else */
   mod->sem_string_table = _mesa_string_buffer_create(mod->ralloc_ctx, 1024);
   copy_semantic_name_to_string(mod->sem_string_table, "");

   const struct dxil_mdnode *input_signature = get_input_signature(mod, s, vulkan, input_clip_size);
   const struct dxil_mdnode *output_signature = get_output_signature(mod, s, vulkan);
   const struct dxil_mdnode *patch_const_signature = get_patch_const_signature(mod, s, vulkan);

   const struct dxil_mdnode *SV_nodes[3] = {
      input_signature,
      output_signature,
      patch_const_signature
   };
   if (output_signature || input_signature || patch_const_signature)
      return dxil_get_metadata_node(mod, SV_nodes, ARRAY_SIZE(SV_nodes));
   else
      return NULL;
}
