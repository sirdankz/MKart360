#ifndef GFX_CC_H
#define GFX_CC_H

#include <stdint.h>
#include "gfx_bool.h"

enum {
    CC_0,
    CC_TEXEL0,
    CC_TEXEL1,
    CC_PRIM,
    CC_SHADE,
    CC_ENV,
    CC_TEXEL0A,
    CC_LOD,
    CC_ONE,
    CC_PRIMA, CC_COMBINED, CC_COMBINEDA, CC_TEXEL1A, CC_SHADEA, CC_ENVA, CC_PRIMLOD
};

enum {
    SHADER_0,
    SHADER_INPUT_1,
    SHADER_INPUT_2,
    SHADER_INPUT_3,
    SHADER_INPUT_4,
    SHADER_TEXEL0,
    SHADER_TEXEL0A,
    SHADER_TEXEL1
};

#define SHADER_OPT_ALPHA (1 << 24)
#define SHADER_OPT_FOG (1 << 25)
#define SHADER_OPT_TEXTURE_EDGE (1 << 26)
#define SHADER_OPT_NOISE (1 << 27)
#define SHADER_OPT_2CYCLE (1 << 28)
#define SHADER_OPT_PRESERVE_ALPHA (1 << 29)

struct CCFeatures {
    uint8_t c[2][4];
    uint8_t c2[2][4];
    bool two_cycle;
    bool opt_alpha;
    bool opt_fog;
    bool opt_texture_edge;
    bool opt_noise;
    bool opt_preserve_alpha;
    bool used_textures[2];
    int num_inputs;
    bool do_single[2];
    bool do_multiply[2];
    bool do_mix[2];
    bool color_alpha_same;
};

#ifdef __cplusplus
extern "C" {
#endif

uint32_t gfx_cc_register(uint64_t combine,uint32_t options);
void gfx_cc_get_features(uint32_t shader_id, struct CCFeatures *cc_features);

#ifdef __cplusplus
}
#endif

#endif
