/* AIR to SPIR-V (air_spirv.h). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air_spirv.h"
#include "metal_compiler.h"
#include "compiler/nir/nir_builder.h"
#include "nir_to_spirv.h"

/* What NIR may hold when it reaches the emitter. Stela lowers to these options' wishes; the emitter takes NIR
   as spirv_to_nir would leave it. */
static const nir_shader_compiler_options options = {
    .max_unroll_iterations = 32,
    .lower_fdph = true,
    .lower_flrp32 = true,
    .lower_flrp64 = true,
    .lower_fsat = true,
    .lower_hadd = true,
    .lower_uadd_sat = true,
    .lower_usub_sat = true,
    .lower_iadd_sat = true,
    .lower_mul_high = true,
    .lower_uadd_carry = true,
    .lower_usub_borrow = true,
    .lower_bitfield_extract = false,
    .lower_bitfield_insert = false,
    .lower_extract_byte = true,
    .lower_extract_word = true,
    .lower_insert_byte = true,
    .lower_insert_word = true,
    .lower_pack_half_2x16 = false,
    .lower_unpack_half_2x16 = false,
    .lower_pack_64_2x32_split = false,
    .lower_unpack_64_2x32_split = false,
    .lower_scmp = true,
    .lower_int64_options = 0,
    .lower_doubles_options = (nir_lower_doubles_options)~0, /* Adreno 750 has no shaderFloat64 */
    .has_fsub = true,
    .has_isub = true,
    .support_16bit_alu = true,
};

static const struct stela_metal_target target = {
    .nir = &options,
    .max_instructions = 1u << 20,
    .max_threadgroup_bytes = 32768, /* Adreno 750 maxComputeSharedMemorySize */
    .max_private_bytes = 65536,
    .max_constant_bytes = 65536,
    .max_varying_slots = 32,
    .max_payload_bytes = 16384,
    .max_mesh_vertices = 256,
    .max_mesh_primitives = 256,
    .stencil_export = 1, /* VK_EXT_shader_stencil_export */
    .raster_order_groups = 0, /* no VK_EXT_fragment_shader_interlock */
};

/* Push constants: Stela reads them with load_push_constant at byte offsets (metal_abi.h STELA_METAL_PUSH_*); the
   emitter reads a push constant block variable through derefs: menhir_push.words[index], 32 bits each. */
static bool lower_push_constant(nir_builder *b, nir_intrinsic_instr *intr, void *data) {
    nir_variable *block = data;
    if (intr->intrinsic != nir_intrinsic_load_push_constant) return false;
    b->cursor = nir_before_instr(&intr->instr);
    unsigned bits = intr->def.bit_size, n = intr->def.num_components;
    unsigned words = n * bits / 32;
    if (bits < 32 || !nir_src_is_const(intr->src[0])) return false; /* left for the emitter to refuse */
    unsigned at = (nir_intrinsic_base(intr) + nir_src_as_uint(intr->src[0])) / 4;
    nir_deref_instr *array = nir_build_deref_struct(b, nir_build_deref_var(b, block), 0);
    nir_def *parts[NIR_MAX_VEC_COMPONENTS];
    for (unsigned i = 0; i < words; i++) parts[i] = nir_load_deref(b, nir_build_deref_array_imm(b, array, at + i));
    nir_def *v = nir_vec(b, parts, words);
    if (bits == 64) v = nir_pack_64_2x32(b, v);
    nir_def_replace(&intr->def, v);
    return true;
}

static nir_variable *push_block(nir_shader *nir) {
    struct glsl_struct_field field = {.type = glsl_array_type(glsl_uint_type(), STELA_METAL_PUSH_BYTES / 4, 4), .name = "words", .offset = 0};
    nir_variable *v = nir_variable_create(nir, nir_var_mem_push_const, glsl_struct_type(&field, 1, "menhir_push", false), "menhir_push");
    v->data.location = INT_MAX;
    return v;
}

/* f2f16 rounding to nearest even is Vulkan's (and Adreno's) rounding for OpFConvert anyway. */
static bool lower_rtne(nir_builder *b, nir_alu_instr *alu, void *data) {
    (void)b, (void)data;
    if (alu->op != nir_op_f2f16_rtne) return false;
    alu->op = nir_op_f2f16;
    return true;
}

static int compile(const void *bitcode, size_t size, const char *entry, const AirSpirvOptions *opt, AirSpirv *out);

int air_compile_spirv(const void *bitcode, size_t size, const char *entry, const AirSpirvOptions *opt, AirSpirv *out) {
    memset(out, 0, sizeof *out);
    glsl_type_singleton_init_or_ref(); /* the push block's types are made after Stela's compile has let go of its own */
    int rc = compile(bitcode, size, entry, opt, out);
    glsl_type_singleton_decref();
    return rc;
}

static int compile(const void *bitcode, size_t size, const char *entry, const AirSpirvOptions *opt, AirSpirv *out) {
    struct stela_air_error error = {0};
    int print_nir = opt->print_nir;
    /* The varyings, in the order both stages place them: the linking function's. */
    static struct stela_metal_reflection reflection; /* 18 KB; the backend compiles on one thread */
    const void *lb = opt->link_bitcode ? opt->link_bitcode : bitcode;
    size_t ls = opt->link_bitcode ? opt->link_size : size;
    const char *le = opt->link_bitcode ? opt->link_entry : entry;
    if (!stela_metal_reflect(lb, ls, le, NULL, 0, &reflection, &error)) {
        snprintf(out->error, sizeof out->error, "reflection of %s: error %d: %s", le, (int)error.kind, error.detail);
        return -1;
    }
    struct stela_metal_linked_varying linked[STELA_METAL_VARYINGS];
    uint32_t nv = reflection.varying_count < STELA_METAL_VARYINGS ? reflection.varying_count : STELA_METAL_VARYINGS;
    for (uint32_t i = 0; i < nv; i++) {
        const struct stela_metal_varying *v = &reflection.varyings[i];
        linked[i] = (struct stela_metal_linked_varying){reflection.names + v->identifier, v->identifier_bytes, v->per_primitive};
    }
    struct stela_metal_specialization spec = {.varyings = linked, .varying_count = nv};
    for (int i = 0; i < 3; i++) spec.workgroup_size[i] = opt->workgroup[i] ? opt->workgroup[i] : 1;
    nir_shader *nir = stela_metal_compile(bitcode, size, entry, &target, &spec, &error);
    if (!nir) {
        snprintf(out->error, sizeof out->error, "AIR to NIR: error %d at instruction %u: %s", (int)error.kind, error.instruction, error.detail);
        return -1;
    }
    if (print_nir) nir_print_shader(nir, stdout);
    nir_variable *block = push_block(nir);
    bool pushes = false;
    NIR_PASS(pushes, nir, nir_shader_intrinsics_pass, lower_push_constant, nir_metadata_control_flow, block);
    if (!pushes) exec_node_remove(&block->node);
    NIR_PASS(_, nir, nir_shader_alu_pass, lower_rtne, nir_metadata_control_flow, NULL);
    NIR_PASS(_, nir, nir_lower_fp16_casts, nir_lower_fp16_rtz | nir_lower_fp16_ru | nir_lower_fp16_rd);
    ntv_shader_prepare(nir);
    if (print_nir) {
        printf("---- prepared for the emitter ----\n");
        nir_print_shader(nir, stdout);
    }
    const struct ntv_info info = {
        .have_vulkan_memory_model = true,
        .has_demote_to_helper = true,
        .is_native_vulkan = true,
        .have_shader_output_layer = true,
        .have_shader_viewport_index_layer = false,
        .spirv_version = SPIRV_VERSION(1, 6),
        .bindless_set_idx = 0,
    };
    struct spirv_shader *spirv = nir_to_spirv(nir, &info);
    stela_metal_release(nir);
    if (!spirv) {
        snprintf(out->error, sizeof out->error, "NIR to SPIR-V failed");
        return -1;
    }
    out->words = malloc(spirv->num_words * 4);
    memcpy(out->words, spirv->words, spirv->num_words * 4);
    out->count = spirv->num_words;
    spirv_shader_delete(spirv);
    return 0;
}

void air_spirv_free(AirSpirv *s) {
    free(s->words);
    s->words = NULL, s->count = 0;
}
