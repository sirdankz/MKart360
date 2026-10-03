#include <xtl.h>
#include "xbox360/netplay.h"
#include "xbox360/assets.h"
#ifdef __cplusplus
extern "C" {
#endif
extern uintptr_t gSegmentTable[16];
#ifdef __cplusplus
}
#endif
static uintptr_t rspSegments[16];
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#if defined(_MSC_VER) && !defined(restrict)
#define restrict __restrict
#endif
#include <assert.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include "gfx_pc.h"
#include "netplay.h"
#include "netplay_view.h"
#include "online_hud.h"
int x360_gfx_online_hud = 0;
extern "C" { extern int gGamestate,gActiveScreenMode,gPlayerCountSelection1; }
#include "gfx_cc.h"
#include "gfx_window_manager_api.h"
#include "gfx_rendering_api.h"

#ifdef __cplusplus
extern "C" void x360_log(const char *message);
#else
extern void x360_log(const char *message);
#endif


#define SUPPORT_CHECK(x) assert(x)

// SCALE_M_N: upscale/downscale M-bit integer to N-bit
#define SCALE_5_8(VAL_) (((VAL_) * 0xFF) / 0x1F)
#define SCALE_8_5(VAL_) ((((VAL_) + 4) * 0x1F) / 0xFF)
#define SCALE_4_8(VAL_) ((VAL_) * 0x11)
#define SCALE_8_4(VAL_) ((VAL_) / 0x11)
#define SCALE_3_8(VAL_) ((VAL_) * 0x24)
#define SCALE_8_3(VAL_) ((VAL_) / 0x24)

#define SCREEN_WIDTH 320 /* MK64 native coordinate space */
#define SCREEN_HEIGHT 240 /* MK64 native coordinate space */
#define HALF_SCREEN_WIDTH (SCREEN_WIDTH / 2)
#define HALF_SCREEN_HEIGHT (SCREEN_HEIGHT / 2)

#define MAX_BUFFERED 256
#define MAX_LIGHTS 2
#define MAX_VERTICES 64

// clip triangles for the software rasterizer in advance
#define GFX_MANUAL_CLIPPING 1

#ifdef ENABLE_SOFTRAST
// leave colors as 0-255 floats
#define GFX_DONT_SCALE_COLORS 1
// don't put in fog color
#define GFX_NO_FOG_COLOR 1
// premultiply by W
#define GFX_W_PREMULT 1
#endif

#ifdef GFX_DONT_SCALE_COLORS
#define GFX_COLOR_ONE 255.f
#define GFX_COLOR_CONVERT(x) (x)
#else
#define GFX_COLOR_ONE 1.f
#define GFX_COLOR_CONVERT(x) (x / 255.f)
#endif

#ifdef GFX_W_PREMULT
#define GFX_OUT_PROP(x) ((x) * w_inv)
#else
#define GFX_OUT_PROP(x) (x)
#endif

enum {
    CLIP_NONE   = 0,
    CLIP_NEAR   = 1,
    CLIP_FAR    = 2,
    CLIP_TOP    = 4,
    CLIP_BOTTOM = 8,
    CLIP_RIGHT  = 16,
    CLIP_LEFT   = 32,
    CLIP_ALL    = 63,
};

struct RGBA {
    uint8_t r, g, b, a;
};

struct XYWidthHeight {
    uint16_t x, y, width, height;
};

struct LoadedVertex {
    float x, y, z, w;
    float u, v;
    struct RGBA color;
    uint8_t clip_rej;
};

struct TextureHashmapNode {
    struct TextureHashmapNode *next;

    const uint8_t *texture_addr;
    uint8_t fmt, siz, texture_unit, ci_palette;
    uint32_t tlut_mode;
    uint32_t width, height, source_size, line_bytes;
    uint32_t content_hash;
    /* B17G7C CI8-STABLE-CACHE: CI8 GPU pixels also depend on the active TLUT. */
    uint32_t palette_hash;

    uint32_t texture_id;
    /* Hash da textura HD que esta na GPU neste no (0 = nenhuma). */
    uint32_t x360_hd_loaded;
    /* Hash HD pedida a thread de leitura para este no (0 = nenhuma): quando
       chegar, o no e reimportado para receber a versao HD. */
    uint32_t x360_hd_pending;
    uint8_t cms, cmt;
    bool linear_filter;
};
/* Cache de texturas ampliado (era 512 entradas / mapa de 1024).
   Quando o pool enche, o cache inteiro e descartado de uma vez -- e cada
   textura reimportada depois disso relê o .tex HD do disco. Com os sprites
   de kart (centenas de quadros distintos em rotacao numa corrida) isso
   acontecia varias vezes por segundo e era a causa dos engasgos.
   Custo: ~64 bytes por entrada, ou seja poucas centenas de KB. */
static struct {
    struct TextureHashmapNode *hashmap[2048];
    struct TextureHashmapNode pool[1024];
    uint32_t pool_pos;
} gfx_texture_cache;

static struct TextureHashmapNode x360_invalid_texture_node[2];
static bool x360_invalid_texture_ready[2];

struct ColorCombiner {
    uint64_t cc_id;
    struct ShaderProgram *prg;
    uint32_t options;
};

static struct ColorCombiner color_combiner_pool[64];
static unsigned color_combiner_pool_size;

static struct RSP {
    float modelview_matrix_stack[11][4][4];
    uint8_t modelview_matrix_stack_size;

    float MP_matrix[4][4];
    float P_matrix[4][4];

    Light_t current_lights[MAX_LIGHTS + 1];
    Light_t lookat[2];
    bool lookat_enabled;
    float current_lights_coeffs[MAX_LIGHTS][3];
    float current_lookat_coeffs[2][3]; // lookat_x, lookat_y
    uint8_t current_num_lights; // includes ambient light
    bool lights_changed;

    uint32_t geometry_mode;
    int16_t fog_mul, fog_offset;

    struct {
        // U0.16
        uint16_t s, t;
    } texture_scaling_factor;

    struct LoadedVertex loaded_vertices[MAX_VERTICES + 4];
} rsp;

struct X360LoadedTexture {
    const uint8_t *addr;
    uint32_t size_bytes;
    uint32_t source_pitch, row_bytes, rows;
    /* B17G6D: hash captured exactly when the N64 load command executes. */
    uint32_t load_capture_hash;
    uint32_t generation, command;
    uintptr_t command_address;
    uint32_t load_words[2];
    uint16_t tmem;
    uint8_t load_tile;
    /* B17G7D LOADBLOCK-DXT: exact 12-bit G_LOADBLOCK dxt field. */
    uint16_t load_dxt;
};
struct X360TextureTile {
    uint8_t fmt, siz, cms, cmt;
    uint8_t palette, masks, maskt, shifts, shiftt;
    uint16_t uls, ult, lrs, lrt, tmem;
    uint32_t line_size_bytes;
};

static struct RDP {
    const uint8_t *palette;
    uint8_t palette_data[512]; /* RDP TLUT storage, independent of source RAM. */
    uint32_t palette_generation, palette_bytes;
    /* B17G6D: palette hash captured exactly at LOADTLUT. */
    uint32_t palette_load_capture_hash;
    struct {
        const uint8_t *addr;
        uint8_t siz;
        uint8_t tile_number;
        uint32_t width;
    } texture_to_load;
    struct X360LoadedTexture loaded_texture[2];
    /* B14c: RDP tiles are independent; tile 6 is also a valid load tile. */
    struct X360TextureTile tiles[8];
    uint8_t render_tile;
    bool textures_changed[2];

    uint32_t other_mode_l, other_mode_h;
    uint64_t combine_mode;
    uint8_t prim_lod_fraction;

    struct RGBA env_color, prim_color, fog_color, fill_color;
    struct XYWidthHeight viewport, scissor;
    bool viewport_or_scissor_changed;
    void *z_buf_address;
    void *color_image_address;
} rdp;

static struct X360TextureTile *gfx_texture_tile(int unit) {
    return &rdp.tiles[(rdp.render_tile + unit) & 7];
}
static struct X360LoadedTexture *gfx_loaded_texture(int unit) {
    return &rdp.loaded_texture[gfx_texture_tile(unit)->tmem / 256];
}

/* B15a: bounded provenance, plus full affected-region fingerprints.
 * Reused RAM addresses are not immutable texture identities. */
#ifdef __cplusplus
extern "C" {
#endif
extern int gGamestate;
extern float gCourseTimer; /* B17G13: same timer rendered by HUD */
extern int gMenuSelection;
#ifdef __cplusplus
}
#endif
/* Eight unique submitted startup vertices; no gameplay tracing. */
/* B18: sampler-slot state, palette banks, CI cache isolation, crop dimensions and UV shifts. */
static unsigned x360_logo_count;
static struct { int valid, normal[3]; float dot[2]; } x360_logo_vertex[MAX_VERTICES + 4];
static uint32_t x360_texture_generation;
static uint32_t x360_texture_commands[5][2];
static uintptr_t x360_texture_command_address[5];
static unsigned x360_texture_trace_count, x360_texture_reject_count;
static unsigned x360_texture_initial_count, x360_texture_lakitu_count, x360_texture_changed_count;
static uint32_t x360_texture_seen[16][2];
static uint32_t x360_texture_hash(const uint8_t *data, uint32_t size) {
    uint32_t h = 2166136261U;
    for (uint32_t i = 0; i < size; ++i) h = (h ^ data[i]) * 16777619U;
    return h;
}

/* ===== HD TEXTURE REPLACEMENT (inicio) ===================================
   Le game:\tex\<hash8hex>.tex : magic(4) + width(4) + height(4) + RGBA32 cru.
   node->width/height NAO sao tocados por isto -- continuam com o tamanho N64
   original, que e o que a normalizacao de UV e o wrap/mask esperam. */
#define X360_HDTEX_MAGIC 0x54584448u

struct X360HDTexHeader {
    uint32_t magic, width, height;
};

/* Cache de resultados por hash, com enderecamento aberto (O(1)).
   Substitui a versao anterior com busca linear, que tinha dois problemas:
   (1) o contador podia passar do tamanho do array, fazendo a busca ler FORA
       dos limites (corrupcao de memoria);
   (2) a busca linear ficava cada vez mais lenta conforme enchia.
   Guardar tambem os "hits" evita reabrir o mesmo arquivo do disco toda vez
   que a textura e reimportada (o que causava engasgos com muitas texturas
   HD, ex: sprites de kart). */
#define X360_HDTEX_CACHE_BITS 14   /* era 12 (4096): o tex.pak passa de 7 mil texturas */
#define X360_HDTEX_CACHE_SIZE (1u << X360_HDTEX_CACHE_BITS)   /* 16384 entradas */
#define X360_HDTEX_ST_EMPTY 0
#define X360_HDTEX_ST_MISS  1
#define X360_HDTEX_ST_HIT   2
#define X360_HDTEX_ST_PENDING 3   /* pedida a thread de leitura, ainda nao chegou */

static uint32_t x360_hdtex_key[X360_HDTEX_CACHE_SIZE];
static uint8_t  x360_hdtex_state[X360_HDTEX_CACHE_SIZE];
static unsigned x360_hdtex_used;

static unsigned x360_hdtex_slot(uint32_t hash) {
    unsigned i = hash & (X360_HDTEX_CACHE_SIZE - 1);
    for (unsigned probe = 0; probe < X360_HDTEX_CACHE_SIZE; ++probe) {
        if (x360_hdtex_state[i] == X360_HDTEX_ST_EMPTY || x360_hdtex_key[i] == hash)
            return i;
        i = (i + 1) & (X360_HDTEX_CACHE_SIZE - 1);
    }
    return i; /* cheio: devolve algo valido; o caller trata como nao-cacheado */
}

static uint8_t x360_hdtex_lookup(uint32_t hash) {
    unsigned i = x360_hdtex_slot(hash);
    return (x360_hdtex_key[i] == hash) ? x360_hdtex_state[i] : X360_HDTEX_ST_EMPTY;
}

static void x360_hdtex_remember(uint32_t hash, uint8_t st) {
    unsigned i = x360_hdtex_slot(hash);
    if (x360_hdtex_state[i] != X360_HDTEX_ST_EMPTY && x360_hdtex_key[i] == hash) {
        x360_hdtex_state[i] = st;   /* atualizar e sempre permitido (ex.: carregando -> pronta) */
        return;
    }
    if (x360_hdtex_used >= X360_HDTEX_CACHE_SIZE - 1) return; /* nunca enche de vez */
    if (x360_hdtex_state[i] == X360_HDTEX_ST_EMPTY) {
        x360_hdtex_key[i] = hash;
        ++x360_hdtex_used;
    }
    x360_hdtex_state[i] = st;
}

/* x360_try_load_hd_texture() fica logo apos a declaracao de gfx_rapi mais
   abaixo neste arquivo (precisa dela para o upload_texture). */
/* ===== HD TEXTURE REPLACEMENT (fim) ====================================== */

/* B17G7E DXT-TMEM-LAYOUT-VERIFY: hash the byte order produced by G_LOADBLOCK's DXT-driven
 * odd-line 32-bit-half swap, without changing source RAM or rendering. */
static uint32_t x360_b17g7e_tmem_hash(const uint8_t *src, uint32_t size_bytes,
                                        uint32_t dxt, uint32_t *row_bytes_out,
                                        uint32_t *rows_out) {
    if (!src || !size_bytes || !dxt) {
        if (row_bytes_out) *row_bytes_out = 0;
        if (rows_out) *rows_out = 0;
        return 0;
    }
    /* DXT is S1.11: integer T advances after ceil(2048/dxt) 64-bit words. */
    const uint32_t words_per_row = (2048u + dxt - 1u) / dxt;
    const uint32_t row_bytes = words_per_row * 8u;
    if (!row_bytes || row_bytes > size_bytes) {
        if (row_bytes_out) *row_bytes_out = row_bytes;
        if (rows_out) *rows_out = 0;
        return 0;
    }
    const uint32_t rows = (size_bytes + row_bytes - 1u) / row_bytes;
    uint32_t h = 2166136261U;
    for (uint32_t y = 0; y < rows; ++y) {
        const uint32_t base = y * row_bytes;
        const uint32_t remaining = size_bytes - base;
        const uint32_t this_row = remaining < row_bytes ? remaining : row_bytes;
        for (uint32_t x = 0; x < this_row; ++x) {
            uint32_t sx = x;
            if ((y & 1u) && this_row >= 8u) {
                /* On odd TMEM lines, each 64-bit word has its two 32-bit halves swapped. */
                const uint32_t in_word = x & 7u;
                sx = (x & ~7u) | ((in_word + 4u) & 7u);
                if (sx >= this_row) sx = x;
            }
            h = (h ^ src[base + sx]) * 16777619U;
        }
    }
    if (row_bytes_out) *row_bytes_out = row_bytes;
    if (rows_out) *rows_out = rows;
    return h;
}
/* Startup has a separate bounded budget; B15's gameplay budget is retained. */
static unsigned x360_texture_startup_count;
static void x360_trace_texture(int unit, int hit, uint32_t hash, bool rejected) {
    if(!x360_logging_enabled())return;
    const bool startup = gMenuSelection == 8 && gGamestate != 4;
    if (rejected ? x360_texture_reject_count >= 8 :
        (startup ? x360_texture_startup_count >= 16 :
         (gGamestate != 4 || x360_texture_trace_count >= 32))) return;
    const struct X360TextureTile *t = gfx_texture_tile(unit);
    const struct X360LoadedTexture *l = gfx_loaded_texture(unit);
    uint32_t ph = 0;
    if (t->fmt == G_IM_FMT_CI && rdp.palette && rdp.palette_bytes <= 512)
        ph = x360_texture_hash(rdp.palette, rdp.palette_bytes);
    if (!rejected && !startup) {
        const bool lakitu_candidate=t->fmt==G_IM_FMT_CI && t->siz==G_IM_SIZ_8b && t->line_size_bytes==56;
        if (lakitu_candidate && x360_texture_lakitu_count<16) {
            if (l->rows && l->source_pitch && l->row_bytes && l->row_bytes<=56 && l->rows<=72) {
                hash=2166136261U;
                for (uint32_t y=0;y<l->rows;y++) hash=(hash^x360_texture_hash(l->addr+y*l->source_pitch,l->row_bytes))*16777619U;
            }
            for(unsigned j=0;j<x360_texture_lakitu_count;j++)
                if(x360_texture_seen[j][0]==hash && x360_texture_seen[j][1]==ph) return;
            x360_texture_seen[x360_texture_lakitu_count][0]=hash;
            x360_texture_seen[x360_texture_lakitu_count++][1]=ph;
        } else if (hit==-2 && x360_texture_changed_count<8) ++x360_texture_changed_count;
        else if(x360_texture_initial_count<8) ++x360_texture_initial_count;
        else return;
    }
    char m[768];
    _snprintf(m,sizeof(m)-1,
        "MK64: B15-TEX%s[%u] load=%02X loadpc=%08X loadwords=%08X/%08X image=%08X source=%08X fmt/siz=%u/%u loadtile=%u render=%u unit=%u "
        "tmem=%u/%u bytes=%u pitch=%u srcpitch=%u row=%u rows=%u bounds=%u,%u,%u,%u "
        "mask=%u,%u shift=%u,%u wrap=%u,%u gen=%u hash=%08X cache=%d pal=%08X pgen=%u phash=%08X "
        "cmds=IMG:%08X/%08X TILE:%08X/%08X BLOCK:%08X/%08X LOADTILE:%08X/%08X SIZE:%08X/%08X\n",
        rejected?"-REJECT":(startup?"-STARTUP":""),rejected?x360_texture_reject_count++:(startup?x360_texture_startup_count++:x360_texture_trace_count++),l->command,(unsigned)l->command_address,l->load_words[0],l->load_words[1],
        (unsigned)(uintptr_t)rdp.texture_to_load.addr,(unsigned)(uintptr_t)l->addr,t->fmt,t->siz,l->load_tile,rdp.render_tile,unit,
        l->tmem,t->tmem,l->size_bytes,t->line_size_bytes,l->source_pitch,l->row_bytes,l->rows,
        t->uls,t->ult,t->lrs,t->lrt,t->masks,t->maskt,t->shifts,t->shiftt,t->cms,t->cmt,
        l->generation,hash,hit,(unsigned)(uintptr_t)rdp.palette,rdp.palette_generation,ph,
        x360_texture_commands[0][0],x360_texture_commands[0][1],x360_texture_commands[1][0],x360_texture_commands[1][1],
        x360_texture_commands[2][0],x360_texture_commands[2][1],x360_texture_commands[3][0],x360_texture_commands[3][1],
        x360_texture_commands[4][0],x360_texture_commands[4][1]);
    m[sizeof(m)-1]=0;x360_log(m);
}

static struct RenderingState {
    bool depth_test;
    bool depth_mask;
    bool decal_mode;
    bool alpha_blend;
    struct XYWidthHeight viewport, scissor;
    struct ShaderProgram *shader_program;
    struct TextureHashmapNode *textures[2];
    struct { bool valid, linear; uint8_t cms, cmt; } sampler[2];
} rendering_state;

struct GfxDimensions gfx_current_dimensions;
static float ratio_x = 1.f;
static float ratio_y = 1.f;
static float inv_ratio_x = 1.f;
static float inv_ratio_y = 1.f;

static bool dropped_frame;

static float buf_vbo[MAX_BUFFERED * (28 * 3)]; // 3 vertices in a triangle and 28 floats per vtx
static size_t buf_vbo_len;
static size_t buf_vbo_num_tris;

/* The Xbox renderer consumes the variable float layout produced below, not
 * the original 16-byte N64 Vtx directly. Keep the exact layout and the F3DEX
 * commands which produced it attached to each buffered batch. */
struct X360PackedVertexLayout {
    struct ShaderProgram *program;
    uint32_t floats_per_vertex;
    uint32_t geometry_mode;
    uint32_t last_vtx_w0, last_vtx_w1;
    uint32_t last_tri_w0, last_tri_w1;
    uint8_t num_inputs, use_texture, use_fog, use_alpha;
};
#ifdef __cplusplus
extern "C" {
#endif
struct X360PackedVertexLayout x360_gfx_batch_layout;
#ifdef __cplusplus
}
#endif
static uint32_t x360_last_vtx_w0, x360_last_vtx_w1;
static uint32_t x360_last_tri_w0, x360_last_tri_w1;
static unsigned x360_projection_log_count;
static unsigned x360_transform_log_count;
static unsigned x360_viewport_log_count;
static unsigned x360_scissor_log_count;
static unsigned x360_rgba32_log_count;
static unsigned x360_bad_vtx_log_count;
static unsigned x360_ci8_diag_count;

#define X360_BAD_DL_ROOT_MAX 32
static Gfx *x360_bad_dl_roots[X360_BAD_DL_ROOT_MAX];
static unsigned x360_bad_dl_root_count;
static Gfx *x360_bad_dl_bad_cmd[X360_BAD_DL_ROOT_MAX];
static uint32_t x360_bad_dl_bad_w0[X360_BAD_DL_ROOT_MAX];
static uint32_t x360_bad_dl_bad_w1[X360_BAD_DL_ROOT_MAX];
static bool x360_b10_marker_logged;

/* B9: keep the display-list ancestry which led to a bad command stream. */
#define X360_GDL_HISTORY_COUNT 16
struct X360GdlHistoryEntry {
    uint32_t sequence;
    uintptr_t source;
    uint32_t source_w0;
    uint32_t raw_target;
    uintptr_t resolved_target;
    uintptr_t segment_base;
    uint8_t segment;
    uint8_t push;
};
static struct X360GdlHistoryEntry x360_gdl_history[X360_GDL_HISTORY_COUNT];
static unsigned x360_gdl_history_head;
static unsigned x360_gdl_history_used;
static unsigned x360_gdl_sequence;
static bool x360_b9_marker_logged;

static struct GfxWindowManagerAPI *gfx_wapi;
static struct GfxRenderingAPI *gfx_rapi;

/* ===== HD TEXTURE REPLACEMENT: upload (precisa de gfx_rapi acima) ===== */
/* Buffer unico compartilhado pelos dois caminhos de carregamento HD
   (import_texture e o desenho de imagem inteira de menu). Ambos rodam na
   mesma thread de render, um por vez, entao compartilhar e seguro e evita
   gastar 16 MB de .bss duas vezes. */
static uint8_t x360_hd_buf[2048 * 2048 * 4];

/* Cache em RAM dos dados HD ja lidos do disco.
   Sem isto, toda reimportacao de textura (que acontece o tempo todo quando
   muitos sprites estao em rotacao) reabria o .tex no USB/HDD. Dezenas de
   aberturas de arquivo por segundo, cada uma com latencia de milissegundos,
   eram a causa principal dos engasgos -- inclusive nos menus, onde os
   sprites de kart ficam animando.
   Orcamento fixo; quando estoura, descarta a entrada usada ha mais tempo. */
/* Estatisticas de desempenho do HD (desligadas). Com 1, grava a cada ~60
   quadros em game:\hdtex-stats.log quantas leituras do tex.pak, acertos e
   descartes do cache em RAM e envios a GPU ocorreram -- para diagnosticar
   quedas de desempenho sem adivinhar a causa. */
#define X360_HDTEX_STATS 0
static uint32_t x360_st_pak, x360_st_ramhit, x360_st_evict, x360_st_up, x360_st_upkb, x360_st_frames;
/* tempo gasto (microssegundos) lendo do disco, descomprimindo e enviando a GPU,
   e acertos da leitura antecipada do tex.pak */
static uint32_t x360_st_disk_us, x360_st_dec_us, x360_st_up_us, x360_st_rahit, x360_st_reads;
static LARGE_INTEGER x360_st_freq;
/* tempo de cada quadro: o maior do periodo e quantos passaram do orcamento
   de 60 FPS (16,7 ms) e de 30 FPS (33,3 ms) */
static uint32_t x360_st_fmax_us, x360_st_f17, x360_st_f34;
static LARGE_INTEGER x360_st_last_frame;
static uint32_t x360_us_since(const LARGE_INTEGER *t0) {
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    if (!x360_st_freq.QuadPart) QueryPerformanceFrequency(&x360_st_freq);
    return (uint32_t)((t1.QuadPart - t0->QuadPart) * 1000000 / x360_st_freq.QuadPart);
}

#define X360_HDRAM_SLOTS   4096   /* era 1024: o grupo de pre-carregamento dos menus tem ~1800 texturas
                                        pequenas, e com 1024 o cache descartava as primeiras (as de
                                        maior prioridade) antes de usar os 40 MB. O limite de bytes
                                        continua o mesmo. */
#define X360_HDRAM_BUDGET  (40u << 20)   /* 40 MB: a memoria do 360 e
                                             unificada, entao este cache
                                             disputa espaco com a VRAM. */

struct X360HDRamEntry {
    uint32_t hash, w, h, bytes, last_used;
    uint8_t *data;
    uint8_t fmt;   /* 0 = RGBA32, 1 = DXT1, 2 = DXT5 (dados comprimidos) */
};
static struct X360HDRamEntry x360_hdram[X360_HDRAM_SLOTS];
static uint32_t x360_hdram_bytes, x360_hdram_clock;

/* Busca direta em todas as posicoes. A versao anterior usava enderecamento
   aberto e parava na primeira posicao vazia; como o descarte zerava posicoes,
   entradas guardadas depois de um "buraco" ficavam inalcancaveis -- ocupavam
   memoria e posicao para sempre, e o cache travava cheio. 1024 comparacoes so
   quando uma textura HD e de fato usada: custo desprezivel. */
static int x360_hdram_find(uint32_t hash) {
    for (unsigned i = 0; i < X360_HDRAM_SLOTS; ++i)
        if (x360_hdram[i].data && x360_hdram[i].hash == hash) return (int)i;
    return -1;
}

/* Descarta a entrada usada ha mais tempo. Devolve a posicao liberada ou -1. */
static int x360_hdram_evict_one(void) {
    int victim = -1;
    uint32_t oldest = 0xFFFFFFFFu;
    for (unsigned i = 0; i < X360_HDRAM_SLOTS; ++i)
        if (x360_hdram[i].data && x360_hdram[i].last_used < oldest) {
            oldest = x360_hdram[i].last_used; victim = (int)i;
        }
    if (victim < 0) return -1;
    free(x360_hdram[victim].data);
    x360_hdram_bytes -= x360_hdram[victim].bytes;
    memset(&x360_hdram[victim], 0, sizeof(x360_hdram[victim]));
    ++x360_st_evict;
    return victim;
}

static void x360_hdram_store_fmt(uint32_t hash, uint32_t w, uint32_t h, const uint8_t *src,
                                 uint32_t bytes, uint8_t fmt) {
    if (bytes > X360_HDRAM_BUDGET) return;
    /* libera por BYTES... */
    while (x360_hdram_bytes + bytes > X360_HDRAM_BUDGET)
        if (x360_hdram_evict_one() < 0) break;
    /* ...e por POSICOES: antes a tabela enchia de texturas pequenas (karts)
       sem nunca descartar, e nada novo era guardado. */
    int slot = -1;
    for (unsigned i = 0; i < X360_HDRAM_SLOTS; ++i)
        if (!x360_hdram[i].data) { slot = (int)i; break; }
    if (slot < 0) slot = x360_hdram_evict_one();
    if (slot < 0) return;
    uint8_t *buf = (uint8_t *)malloc(bytes);
    if (!buf) return;
    memcpy(buf, src, bytes);
    x360_hdram[slot].hash = hash;
    x360_hdram[slot].w = w;
    x360_hdram[slot].h = h;
    x360_hdram[slot].bytes = bytes;
    x360_hdram[slot].data = buf;
    x360_hdram[slot].fmt = fmt;
    x360_hdram[slot].last_used = ++x360_hdram_clock;
    x360_hdram_bytes += bytes;
}

static void x360_hdram_store(uint32_t hash, uint32_t w, uint32_t h, const uint8_t *src, uint32_t bytes) {
    x360_hdram_store_fmt(hash, w, h, src, bytes, 0);
}

/* ---- Decodificador DXT1/DXT5 (BC1/BC3) -----------------------------------
   O tex.pak pode guardar texturas comprimidas em DXT: 4 a 8 vezes menos
   bytes para ler do disco e para manter no cache em RAM. Aqui elas sao
   descomprimidas para RGBA32 antes do envio a GPU (o renderer continua
   recebendo RGBA, sem mudancas). Leitura byte a byte: independe da ordem de
   bytes da CPU (o Xenon e big-endian; o formato DXT e little-endian). */
static uint8_t x360_dxt_raw[2048 * 2048];   /* maior caso: DXT5 2048x2048 */

static void x360_rgb565(uint32_t c, uint8_t *o) {
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    o[0] = (uint8_t)((r << 3) | (r >> 2));
    o[1] = (uint8_t)((g << 2) | (g >> 4));
    o[2] = (uint8_t)((b << 3) | (b >> 2));
}

static uint32_t x360_dxt_size(uint32_t w, uint32_t h, uint32_t fmt) {
    if (fmt == 0) return w * h * 4;
    if ((w & 3) || (h & 3)) return 0;
    return (w / 4) * (h / 4) * (fmt == 2 ? 16u : 8u);
}

static void x360_dxt_decode(const uint8_t *src, uint32_t w, uint32_t h, uint32_t fmt, uint8_t *dst) {
    const uint32_t bw = w / 4, bh = h / 4, step = (fmt == 2) ? 16u : 8u;
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t *blk = src + (by * bw + bx) * step;
            uint8_t alpha[16];
            if (fmt == 2) {
                uint32_t a0 = blk[0], a1 = blk[1], pal[8];
                pal[0] = a0; pal[1] = a1;
                if (a0 > a1) {
                    for (uint32_t k = 1; k <= 6; ++k)
                        pal[k + 1] = ((7 - k) * a0 + k * a1) / 7;
                } else {
                    for (uint32_t k = 1; k <= 4; ++k)
                        pal[k + 1] = ((5 - k) * a0 + k * a1) / 5;
                    pal[6] = 0; pal[7] = 255;
                }
                unsigned long long bits = 0;
                for (int k = 5; k >= 0; --k) bits = (bits << 8) | blk[2 + k];
                for (uint32_t i = 0; i < 16; ++i)
                    alpha[i] = (uint8_t)pal[(bits >> (3 * i)) & 7];
                blk += 8;
            }
            const uint32_t c0 = blk[0] | (blk[1] << 8), c1 = blk[2] | (blk[3] << 8);
            const uint32_t idx = blk[4] | (blk[5] << 8) | (blk[6] << 16) | ((uint32_t)blk[7] << 24);
            uint8_t col[4][4];
            x360_rgb565(c0, col[0]); x360_rgb565(c1, col[1]);
            const bool quatro = (fmt == 2) || (c0 > c1);
            for (int ch = 0; ch < 3; ++ch) {
                if (quatro) {
                    col[2][ch] = (uint8_t)((2 * col[0][ch] + col[1][ch]) / 3);
                    col[3][ch] = (uint8_t)((col[0][ch] + 2 * col[1][ch]) / 3);
                } else {
                    col[2][ch] = (uint8_t)((col[0][ch] + col[1][ch]) / 2);
                    col[3][ch] = 0;
                }
            }
            col[0][3] = col[1][3] = col[2][3] = 255;
            col[3][3] = quatro ? 255 : 0;
            for (uint32_t i = 0; i < 16; ++i) {
                const uint32_t k = (idx >> (2 * i)) & 3;
                uint8_t *p = dst + (((by * 4 + (i >> 2)) * w) + bx * 4 + (i & 3)) * 4;
                p[0] = col[k][0]; p[1] = col[k][1]; p[2] = col[k][2];
                p[3] = (fmt == 2) ? alpha[i] : col[k][3];
            }
        }
    }
}

/* ---- tex.pak: um unico arquivo com todas as texturas HD ----------------
   Abrir milhares de arquivos individuais e o que causava os engasgos de
   "cache frio": cada CreateFileA tem latencia propria, e no inicio de cada
   tela dezenas de sprites novos aparecem de uma vez. Com o pak, o arquivo e
   aberto UMA vez e cada textura vira um seek+read. Sem o pak, o codigo cai
   de volta nos .tex soltos em game:\tex\XX\. */
#define X360_PAK_MAGIC 0x4844504Bu   /* "HDPK" */

struct X360PakEntry { uint32_t hash, off, size, w, h, fmt, preload; };

/* Leitura antecipada: cada leitura do disco tem um custo fixo de latencia,
   quase igual para 8 KB ou 256 KB. As estatisticas mostraram 150-240 leituras
   por segundo na primeira passagem pelos menus -- e o DXT, que reduziu os
   BYTES, nao mudou as travadas. Como os pedacos de uma mesma imagem ficam
   lado a lado no tex.pak, lemos um trecho maior de uma vez e servimos os
   pedidos seguintes da memoria. 0 desliga. */
#define X360_PAK_READAHEAD_KB 64   /* medido: melhor resultado em pendrive (0, 64 e 256 testados) */
#if X360_PAK_READAHEAD_KB
static uint8_t x360_ra_buf[X360_PAK_READAHEAD_KB * 1024];
static uint32_t x360_ra_off, x360_ra_len;
#endif
static uint32_t x360_pak_last_fmt, x360_pak_last_size;
static HANDLE x360_pak_file = INVALID_HANDLE_VALUE;
static struct X360PakEntry *x360_pak_index;
static uint32_t x360_pak_count;
static bool x360_pak_tried;
/* Tabela de busca (enderecamento aberto) sobre o indice: a busca linear
   percorria ate milhares de entradas a cada textura nova, o que pesava
   justamente quando varios sprites entram em cena de uma vez. */
static int32_t *x360_pak_lut;      /* indice no array, ou -1 */
static uint32_t x360_pak_lut_mask;

static uint32_t x360_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void x360_pak_open(void) {
    if (x360_pak_tried) return;
    x360_pak_tried = true;
    HANDLE f = CreateFileA("game:\\tex.pak", GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    uint8_t head[12];
    DWORD got = 0;
    if (!ReadFile(f, head, 12, &got, NULL) || got != 12 ||
        x360_be32(head) != X360_PAK_MAGIC) { CloseHandle(f); return; }
    uint32_t count = x360_be32(head + 8);
    if (!count || count > 65536) { CloseHandle(f); return; }
    /* versao 1: 20 bytes por entrada (so RGBA32); versao 2: 24 bytes, com o
       formato de cada textura (RGBA32/DXT1/DXT5). */
    const uint32_t versao = x360_be32(head + 4);
    const uint32_t esz = (versao >= 2) ? 24u : 20u;
    uint32_t bytes = count * esz;
    uint8_t *raw = (uint8_t *)malloc(bytes);
    if (!raw) { CloseHandle(f); return; }
    if (!ReadFile(f, raw, bytes, &got, NULL) || got != bytes) {
        free(raw); CloseHandle(f); return;
    }
    x360_pak_index = (struct X360PakEntry *)malloc(count * sizeof(struct X360PakEntry));
    if (!x360_pak_index) { free(raw); CloseHandle(f); return; }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *e = raw + i * esz;
        x360_pak_index[i].hash = x360_be32(e);
        x360_pak_index[i].off  = x360_be32(e + 4);
        x360_pak_index[i].size = x360_be32(e + 8);
        x360_pak_index[i].w    = x360_be32(e + 12);
        x360_pak_index[i].h    = x360_be32(e + 16);
        {
            /* campo final: formato nos bits baixos; bit 31 = grupo de
               pre-carregamento dos menus (marcado pelo PACK_TEXTURES.py) */
            const uint32_t campo = (esz >= 24) ? x360_be32(e + 20) : 0;
            x360_pak_index[i].fmt     = campo & 0xFF;
            x360_pak_index[i].preload = campo >> 31;
        }
    }
    free(raw);
    x360_pak_count = count;
    x360_pak_file = f;

    /* Tabela com o dobro da capacidade (arredondado para potencia de 2),
       para manter as colisoes baixas. */
    uint32_t cap = 64;
    while (cap < count * 2) cap <<= 1;
    x360_pak_lut = (int32_t *)malloc(cap * sizeof(int32_t));
    if (x360_pak_lut) {
        x360_pak_lut_mask = cap - 1;
        for (uint32_t i = 0; i < cap; ++i) x360_pak_lut[i] = -1;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t k = x360_pak_index[i].hash & x360_pak_lut_mask;
            while (x360_pak_lut[k] >= 0) k = (k + 1) & x360_pak_lut_mask;
            x360_pak_lut[k] = (int32_t)i;
        }
    }
}

static bool x360_pak_fetch(uint32_t off, uint32_t size, uint8_t *dest) {
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    DWORD got = 0;
    bool ok = false;
#if X360_PAK_READAHEAD_KB
    if (size <= sizeof(x360_ra_buf)) {
        if (x360_ra_len && off >= x360_ra_off && off + size <= x360_ra_off + x360_ra_len) {
            memcpy(dest, x360_ra_buf + (off - x360_ra_off), size);
            ++x360_st_rahit;
            return true;   /* da memoria: nao conta tempo de disco */
        }
        if (SetFilePointer(x360_pak_file, (LONG)off, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
            ReadFile(x360_pak_file, x360_ra_buf, sizeof(x360_ra_buf), &got, NULL) && got >= size) {
            x360_ra_off = off;
            x360_ra_len = got;
            memcpy(dest, x360_ra_buf, size);
            ok = true;
        } else {
            x360_ra_len = 0;
        }
        ++x360_st_reads;
        x360_st_disk_us += x360_us_since(&t0);
        return ok;
    }
#endif
    if (SetFilePointer(x360_pak_file, (LONG)off, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
        ReadFile(x360_pak_file, dest, size, &got, NULL) && got == size)
        ok = true;
    ++x360_st_reads;
    x360_st_disk_us += x360_us_since(&t0);
    return ok;
}

static const struct X360PakEntry *x360_pak_find(uint32_t hash) {
    x360_pak_open();
    if (x360_pak_file == INVALID_HANDLE_VALUE) return NULL;
    const struct X360PakEntry *e = NULL;
    if (x360_pak_lut) {
        uint32_t k = hash & x360_pak_lut_mask;
        for (uint32_t p = 0; p <= x360_pak_lut_mask; ++p) {
            int32_t at = x360_pak_lut[k];
            if (at < 0) break;
            if (x360_pak_index[at].hash == hash) { e = &x360_pak_index[at]; break; }
            k = (k + 1) & x360_pak_lut_mask;
        }
    } else {
        for (uint32_t i = 0; i < x360_pak_count; ++i)
            if (x360_pak_index[i].hash == hash) { e = &x360_pak_index[i]; break; }
    }
    return e;
}

static bool x360_pak_read(uint32_t hash, uint32_t *w, uint32_t *h) {
    x360_pak_open();
    if (x360_pak_file == INVALID_HANDLE_VALUE) return false;
    const struct X360PakEntry *e = x360_pak_find(hash);
    if (e) {
        if (!e->w || !e->h || e->w > 2048 || e->h > 2048 || e->fmt > 2) return false;
        if (!e->size || e->size != x360_dxt_size(e->w, e->h, e->fmt)) return false;
        /* RGBA vai direto para x360_hd_buf; DXT e lido comprimido e
           descomprimido para x360_hd_buf (os dados comprimidos ficam em
           x360_dxt_raw para o cache em RAM). */
        uint8_t *dest = (e->fmt == 0) ? x360_hd_buf : x360_dxt_raw;
        if (e->size > ((e->fmt == 0) ? sizeof(x360_hd_buf) : sizeof(x360_dxt_raw)))
            return false;
        if (!x360_pak_fetch(e->off, e->size, dest))
            return false;
        if (e->fmt != 0) {
            LARGE_INTEGER td;
            QueryPerformanceCounter(&td);
            x360_dxt_decode(x360_dxt_raw, e->w, e->h, e->fmt, x360_hd_buf);
            x360_st_dec_us += x360_us_since(&td);
        }
        x360_pak_last_fmt = e->fmt;
        x360_pak_last_size = e->size;
        *w = e->w; *h = e->h;
        return true;
    }
    return false;
}

/* ---- Carregamento em segundo plano ----------------------------------------
   Uma textura HD que ainda nao esta no cache em RAM era lida do disco no meio
   do quadro, e o jogo esperava. Em eventos que mostram varias texturas novas
   de uma vez (batida num adversario, troca de volta) isso aparecia como
   travadinha -- principalmente no HD interno, onde cada leitura fora de
   sequencia move a cabeca de leitura.
   Agora uma thread separada faz as leituras. Na primeira vez, a textura
   aparece na versao original por alguns quadros e a HD entra quando chega.
   Regras de seguranca: a thread de leitura SO le o disco (com seu proprio
   acesso ao arquivo) e entrega os dados numa fila; o cache em RAM, as tabelas
   e a GPU continuam sendo usados SOMENTE pela thread do jogo.
   0 desliga (volta a ler na hora, como antes). */
/* 0 = leitura na hora (comportamento aprovado nos testes).
   1 = EXPERIMENTAL: so a PRIMEIRA leitura de cada textura e feita em segundo
       plano; uma textura que ja apareceu em HD e foi descartada do cache e
       relida na hora (como em 0), para nunca voltar a versao original.
       A versao anterior deste modo lia TUDO em segundo plano e, com o cache
       cheio nas corridas, as texturas ficavam alternando entre HD e original. */
#define X360_HD_ASYNC 0
#define X360_ASYNC_Q 512

struct X360AsyncReq  { uint32_t hash, off, size, w, h, fmt; };
struct X360AsyncDone { uint32_t hash, w, h, fmt, size; uint8_t *data; };
static struct X360AsyncReq  x360_aq[X360_ASYNC_Q];
static struct X360AsyncDone x360_ad[X360_ASYNC_Q];
static unsigned x360_aq_head, x360_aq_tail, x360_ad_head, x360_ad_tail;
static CRITICAL_SECTION x360_async_cs;
static HANDLE x360_async_evt, x360_async_file = INVALID_HANDLE_VALUE;
static bool x360_async_ready, x360_async_tried;

static DWORD WINAPI x360_async_main(LPVOID unused) {
    (void)unused;
    for (;;) {
        WaitForSingleObject(x360_async_evt, INFINITE);
        for (;;) {
            struct X360AsyncReq r;
            bool tem = false;
            EnterCriticalSection(&x360_async_cs);
            if (x360_aq_head != x360_aq_tail) {
                r = x360_aq[x360_aq_tail % X360_ASYNC_Q];
                ++x360_aq_tail;
                tem = true;
            }
            LeaveCriticalSection(&x360_async_cs);
            if (!tem) break;
            uint8_t *buf = (uint8_t *)malloc(r.size);
            DWORD got = 0;
            if (!buf ||
                SetFilePointer(x360_async_file, (LONG)r.off, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER ||
                !ReadFile(x360_async_file, buf, r.size, &got, NULL) || got != r.size) {
                free(buf);
                buf = NULL;            /* entregue como falha: vira MISS */
            }
            EnterCriticalSection(&x360_async_cs);
            {
                struct X360AsyncDone d = { r.hash, r.w, r.h, r.fmt, r.size, buf };
                x360_ad[x360_ad_head % X360_ASYNC_Q] = d;
                ++x360_ad_head;        /* a fila de prontas nunca transborda: tem o
                                          mesmo tamanho da de pedidos e e esvaziada
                                          todo quadro */
            }
            LeaveCriticalSection(&x360_async_cs);
        }
    }
    return 0;
}

static void x360_async_init(void) {
    if (x360_async_tried) return;
    x360_async_tried = true;
#if X360_HD_ASYNC
    if (x360_pak_file == INVALID_HANDLE_VALUE) return;
    x360_async_file = CreateFileA("game:\\tex.pak", GENERIC_READ, FILE_SHARE_READ, NULL,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (x360_async_file == INVALID_HANDLE_VALUE) return;
    InitializeCriticalSection(&x360_async_cs);
    x360_async_evt = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!x360_async_evt) return;
    HANDLE th = CreateThread(NULL, 64 * 1024, x360_async_main, NULL, 0, NULL);
    if (!th) return;
    CloseHandle(th);
    x360_async_ready = true;
#endif
}

/* Pede a textura a thread de leitura. Devolve false se a fila estiver cheia
   (o pedido sera repetido numa proxima importacao). */
static bool x360_async_request(const struct X360PakEntry *e) {
    bool ok = false;
    EnterCriticalSection(&x360_async_cs);
    if (x360_aq_head - x360_aq_tail < X360_ASYNC_Q &&
        x360_ad_head - x360_ad_tail + (x360_aq_head - x360_aq_tail) < X360_ASYNC_Q) {
        struct X360AsyncReq r = { e->hash, e->off, e->size, e->w, e->h, e->fmt };
        x360_aq[x360_aq_head % X360_ASYNC_Q] = r;
        ++x360_aq_head;
        ok = true;
    }
    LeaveCriticalSection(&x360_async_cs);
    if (ok) SetEvent(x360_async_evt);
    return ok;
}

/* Chamada pela thread do jogo no inicio de cada quadro: passa as texturas
   que chegaram para o cache em RAM. */
static void x360_async_drain(void) {
    if (!x360_async_ready) return;
    for (;;) {
        struct X360AsyncDone d;
        bool tem = false;
        EnterCriticalSection(&x360_async_cs);
        if (x360_ad_head != x360_ad_tail) {
            d = x360_ad[x360_ad_tail % X360_ASYNC_Q];
            ++x360_ad_tail;
            tem = true;
        }
        LeaveCriticalSection(&x360_async_cs);
        if (!tem) break;
        if (d.data) {
            x360_hdram_store_fmt(d.hash, d.w, d.h, d.data, d.size, (uint8_t)d.fmt);
            free(d.data);
            ++x360_st_pak;
            x360_hdtex_remember(d.hash, X360_HDTEX_ST_HIT);
        } else {
            x360_hdtex_remember(d.hash, X360_HDTEX_ST_MISS);
        }
    }
}

/* ---- Pre-carregamento dos menus -------------------------------------------
   A primeira entrada na selecao de modo travava lendo do disco, na hora, os
   blocos HD do menu. O tex.pak traz agora um grupo de texturas de menu,
   contiguo no inicio do arquivo e marcado no indice. Fora das corridas, o
   jogo le esse grupo aos poucos para o cache em RAM, com um limite de tempo
   por quadro -- normalmente enquanto a tela inicial esta parada. Nas corridas
   nada muda (o pre-carregamento nem roda, e o cache funciona como antes).
   0 desliga. */
#define X360_PRELOAD_MS_POR_QUADRO 5   /* era 3: medido com folga na tela inicial */
static uint32_t x360_pre_pos;
static bool x360_pre_done;
/* estatisticas do pre-carregamento: lidas no periodo, total acumulado */
static uint32_t x360_st_pre, x360_pre_total, x360_pre_kb, x360_pre_ja;

/* Leitura do grupo INTEIRO de uma vez, no primeiro quadro (ainda no boot).
   O grupo esta contiguo no tex.pak, entao e uma unica leitura sequencial de
   ~20 MB -- o caso mais rapido para qualquer disco (cerca de 1 s no pendrive),
   paga uma vez so, antes da tela inicial. Assim o menu fica pronto desde a
   primeira entrada, sem precisar esperar na tela inicial. Se algo impedir
   (grupo nao contiguo, falta de memoria, erro de leitura), o modo gradual
   abaixo assume normalmente. 0 desliga. */
#define X360_PRELOAD_NO_BOOT 1
static bool x360_pre_boot_tried;

static void x360_preload_boot(void) {
    x360_pre_boot_tried = true;
#if X360_PRELOAD_NO_BOOT
    uint32_t n = 0;
    while (n < x360_pak_count && x360_pak_index[n].preload) ++n;
    if (!n) return;
    const uint32_t ini = x360_pak_index[0].off;
    uint32_t fim = ini;
    for (uint32_t i = 0; i < n; ++i) {
        if (x360_pak_index[i].off != fim) return;     /* nao contiguo: modo gradual */
        fim += x360_pak_index[i].size;
    }
    const uint32_t total = fim - ini;
    if (!total || total > (64u << 20)) return;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    DWORD got = 0;
    if (SetFilePointer(x360_pak_file, (LONG)ini, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER ||
        !ReadFile(x360_pak_file, buf, total, &got, NULL) || got != total) {
        free(buf);
        return;
    }
    x360_st_disk_us += x360_us_since(&t0);
    ++x360_st_reads;
    for (uint32_t i = 0; i < n; ++i) {
        const struct X360PakEntry *e = &x360_pak_index[i];
        if (!e->w || !e->h || e->w > 2048 || e->h > 2048 || e->fmt > 2) continue;
        if (!e->size || e->size != x360_dxt_size(e->w, e->h, e->fmt)) continue;
        if (x360_hdram_find(e->hash) >= 0) { ++x360_pre_ja; continue; }
        x360_hdram_store_fmt(e->hash, e->w, e->h, buf + (e->off - ini), e->size, (uint8_t)e->fmt);
        x360_hdtex_remember(e->hash, X360_HDTEX_ST_HIT);
        ++x360_st_pre; ++x360_pre_total; x360_pre_kb += e->size >> 10;
    }
    free(buf);
    x360_pre_pos = n;
    x360_pre_done = true;
#endif
}

static void x360_preload_step(void) {
#if X360_PRELOAD_MS_POR_QUADRO
    /* Saiu de uma corrida (inclusive das demos automaticas da tela inicial):
       a corrida encheu o cache e pode ter descartado os menus. Recomeca o
       grupo; o que ainda estiver na memoria e pulado. */
    static int estado_anterior = -1;
    if (estado_anterior == 4 && gGamestate != 4) {
        x360_pre_pos = 0;
        x360_pre_done = false;
    }
    estado_anterior = gGamestate;
    if (x360_pre_done || gGamestate == 4 /* RACING */) return;
    x360_pak_open();
    if (x360_pak_file == INVALID_HANDLE_VALUE || !x360_pak_index) { x360_pre_done = true; return; }
    if (!x360_pre_boot_tried) {                 /* primeiro quadro: tudo de uma vez */
        x360_preload_boot();
        if (x360_pre_done) return;
    }
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    while (x360_pre_pos < x360_pak_count) {
        const struct X360PakEntry *e = &x360_pak_index[x360_pre_pos];
        if (!e->preload) { x360_pre_done = true; return; }   /* fim do grupo */
        ++x360_pre_pos;
        if (x360_hdram_find(e->hash) >= 0) { ++x360_pre_ja; continue; }
        if (!e->w || !e->h || e->w > 2048 || e->h > 2048 || e->fmt > 2) continue;
        if (!e->size || e->size != x360_dxt_size(e->w, e->h, e->fmt)) continue;
        uint8_t *dest = (e->fmt == 0) ? x360_hd_buf : x360_dxt_raw;
        if (e->size > ((e->fmt == 0) ? sizeof(x360_hd_buf) : sizeof(x360_dxt_raw))) continue;
        if (!x360_pak_fetch(e->off, e->size, dest)) continue;
        x360_hdram_store_fmt(e->hash, e->w, e->h, dest, e->size, (uint8_t)e->fmt);
        x360_hdtex_remember(e->hash, X360_HDTEX_ST_HIT);
        ++x360_st_pre; ++x360_pre_total; x360_pre_kb += e->size >> 10;
        if (x360_us_since(&t0) >= X360_PRELOAD_MS_POR_QUADRO * 1000u) return;
    }
    x360_pre_done = true;
#endif
}

static bool x360_try_load_hd_texture(uint32_t hash) {
    if (x360_hdtex_lookup(hash) == X360_HDTEX_ST_MISS) return false;

    /* Caminho rapido: ja esta em RAM, nao toca no disco. */
    int slot = x360_hdram_find(hash);
    if (slot >= 0) {
        x360_hdram[slot].last_used = ++x360_hdram_clock;
        ++x360_st_ramhit; ++x360_st_up;
        x360_st_upkb += (x360_hdram[slot].w * x360_hdram[slot].h * 4) >> 10;
        LARGE_INTEGER tu;
        if (x360_hdram[slot].fmt != 0) {
            QueryPerformanceCounter(&tu);
            x360_dxt_decode(x360_hdram[slot].data, x360_hdram[slot].w, x360_hdram[slot].h,
                            x360_hdram[slot].fmt, x360_hd_buf);
            x360_st_dec_us += x360_us_since(&tu);
            QueryPerformanceCounter(&tu);
            gfx_rapi->upload_texture(x360_hd_buf, x360_hdram[slot].w, x360_hdram[slot].h);
        } else {
            QueryPerformanceCounter(&tu);
            gfx_rapi->upload_texture(x360_hdram[slot].data, x360_hdram[slot].w, x360_hdram[slot].h);
        }
        x360_st_up_us += x360_us_since(&tu);
        return true;
    }

#if X360_HD_ASYNC
    /* Ja pedida e ainda nao chegou: usa a original por enquanto. */
    if (x360_hdtex_lookup(hash) == X360_HDTEX_ST_PENDING) return false;
    {
        const struct X360PakEntry *pe = x360_pak_find(hash);
        x360_async_init();
        /* Ja foi carregada antes (e descartada do cache): rele na hora, para
           nao voltar a versao original no meio do jogo. */
        if (x360_async_ready && x360_hdtex_lookup(hash) != X360_HDTEX_ST_HIT) {
            if (!pe) {                         /* nao esta no pak: sem HD */
                x360_hdtex_remember(hash, X360_HDTEX_ST_MISS);
                return false;
            }
            if (pe->w && pe->h && pe->w <= 2048 && pe->h <= 2048 && pe->fmt <= 2 &&
                pe->size && pe->size == x360_dxt_size(pe->w, pe->h, pe->fmt) &&
                x360_async_request(pe))
                x360_hdtex_remember(hash, X360_HDTEX_ST_PENDING);
            return false;                      /* original agora; HD num proximo quadro */
        }
    }
#endif

    /* Caminho preferencial: tex.pak (arquivo unico, ja aberto). */
    {
        uint32_t pw = 0, ph = 0;
        if (x360_pak_read(hash, &pw, &ph)) {
            ++x360_st_pak; ++x360_st_up; x360_st_upkb += (pw * ph * 4) >> 10;
            if (x360_pak_last_fmt != 0)   /* guarda o COMPRIMIDO: cabe muito mais */
                x360_hdram_store_fmt(hash, pw, ph, x360_dxt_raw, x360_pak_last_size,
                                     (uint8_t)x360_pak_last_fmt);
            else
                x360_hdram_store(hash, pw, ph, x360_hd_buf, pw * ph * 4);
            LARGE_INTEGER tu;
            QueryPerformanceCounter(&tu);
            gfx_rapi->upload_texture(x360_hd_buf, pw, ph);
            x360_st_up_us += x360_us_since(&tu);
            x360_hdtex_remember(hash, X360_HDTEX_ST_HIT);
            return true;
        }
    }

    /* Com o tex.pak aberto, uma textura que nao esta nele simplesmente nao tem
       versao HD: nao tentamos o .tex avulso no disco. Antes, a primeira vez de
       cada textura sem HD custava uma tentativa de abrir arquivo -- e eventos
       que mostram muitas texturas novas de uma vez (batida num adversario, o
       Lakitu na troca de volta) geravam dezenas dessas consultas no mesmo
       quadro, visiveis como travadinha (principalmente no HD interno). Os .tex
       avulsos continuam funcionando para quem nao usa o tex.pak. */
    if (x360_pak_file != INVALID_HANDLE_VALUE) {
        x360_hdtex_remember(hash, X360_HDTEX_ST_MISS);
        return false;
    }

    char path[64];
    /* Subpasta pelos 2 primeiros digitos do hash: o FATX do Xbox 360 nao
       aceita mais de 4096 entradas por pasta, e o elenco completo passa
       disso. Com 256 subpastas sobram ~20 arquivos em cada. */
    _snprintf(path, sizeof(path) - 1, "game:\\tex\\%02x\\%08x.tex",
              (unsigned)((hash >> 24) & 0xFF), hash);
    path[sizeof(path) - 1] = 0;

    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        x360_hdtex_remember(hash, X360_HDTEX_ST_MISS);
        return false;
    }

    struct X360HDTexHeader hdr;
    DWORD got = 0;
    bool ok = ReadFile(f, &hdr, sizeof(hdr), &got, NULL) && got == sizeof(hdr)
              && hdr.magic == X360_HDTEX_MAGIC
              && hdr.width > 0 && hdr.height > 0
              && hdr.width <= 2048 && hdr.height <= 2048;

    uint32_t need = 0;
    if (ok) {
        need = hdr.width * hdr.height * 4;
        DWORD readBytes = 0;
        ok = ReadFile(f, x360_hd_buf, need, &readBytes, NULL) && readBytes == need;
    }
    CloseHandle(f);

    if (!ok) {
        x360_hdtex_remember(hash, X360_HDTEX_ST_MISS);
        return false;
    }

    x360_hdram_store(hash, hdr.width, hdr.height, x360_hd_buf, need);
    gfx_rapi->upload_texture(x360_hd_buf, hdr.width, hdr.height);
    x360_hdtex_remember(hash, X360_HDTEX_ST_HIT);
    return true;
}
/* ===== HD TEXTURE REPLACEMENT: upload (fim) ============================ */

static void gfx_flush(void) {
    if (buf_vbo_len > 0) {
        int num = buf_vbo_num_tris;
        gfx_rapi->draw_triangles(buf_vbo, buf_vbo_len, buf_vbo_num_tris);
        buf_vbo_len = 0;
        buf_vbo_num_tris = 0;
    }
}

static struct ShaderProgram *gfx_lookup_or_create_shader_program(uint32_t shader_id) {
    struct ShaderProgram *prg = gfx_rapi->lookup_shader(shader_id);
    if (prg == NULL) {
        gfx_rapi->unload_shader(rendering_state.shader_program);
        prg = gfx_rapi->create_and_load_new_shader(shader_id);
        rendering_state.shader_program = prg;
    }
    return prg;
}

static void gfx_generate_cc(struct ColorCombiner *comb,uint64_t cc_id,uint32_t options) {
    comb->cc_id=cc_id;comb->options=options;
    comb->prg=gfx_lookup_or_create_shader_program(gfx_cc_register(cc_id,options));
}

static struct ColorCombiner *gfx_lookup_or_create_color_combiner(uint64_t cc_id,uint32_t options) {
    static struct ColorCombiner *prev_combiner;
    if (prev_combiner != NULL && prev_combiner->cc_id == cc_id && prev_combiner->options==options) {
        return prev_combiner;
    }

    for (size_t i = 0; i < color_combiner_pool_size; i++) {
        if (color_combiner_pool[i].cc_id == cc_id && color_combiner_pool[i].options==options) {
            return prev_combiner = &color_combiner_pool[i];
        }
    }
    gfx_flush();
    if(color_combiner_pool_size==sizeof(color_combiner_pool)/sizeof(color_combiner_pool[0]))color_combiner_pool_size=0;
    struct ColorCombiner *comb = &color_combiner_pool[color_combiner_pool_size++];
    gfx_generate_cc(comb, cc_id, options);
    return prev_combiner = comb;
}

static bool gfx_texture_cache_lookup(int tile, struct TextureHashmapNode **n, const uint8_t *orig_addr, uint32_t fmt, uint32_t siz) {
    /* B17G2: MK64 reuses RAM addresses for texture data.  B16a's identity-only
     * cache can therefore return old GPU pixels after a checkpoint/state change.
     * Hash only bounded, non-cropped block loads; CI/cropped textures still
     * refresh normally. */
    const uint32_t size = gfx_loaded_texture(tile)->size_bytes;
    const uint32_t content = !gfx_loaded_texture(tile)->rows && orig_addr &&
        size && size <= 8192 && gfx_texture_tile(tile)->line_size_bytes &&
        gfx_texture_tile(tile)->line_size_bytes <= 8192
        ? x360_texture_hash(orig_addr, size) : 0;
    /* B17G7C CI8-STABLE-CACHE: avoid rebuilding/replacing the same CI8 D3D texture when
     * neither its index bytes nor its 512-byte TLUT changed. */
    const uint32_t palette_content =
        (fmt == G_IM_FMT_CI && rdp.palette && rdp.palette_bytes && rdp.palette_bytes <= 512)
        ? x360_texture_hash(rdp.palette, rdp.palette_bytes) : 0;

    size_t hash = (uintptr_t)orig_addr;
    /* mascara acompanha o tamanho do hashmap (4096 entradas). */
    hash = (hash >> 5) & 0x7ff;
    struct TextureHashmapNode **node = &gfx_texture_cache.hashmap[hash];
    while (*node != NULL && *node - gfx_texture_cache.pool < (int)gfx_texture_cache.pool_pos) {
        if ((*node)->texture_addr == orig_addr && (*node)->fmt == fmt && (*node)->siz == siz
            && (*node)->texture_unit == tile
            && (*node)->content_hash == content
            && (fmt != G_IM_FMT_CI || (*node)->palette_hash == palette_content)
            && (*node)->source_size == size
            && (*node)->line_bytes == gfx_texture_tile(tile)->line_size_bytes
            && (fmt != G_IM_FMT_CI ||
                ((*node)->ci_palette == gfx_texture_tile(tile)->palette &&
                 (*node)->tlut_mode == (rdp.other_mode_h & (3U << G_MDSFT_TEXTLUT))))) {
            gfx_rapi->select_texture(tile, (*node)->texture_id);
            *n = *node;
            bool hit = gfx_loaded_texture(tile)->rows == 0
                && (*node)->source_size == gfx_loaded_texture(tile)->size_bytes
                && (*node)->line_bytes == gfx_texture_tile(tile)->line_size_bytes
                && (*node)->content_hash == content
                && (fmt != G_IM_FMT_CI || (*node)->palette_hash == palette_content);
            /* Carregamento em segundo plano: a versao HD deste no chegou ->
               forca reimportacao para envia-la a GPU. Se falhou, desiste. */
            if ((*node)->x360_hd_pending) {
                const uint8_t pst = x360_hdtex_lookup((*node)->x360_hd_pending);
                if (pst == X360_HDTEX_ST_MISS)
                    (*node)->x360_hd_pending = 0;
                else if (pst != X360_HDTEX_ST_PENDING &&
                         x360_hdram_find((*node)->x360_hd_pending) >= 0)
                    hit = false;
            }
            const bool changed = (*node)->content_hash != content
                || (fmt == G_IM_FMT_CI && (*node)->palette_hash != palette_content);

            /* B17G6D: when this address is being reported as changed (cache=-2),
             * compare current RAM with the exact bytes fingerprinted at the
             * N64 LOADBLOCK/LOADTLUT command. */
            {
                static unsigned x360_b17g6d_verify_logs;
                const struct X360LoadedTexture *verify_l = gfx_loaded_texture(tile);
                uint32_t verify_ph = 0;
                if (x360_logging_enabled() && fmt == G_IM_FMT_CI && rdp.palette &&
                    rdp.palette_bytes && rdp.palette_bytes <= 512) {
                    verify_ph = x360_texture_hash(rdp.palette, rdp.palette_bytes);
                }

                const uint32_t tex_cap = verify_l->load_capture_hash;
                const uint32_t pal_cap = rdp.palette_load_capture_hash;
                const unsigned tex_changed =
                    (tex_cap != 0 && content != tex_cap) ? 1U : 0U;
                const unsigned pal_changed =
                    (fmt == G_IM_FMT_CI && pal_cap != 0 && verify_ph != pal_cap) ? 1U : 0U;

                if (x360_logging_enabled() && gGamestate == 4 && x360_b17g6d_verify_logs < 16 &&
                    (changed || tex_changed || pal_changed)) {
                    char verify_message[320];
                    _snprintf(verify_message, sizeof(verify_message) - 1,
                        "MK64: B17G6D RAM-VERIFY gen=%u addr=%08X fmt/siz=%u/%u "
                        "texCap=%08X texNow=%08X texChanged=%u pgen=%u "
                        "palCap=%08X palNow=%08X palChanged=%u cacheChanged=%u\n",
                        (unsigned)verify_l->generation,
                        (unsigned)(uintptr_t)orig_addr,
                        (unsigned)fmt, (unsigned)siz,
                        (unsigned)tex_cap, (unsigned)content, tex_changed,
                        (unsigned)rdp.palette_generation,
                        (unsigned)pal_cap, (unsigned)verify_ph, pal_changed,
                        changed ? 1U : 0U);
                    verify_message[sizeof(verify_message) - 1] = 0;
                    x360_log(verify_message);
                    ++x360_b17g6d_verify_logs;
                }
            }

            (*node)->content_hash = content;
            (*node)->palette_hash = palette_content;
            x360_trace_texture(tile, changed ? -2 : (hit ? 1 : 0), content, false);
            return hit;
        }
        node = &(*node)->next;
    }
    if (gfx_texture_cache.pool_pos == sizeof(gfx_texture_cache.pool) / sizeof(struct TextureHashmapNode)) {
        /* B17G4: detach all buckets before recycled pool nodes are reused. */
        memset(gfx_texture_cache.hashmap, 0, sizeof(gfx_texture_cache.hashmap));
        gfx_texture_cache.pool_pos = 0;
        node = &gfx_texture_cache.hashmap[hash];
        x360_log("MK64: B17G4 texture cache map reset at pool rollover\n");
    }
    *node = &gfx_texture_cache.pool[gfx_texture_cache.pool_pos++];
    if ((*node)->texture_addr == NULL) {
        (*node)->texture_id = gfx_rapi->new_texture();
    }
    gfx_rapi->select_texture(tile, (*node)->texture_id);
    gfx_rapi->set_sampler_parameters(tile, false, 0, 0);
    (*node)->cms = 0;
    (*node)->cmt = 0;
    (*node)->linear_filter = false;
    (*node)->next = NULL;
    (*node)->texture_addr = orig_addr;
    (*node)->fmt = fmt;
    (*node)->siz = siz;
    (*node)->texture_unit = (uint8_t)tile;
    (*node)->ci_palette = gfx_texture_tile(tile)->palette;
    (*node)->tlut_mode = rdp.other_mode_h & (3U << G_MDSFT_TEXTLUT);
    (*node)->content_hash = content;
    (*node)->palette_hash = palette_content;
    (*node)->x360_hd_loaded = 0;  /* no novo: a GPU ainda nao tem textura HD dele */
    (*node)->x360_hd_pending = 0;
    x360_trace_texture(tile, 0, content, false);
    *n = *node;
    return false;
}

static void import_texture_rgba16(int tile) {
    static uint8_t rgba32_buf[65536];

    for (uint32_t i = 0; i < (*gfx_loaded_texture(tile)).size_bytes / 2; i++) {
        uint16_t col16 = ((*gfx_loaded_texture(tile)).addr[2 * i] << 8) | (*gfx_loaded_texture(tile)).addr[2 * i + 1];
        uint8_t a = col16 & 1;
        uint8_t r = col16 >> 11;
        uint8_t g = (col16 >> 6) & 0x1f;
        uint8_t b = (col16 >> 1) & 0x1f;
        rgba32_buf[4*i + 0] = SCALE_5_8(r);
        rgba32_buf[4*i + 1] = SCALE_5_8(g);
        rgba32_buf[4*i + 2] = SCALE_5_8(b);
        rgba32_buf[4*i + 3] = a ? 255 : 0;
    }

    /* Use the same packed dimensions as UV normalization for every format. */
    uint32_t width = rendering_state.textures[tile]->width;
    uint32_t height = rendering_state.textures[tile]->height;

    gfx_rapi->upload_texture(rgba32_buf, width, height);
}

static void import_texture_rgba32(int tile) {
    /* RGBA32 line describes one of two TMEM banks (gbi.h LINE_BYTES=2).
     * The original linear source contains both banks: four bytes per texel. */
    uint32_t width = rendering_state.textures[tile]->width;
    uint32_t height = rendering_state.textures[tile]->height;
    if (x360_rgba32_log_count < 4) {
        char message[192];
        _snprintf(message, sizeof(message) - 1,
                  "MK64: RGBA32 texture line=%uB load=%uB size=%ux%u\n",
                  (unsigned)gfx_texture_tile(tile)->line_size_bytes,
                  (unsigned)(*gfx_loaded_texture(tile)).size_bytes,
                  (unsigned)width, (unsigned)height);
        message[sizeof(message) - 1] = 0;
        x360_log(message);
        ++x360_rgba32_log_count;
    }
    gfx_rapi->upload_texture((*gfx_loaded_texture(tile)).addr, width, height);
}

static void import_texture_ia4(int tile) {
    static uint8_t rgba32_buf[65536];

    for (uint32_t i = 0; i < (*gfx_loaded_texture(tile)).size_bytes * 2; i++) {
        uint8_t byte = (*gfx_loaded_texture(tile)).addr[i / 2];
        uint8_t part = (byte >> (4 - (i % 2) * 4)) & 0xf;
        uint8_t intensity = part >> 1;
        uint8_t alpha = part & 1;
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = SCALE_3_8(r);
        rgba32_buf[4*i + 1] = SCALE_3_8(g);
        rgba32_buf[4*i + 2] = SCALE_3_8(b);
        rgba32_buf[4*i + 3] = alpha ? 255 : 0;
    }

    uint32_t width = rendering_state.textures[tile]->width;
    uint32_t height = rendering_state.textures[tile]->height;

    gfx_rapi->upload_texture(rgba32_buf, width, height);
}

static void import_texture_ia8(int tile) {
    static uint8_t rgba32_buf[65536];

    for (uint32_t i = 0; i < (*gfx_loaded_texture(tile)).size_bytes; i++) {
        uint8_t intensity = (*gfx_loaded_texture(tile)).addr[i] >> 4;
        uint8_t alpha = (*gfx_loaded_texture(tile)).addr[i] & 0xf;
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = SCALE_4_8(r);
        rgba32_buf[4*i + 1] = SCALE_4_8(g);
        rgba32_buf[4*i + 2] = SCALE_4_8(b);
        rgba32_buf[4*i + 3] = SCALE_4_8(alpha);
    }

    uint32_t width = rendering_state.textures[tile]->width;
    uint32_t height = rendering_state.textures[tile]->height;

    gfx_rapi->upload_texture(rgba32_buf, width, height);
}

static void import_texture_ia16(int tile) {
    static uint8_t rgba32_buf[65536];

    for (uint32_t i = 0; i < (*gfx_loaded_texture(tile)).size_bytes / 2; i++) {
        uint8_t intensity = (*gfx_loaded_texture(tile)).addr[2 * i];
        uint8_t alpha = (*gfx_loaded_texture(tile)).addr[2 * i + 1];
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = r;
        rgba32_buf[4*i + 1] = g;
        rgba32_buf[4*i + 2] = b;
        rgba32_buf[4*i + 3] = alpha;
    }

    uint32_t width = rendering_state.textures[tile]->width;
    uint32_t height = rendering_state.textures[tile]->height;

    gfx_rapi->upload_texture(rgba32_buf, width, height);
}

static void import_texture_i4(int tile) {
    static uint8_t rgba32_buf[65536];

    for (uint32_t i = 0; i < (*gfx_loaded_texture(tile)).size_bytes * 2; i++) {
        uint8_t byte = (*gfx_loaded_texture(tile)).addr[i / 2];
        uint8_t part = (byte >> (4 - (i % 2) * 4)) & 0xf;
        uint8_t intensity = part;
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = SCALE_4_8(r);
        rgba32_buf[4*i + 1] = SCALE_4_8(g);
        rgba32_buf[4*i + 2] = SCALE_4_8(b);
        /* B13a: N64 intensity is replicated to RGBA, including alpha. */
        rgba32_buf[4*i + 3] = SCALE_4_8(intensity);
    }

    uint32_t width = rendering_state.textures[tile]->width;
    uint32_t height = rendering_state.textures[tile]->height;

    gfx_rapi->upload_texture(rgba32_buf, width, height);
}

static void import_texture_i8(int tile) {
    static uint8_t rgba32_buf[65536];

    for (uint32_t i = 0; i < (*gfx_loaded_texture(tile)).size_bytes; i++) {
        uint8_t intensity = (*gfx_loaded_texture(tile)).addr[i];
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = r;
        rgba32_buf[4*i + 1] = g;
        rgba32_buf[4*i + 2] = b;
        /* B13a: N64 intensity is replicated to RGBA, including alpha. */
        rgba32_buf[4*i + 3] = intensity;
    }

    uint32_t width = rendering_state.textures[tile]->width;
    uint32_t height = rendering_state.textures[tile]->height;

    gfx_rapi->upload_texture(rgba32_buf, width, height);
}


static void import_texture_ci4(int tile) {
    static uint8_t rgba32_buf[65536];

    for (uint32_t i = 0; i < (*gfx_loaded_texture(tile)).size_bytes * 2; i++) {
        uint8_t byte = (*gfx_loaded_texture(tile)).addr[i / 2];
        uint8_t idx = (byte >> (4 - (i % 2) * 4)) & 0xf;
        idx += (gfx_texture_tile(tile)->palette & 15) * 16;
        uint16_t col16 = (rdp.palette[idx * 2] << 8) | rdp.palette[idx * 2 + 1]; // Big endian load
        uint8_t a = col16 & 1;
        uint8_t r = col16 >> 11;
        uint8_t g = (col16 >> 6) & 0x1f;
        uint8_t b = (col16 >> 1) & 0x1f;
        rgba32_buf[4*i + 0] = SCALE_5_8(r);
        rgba32_buf[4*i + 1] = SCALE_5_8(g);
        rgba32_buf[4*i + 2] = SCALE_5_8(b);
        rgba32_buf[4*i + 3] = a ? 255 : 0;
        if ((rdp.other_mode_h & (3U << G_MDSFT_TEXTLUT)) == G_TT_IA16) {
            rgba32_buf[4*i + 0] = rgba32_buf[4*i + 1] = rgba32_buf[4*i + 2] = col16 >> 8;
            rgba32_buf[4*i + 3] = col16 & 255;
        }
    }

    uint32_t width = rendering_state.textures[tile]->width;
    uint32_t height = rendering_state.textures[tile]->height;

    gfx_rapi->upload_texture(rgba32_buf, width, height);
}

static void import_texture_ci8(int tile) {
    static uint8_t rgba32_buf[65536];

    if (x360_logging_enabled() && x360_ci8_diag_count < 4 &&
        (*gfx_loaded_texture(tile)).addr != NULL &&
        rdp.palette != NULL &&
        (*gfx_loaded_texture(tile)).size_bytes != 0) {
        uint32_t idx_nonzero = 0, pal_nonzero = 0;
        uint8_t idx_min = 255, idx_max = 0, first_nonzero = 0;
        const uint32_t size = (*gfx_loaded_texture(tile)).size_bytes;
        for (uint32_t j = 0; j < size; ++j) {
            const uint8_t value = (*gfx_loaded_texture(tile)).addr[j];
            if (value != 0) {
                if (first_nonzero == 0) first_nonzero = value;
                ++idx_nonzero;
            }
            if (value < idx_min) idx_min = value;
            if (value > idx_max) idx_max = value;
        }
        for (uint32_t j = 0; j < 512; ++j) {
            if (rdp.palette[j] != 0) ++pal_nonzero;
        }

        const uint16_t sample_color =
            ((uint16_t)rdp.palette[(uint32_t)first_nonzero * 2] << 8) |
            rdp.palette[(uint32_t)first_nonzero * 2 + 1];
        char message[320];
        _snprintf(message, sizeof(message)-1,
                  "MK64: CI8-DIAG[%u] src=%08X pal=%08X bytes=%u line=%u idxNZ=%u idxRange=%u..%u firstNZ=%u palNZ=%u sample=%04X\n",
                  x360_ci8_diag_count,
                  (unsigned)(uintptr_t)(*gfx_loaded_texture(tile)).addr,
                  (unsigned)(uintptr_t)rdp.palette,
                  (unsigned)size,
                  (unsigned)gfx_texture_tile(tile)->line_size_bytes,
                  (unsigned)idx_nonzero,
                  (unsigned)idx_min, (unsigned)idx_max,
                  (unsigned)first_nonzero,
                  (unsigned)pal_nonzero,
                  (unsigned)sample_color);
        message[sizeof(message)-1]=0;
        x360_log(message);
        ++x360_ci8_diag_count;
    }


    /* B17G7B SAFE-CI8: low-overhead diagnostic only.
     * Runs only on CI8 cache misses/refreshes during a race, only for the
     * 2048-byte / 64-byte-line texture class already seen corrupting.
     * No renderer changes, no breakpoints, no pixel scanning, no allocations.
     * Logs are hard-capped at 32 lines. */
    {
        struct X360B17G7BSlot {
            const uint8_t *addr;
            uint32_t tex_cap;
            uint32_t pal_cap;
            uint32_t texture_id;
            uint16_t tmem;
            uint8_t masks, maskt, shifts, shiftt, cms, cmt;
        };
        static struct X360B17G7BSlot slots[16];
        static unsigned slot_count;
        static unsigned match_count;
        static unsigned log_count;

        const struct X360LoadedTexture *l = gfx_loaded_texture(tile);
        const struct X360TextureTile *t = gfx_texture_tile(tile);

        if (x360_logging_enabled() && gGamestate == 4 &&
            l != NULL && l->addr != NULL &&
            rdp.palette != NULL &&
            l->size_bytes == 2048 &&
            t->line_size_bytes == 64) {

            ++match_count;

            unsigned s = 0;
            while (s < slot_count && slots[s].addr != l->addr) {
                ++s;
            }

            const uint32_t tex_id =
                rendering_state.textures[tile] != NULL
                    ? rendering_state.textures[tile]->texture_id : 0;

            bool emit = false;
            char kind = 'S';

            if (s == slot_count && slot_count < 16) {
                ++slot_count;
                emit = true;
                kind = 'N';
            } else if (s < slot_count) {
                if (slots[s].tex_cap != l->load_capture_hash ||
                    slots[s].pal_cap != rdp.palette_load_capture_hash ||
                    slots[s].texture_id != tex_id ||
                    slots[s].tmem != t->tmem ||
                    slots[s].masks != t->masks ||
                    slots[s].maskt != t->maskt ||
                    slots[s].shifts != t->shifts ||
                    slots[s].shiftt != t->shiftt ||
                    slots[s].cms != t->cms ||
                    slots[s].cmt != t->cmt) {
                    emit = true;
                    kind = 'C';
                }
            }

            /* Sparse heartbeat: at most one sample every 512 matching imports.
             * This lets a long race show stable state without log spam. */
            if (!emit && (match_count & 511U) == 0U && s < slot_count) {
                emit = true;
                kind = 'H';
            }

            if (s < 16) {
                slots[s].addr = l->addr;
                slots[s].tex_cap = l->load_capture_hash;
                slots[s].pal_cap = rdp.palette_load_capture_hash;
                slots[s].texture_id = tex_id;
                slots[s].tmem = t->tmem;
                slots[s].masks = t->masks;
                slots[s].maskt = t->maskt;
                slots[s].shifts = t->shifts;
                slots[s].shiftt = t->shiftt;
                slots[s].cms = t->cms;
                slots[s].cmt = t->cmt;
            }

            if (emit && log_count < 16) {
                char message[320];
                _snprintf(message, sizeof(message) - 1,
                    "MK64: B17G7B CI8SAFE %c n=%u slot=%u unit=%d src=%08X "
                    "texCap=%08X palCap=%08X gen=%u pgen=%u texId=%u "
                    "tmem=%u mask=%u,%u shift=%u,%u wrap=%u,%u\n",
                    kind,
                    match_count,
                    s,
                    tile,
                    (unsigned)(uintptr_t)l->addr,
                    (unsigned)l->load_capture_hash,
                    (unsigned)rdp.palette_load_capture_hash,
                    (unsigned)l->generation,
                    (unsigned)rdp.palette_generation,
                    (unsigned)tex_id,
                    (unsigned)t->tmem,
                    (unsigned)t->masks, (unsigned)t->maskt,
                    (unsigned)t->shifts, (unsigned)t->shiftt,
                    (unsigned)t->cms, (unsigned)t->cmt);
                message[sizeof(message) - 1] = 0;
                x360_log(message);
                ++log_count;
            }
        }
    }

    for (uint32_t i = 0; i < (*gfx_loaded_texture(tile)).size_bytes; i++) {
        uint8_t idx = (*gfx_loaded_texture(tile)).addr[i];
        uint16_t col16 = (rdp.palette[idx * 2] << 8) | rdp.palette[idx * 2 + 1]; // Big endian load
        uint8_t a = col16 & 1;
        uint8_t r = col16 >> 11;
        uint8_t g = (col16 >> 6) & 0x1f;
        uint8_t b = (col16 >> 1) & 0x1f;
        rgba32_buf[4*i + 0] = SCALE_5_8(r);
        rgba32_buf[4*i + 1] = SCALE_5_8(g);
        rgba32_buf[4*i + 2] = SCALE_5_8(b);
        rgba32_buf[4*i + 3] = a ? 255 : 0;
        if ((rdp.other_mode_h & (3U << G_MDSFT_TEXTLUT)) == G_TT_IA16) {
            rgba32_buf[4*i + 0] = rgba32_buf[4*i + 1] = rgba32_buf[4*i + 2] = col16 >> 8;
            rgba32_buf[4*i + 3] = col16 & 255;
        }
    }

    uint32_t width = rendering_state.textures[tile]->width;
    uint32_t height = rendering_state.textures[tile]->height;

    /* B17G8 LOW-OVERHEAD-FLIGHT-RECORDER: compact combined snapshot on real CI8 imports only.
     * Rendering is unchanged. */
    {
        static unsigned x360_b17g8_logs;
        if (x360_logging_enabled() && gGamestate == 4 && x360_b17g8_logs < 48 &&
            (*gfx_loaded_texture(tile)).addr && rdp.palette &&
            (*gfx_loaded_texture(tile)).size_bytes && width && height) {
            const struct X360LoadedTexture *l = gfx_loaded_texture(tile);
            const struct X360TextureTile *t = gfx_texture_tile(tile);
            const struct TextureHashmapNode *n = rendering_state.textures[tile];
            const uint32_t raw_hash = x360_texture_hash(l->addr, l->size_bytes);
            const uint32_t pal_hash =
                (rdp.palette_bytes && rdp.palette_bytes <= 512)
                ? x360_texture_hash(rdp.palette, rdp.palette_bytes) : 0;
            const uint32_t rgba_bytes = l->size_bytes * 4u;
            const uint32_t rgba_hash =
                (rgba_bytes <= sizeof(rgba32_buf))
                ? x360_texture_hash(rgba32_buf, rgba_bytes) : 0;

            uint32_t dxt_row_bytes = 0, dxt_rows = 0, tmem_hash = 0;
            if (l->load_dxt) {
                const uint32_t words_per_row =
                    (2048u + (uint32_t)l->load_dxt - 1u) / (uint32_t)l->load_dxt;
                dxt_row_bytes = words_per_row * 8u;
                if (dxt_row_bytes && dxt_row_bytes <= l->size_bytes) {
                    dxt_rows = (l->size_bytes + dxt_row_bytes - 1u) / dxt_row_bytes;
                    uint32_t h = 2166136261U;
                    for (uint32_t y = 0; y < dxt_rows; ++y) {
                        const uint32_t base = y * dxt_row_bytes;
                        if (base >= l->size_bytes) break;
                        uint32_t row_n = l->size_bytes - base;
                        if (row_n > dxt_row_bytes) row_n = dxt_row_bytes;
                        for (uint32_t x = 0; x < row_n; ++x) {
                            uint32_t sx = x;
                            if (y & 1u) {
                                const uint32_t iw = x & 7u;
                                const uint32_t sw = (x & ~7u) | ((iw + 4u) & 7u);
                                if (sw < row_n) sx = sw;
                            }
                            h = (h ^ l->addr[base + sx]) * 16777619U;
                        }
                    }
                    tmem_hash = h;
                }
            }

            char message[512];
            _snprintf(message, sizeof(message) - 1,
                "MK64: B17G8 CI8 gen=%u pgen=%u id=%u addr=%08X bytes=%u line=%u wh=%ux%u "
                "dxt=%u drow=%u drows=%u raw=%08X tmemHash=%08X pal=%08X rgba=%08X "
                "cacheTex=%08X cachePal=%08X tmem=%u mask=%u,%u shift=%u,%u wrap=%u,%u\n",
                (unsigned)l->generation,
                (unsigned)rdp.palette_generation,
                (unsigned)(n ? n->texture_id : 0),
                (unsigned)(uintptr_t)l->addr,
                (unsigned)l->size_bytes,
                (unsigned)t->line_size_bytes,
                (unsigned)width, (unsigned)height,
                (unsigned)l->load_dxt,
                (unsigned)dxt_row_bytes, (unsigned)dxt_rows,
                (unsigned)raw_hash, (unsigned)tmem_hash,
                (unsigned)pal_hash, (unsigned)rgba_hash,
                (unsigned)(n ? n->content_hash : 0),
                (unsigned)(n ? n->palette_hash : 0),
                (unsigned)t->tmem,
                (unsigned)t->masks, (unsigned)t->maskt,
                (unsigned)t->shifts, (unsigned)t->shiftt,
                (unsigned)t->cms, (unsigned)t->cmt);
            message[sizeof(message) - 1] = 0;
            x360_log(message);
            ++x360_b17g8_logs;
        }
    }

    gfx_rapi->upload_texture(rgba32_buf, width, height);
}

static void import_texture(int tile) {
    uint8_t fmt = gfx_texture_tile(tile)->fmt;
    uint8_t siz = gfx_texture_tile(tile)->siz;

    const uint8_t *source = (*gfx_loaded_texture(tile)).addr;
    uint32_t source_size = (*gfx_loaded_texture(tile)).size_bytes;
    uint32_t pitch = gfx_texture_tile(tile)->line_size_bytes * (siz == G_IM_SIZ_32b ? 2 : 1);
    uint32_t rows = (*gfx_loaded_texture(tile)).rows;
    const uint32_t original_row_bytes = (*gfx_loaded_texture(tile)).row_bytes;
    static uint8_t tile_data[8192];
    static unsigned tile_logs, invalid_logs;
    if (!source || !source_size || !pitch || pitch > sizeof(tile_data) ||
        (!rows && source_size < pitch) || (fmt == G_IM_FMT_CI && !rdp.palette) ||
        (rows ? rows > sizeof(tile_data) / pitch : source_size > sizeof(tile_data)) ||
        (rows && (!(*gfx_loaded_texture(tile)).row_bytes || !(*gfx_loaded_texture(tile)).source_pitch))) {
        x360_trace_texture(tile,-1,0,true);
        static const uint8_t blank[4] = {0,0,0,0};
        const unsigned u = (unsigned)tile & 1U;
        struct TextureHashmapNode *fallback = &x360_invalid_texture_node[u];
        if (!x360_invalid_texture_ready[u]) {
            memset(fallback,0,sizeof(*fallback));
            fallback->texture_id = gfx_rapi->new_texture();
            fallback->width = fallback->height = 1;
            fallback->texture_addr = blank;
            gfx_rapi->select_texture(tile,fallback->texture_id);
            gfx_rapi->set_sampler_parameters(tile,false,0,0);
            gfx_rapi->upload_texture(blank,1,1);
            x360_invalid_texture_ready[u] = true;
        } else {
            gfx_rapi->select_texture(tile,fallback->texture_id);
        }
        fallback->cms = gfx_texture_tile(tile)->cms;
        fallback->cmt = gfx_texture_tile(tile)->cmt;
        fallback->linear_filter = false;
        rendering_state.textures[tile] = fallback;
        if (invalid_logs++ < 8)
            x360_log("MK64: B17G3 isolated invalid texture state on fallback texture\n");
        return;
    }

    /* B17G14C TERRAIN-ROAD-RECORDER: timer-correlated cache/import recorder for course surfaces.
     * boot-35 showed the road/terrain family in 0x8288xxxx, 4096 bytes, DXT=256.
     * This is diagnostic only; it does not change texture selection or pixels. */
    const bool x360_b17g14c_hit =
        gfx_texture_cache_lookup(tile, &rendering_state.textures[tile], source, fmt, siz);
    {
        static int last_q = -1;
        static uintptr_t seen[8];
        static unsigned seen_n;
        const struct X360LoadedTexture *cl = gfx_loaded_texture(tile);
        const struct X360TextureTile *ct = gfx_texture_tile(tile);
        const uintptr_t sp = (uintptr_t)source;
        const int q = (int)(gCourseTimer * 4.0f);
        const bool course_surface =
            x360_logging_enabled() && gGamestate == 4 && gCourseTimer >= 0.0f && gCourseTimer < 30.0f &&
            sp >= 0x82880000u && sp < 0x82890000u &&
            source_size == 4096u && cl->load_dxt == 256u;
        if (course_surface) {
            if (q != last_q) {
                last_q = q;
                seen_n = 0;
                for (unsigned si = 0; si < 8; ++si) seen[si] = 0;
            }
            bool already = false;
            for (unsigned si = 0; si < seen_n; ++si)
                if (seen[si] == sp) already = true;
            if (!already && seen_n < 8) {
                const unsigned t100 = (unsigned)(gCourseTimer * 100.0f);
                const struct TextureHashmapNode *cn = rendering_state.textures[tile];
                const uint32_t now_hash = x360_texture_hash(source, source_size);
                char m[768];
                _snprintf(m, sizeof(m) - 1,
                    "MK64: B17G14C SURFACE t100=%u unit=%d src=%08X id=%u hit=%u "
                    "fmt/siz=%u/%u bytes=%u line=%u wh=%ux%u raw=%08X nodeHash=%08X "
                    "gen=%u loadpc=%08X dxt=%u tmem=%u tile=%u renderTile=%u "
                    "mask=%u,%u shift=%u,%u wrap=%u,%u bounds=%u,%u,%u,%u\n",
                    t100, tile, (unsigned)sp,
                    (unsigned)(cn ? cn->texture_id : 0),
                    x360_b17g14c_hit ? 1u : 0u,
                    (unsigned)fmt, (unsigned)siz, (unsigned)source_size,
                    (unsigned)ct->line_size_bytes,
                    (unsigned)(cn ? cn->width : 0), (unsigned)(cn ? cn->height : 0),
                    (unsigned)now_hash, (unsigned)(cn ? cn->content_hash : 0),
                    (unsigned)cl->generation, (unsigned)cl->command_address,
                    (unsigned)cl->load_dxt, (unsigned)ct->tmem,
                    (unsigned)cl->load_tile, (unsigned)rdp.render_tile,
                    (unsigned)ct->masks, (unsigned)ct->maskt,
                    (unsigned)ct->shifts, (unsigned)ct->shiftt,
                    (unsigned)ct->cms, (unsigned)ct->cmt,
                    (unsigned)ct->uls, (unsigned)ct->ult,
                    (unsigned)ct->lrs, (unsigned)ct->lrt);
                m[sizeof(m) - 1] = 0;
                x360_log(m);
                seen[seen_n++] = sp;
            }
        }
    }
    if (x360_b17g14c_hit) {
        return;
    }

    struct TextureHashmapNode *node = rendering_state.textures[tile];
    node->source_size = source_size;
    node->line_bytes = gfx_texture_tile(tile)->line_size_bytes;
    if (rows) {
        uint32_t bytes = (*gfx_loaded_texture(tile)).row_bytes;
        if (bytes > pitch) bytes = pitch;
        if ((*gfx_loaded_texture(tile)).source_pitch && bytes > (*gfx_loaded_texture(tile)).source_pitch)
            bytes = (*gfx_loaded_texture(tile)).source_pitch;
        /* B17G6A: for RGBA16 G_LOADTILE rows, upload the bounded logical
         * row width rather than padding each row to the RDP/TMEM line stride.
         * The debugger case becomes 320px/640B instead of 324px/648B. */
        uint32_t upload_pitch = pitch;
        if (fmt == G_IM_FMT_RGBA && siz == G_IM_SIZ_16b && bytes &&
            (*gfx_loaded_texture(tile)).source_pitch &&
            bytes <= (*gfx_loaded_texture(tile)).source_pitch) {
            upload_pitch = bytes;
        }
        memset(tile_data, 0, upload_pitch * rows);
        for (uint32_t y = 0; y < rows; ++y)
            memcpy(tile_data + y * upload_pitch,
                   source + y * (*gfx_loaded_texture(tile)).source_pitch, bytes);
        (*gfx_loaded_texture(tile)).addr = tile_data;
        (*gfx_loaded_texture(tile)).row_bytes = bytes;
        (*gfx_loaded_texture(tile)).size_bytes = upload_pitch * rows;
        pitch = upload_pitch;
        if (tile_logs++ < 8) {
            char message[192];
            _snprintf(message, sizeof(message)-1,
                "MK64: cropped texture fmt=%u siz=%u sourcePitch=%u copy=%u uploadPitch=%u rows=%u\n",
                fmt, siz, (*gfx_loaded_texture(tile)).source_pitch, bytes, pitch, rows);
            message[sizeof(message)-1]=0; x360_log(message);
        }
    }
    node->width = pitch * 8 / (4u << siz);
    node->height = (*gfx_loaded_texture(tile)).size_bytes / pitch;
    if (!node->height) node->height = 1;

    /* HD texture replacement: hash do conteudo ORIGINAL do texel data.
       node->width/height NAO sao tocados aqui. */
    {
        uint32_t hd_hash = x360_texture_hash(source, source_size);
        uint32_t hd_logical_hash = 0;
        /* Texturas CI com paleta completa (256 cores): hash de pixels + paleta.
           Animacoes por troca de paleta (os neons da Rainbow Road) usam os
           MESMOS pixels em todos os quadros; so a paleta muda. Com o hash so
           dos pixels, todos os quadros tinham o mesmo hash -- no tex.pak so
           cabia um, e no jogo a imagem HD ficava parada. Este hash e procurado
           ANTES do hash so de pixels; se nao existir no tex.pak, o
           comportamento e exatamente o de antes (karts etc. nao mudam).
           Equivale a FNV-1a sobre (pixels || paleta), como no extrator. */
        uint32_t hd_pal_hash = 0;
        if (gfx_texture_tile(tile)->fmt == G_IM_FMT_CI && rdp.palette && rdp.palette_bytes == 512) {
            const uint8_t *pal = (const uint8_t *)rdp.palette;
            uint32_t ph = hd_hash;
            for (uint32_t i = 0; i < 512; ++i) ph = (ph ^ pal[i]) * 16777619U;
            hd_pal_hash = ph;
        }
        uint32_t hd_logical_rows = 0;
        /* G_LOADTILE usa lrt inclusivo. Em algumas texturas do menu (nomes,
           titulos etc.) isso faz o renderer capturar uma linha extra: o
           conteudo usado pelo extrator/PAK corresponde a N linhas, enquanto
           source_size contem N+1. Nao alteramos a carga real; apenas
           calculamos um segundo hash sem a linha extra como fallback. */
        if (rows > 1 && (*gfx_loaded_texture(tile)).row_bytes &&
            source_size == (*gfx_loaded_texture(tile)).row_bytes * rows) {
            const uint32_t requested_rows =
                (rdp.tiles[(*gfx_loaded_texture(tile)).load_tile & 7].lrt >=
                 rdp.tiles[(*gfx_loaded_texture(tile)).load_tile & 7].ult)
                    ? ((rdp.tiles[(*gfx_loaded_texture(tile)).load_tile & 7].lrt -
                        rdp.tiles[(*gfx_loaded_texture(tile)).load_tile & 7].ult) >> 2)
                    : 0;
            if (requested_rows > 0 && requested_rows < rows) {
                hd_logical_rows = requested_rows;
                hd_logical_hash = x360_texture_hash(
                    source, (*gfx_loaded_texture(tile)).row_bytes * hd_logical_rows);
            }
        }
        /* NAO sobrescrever node->content_hash aqui: e o campo que
           gfx_texture_cache_lookup usa para reconhecer a textura. Para cargas
           em blocos (LOADTILE, ex: retratos do menu) ele deve ficar 0; com o
           hash aqui, nenhum bloco era reconhecido de novo, o cache enchia e
           era esvaziado sem parar no meio do quadro, causando blocos trocados
           e piscadas. */
        /* Cargas em blocos (LOADTILE, ex: retratos do menu) nunca contam como
           "ja carregadas" no cache do port, entao sao reimportadas a CADA
           quadro. Com a textura original (~4 KB) isso passa despercebido; com
           a HD (~67 KB por bloco) eram ~2100 envios por segundo a GPU (~140
           MB/s), cada um criando uma textura nova -- o que travava a selecao
           de personagens depois de uma corrida, com a memoria de video cheia.
           Se a GPU ja tem exatamente esta textura HD neste no, nao reenvia. */
        /* Tambem aceita o hash logico (sem a linha extra do G_LOADTILE
           inclusivo): faixas encontradas por ele seriam reenviadas a GPU a
           cada quadro se so o hash completo -- que muda com o "lixo" depois
           da imagem -- fosse comparado. */
        if (node->x360_hd_loaded != 0 &&
            (node->x360_hd_loaded == hd_hash ||
             (hd_pal_hash != 0 && node->x360_hd_loaded == hd_pal_hash) ||
             (hd_logical_hash != 0 && node->x360_hd_loaded == hd_logical_hash))) {
            return;
        }
        bool hd_found = false;
        uint32_t hd_used_hash = hd_hash;
        if (hd_pal_hash) {
            hd_found = x360_try_load_hd_texture(hd_pal_hash);
            if (hd_found) hd_used_hash = hd_pal_hash;
        }
        if (!hd_found) hd_found = x360_try_load_hd_texture(hd_hash);
        if (!hd_found && hd_logical_hash) {
            hd_found = x360_try_load_hd_texture(hd_logical_hash);
            if (hd_found) hd_used_hash = hd_logical_hash;
        }

        /* TKMK00 texture_ok e carregada em dois G_LOADTILEs horizontais.
           O tex.pak guarda o OK completo sob DFF91B13, enquanto cada
           G_LOADTILE produz um hash somente da metade.

           Nao carregamos a imagem inteira para cada metade: isso quebraria
           a amostragem porque node->width/height continuam representando
           o bloco N64. Quando os hashes das duas metades nao existem no PAK,
           usamos DFF91B13 como fonte e recortamos a metade correspondente.
           O trace mostrou os blocos N64 em x=0 e x=15; o tamanho do recorte
           acompanha a escala da arte HD (2x, 4x, 8x etc.). */
        if (!hd_found && (hd_hash == 0x35d069ffu || hd_hash == 0x154196f3u)) {
            const uint32_t ok_full_hash = 0xdff91b13u;
            int ok_slot = x360_hdram_find(ok_full_hash);
            const uint8_t *ok_pixels = NULL;
            uint32_t ok_w = 0, ok_h = 0;

            if (ok_slot >= 0) {
                x360_hdram[ok_slot].last_used = ++x360_hdram_clock;
                ok_pixels = x360_hdram[ok_slot].fmt ? NULL : x360_hdram[ok_slot].data;
                ok_w = x360_hdram[ok_slot].w;
                ok_h = x360_hdram[ok_slot].h;
            } else {
                uint32_t pw = 0, ph = 0;
                if (x360_pak_read(ok_full_hash, &pw, &ph)) {
                    x360_hdram_store(ok_full_hash, pw, ph, x360_hd_buf, pw * ph * 4);
                    ok_slot = x360_hdram_find(ok_full_hash);
                    if (ok_slot >= 0) {
                        x360_hdram[ok_slot].last_used = ++x360_hdram_clock;
                        ok_pixels = x360_hdram[ok_slot].fmt ? NULL : x360_hdram[ok_slot].data;
                        ok_w = x360_hdram[ok_slot].w;
                        ok_h = x360_hdram[ok_slot].h;
                    }
                }
            }

            /* O PNG do OK pode estar em 2x (62x38), 4x (124x76) ou
               8x (248x152). A geometria original continua sendo 31x19
               e os dois G_LOADTILEs continuam sendo as janelas x=0..15
               e x=15..31. Calculamos o recorte pela escala da arte HD,
               aceitando explicitamente as tres resolucoes usadas pelo
               projeto sem alterar os outros caminhos de textura. */
            if (ok_pixels &&
                ((ok_w == 62u && ok_h == 38u) ||
                 (ok_w == 124u && ok_h == 76u) ||
                 (ok_w == 248u && ok_h == 152u))) {
                const uint32_t ok_scale = ok_w / 31u;
                const uint32_t crop_x = (hd_hash == 0x35d069ffu) ? 0u : (15u * ok_scale);
                const uint32_t crop_w = 16u * ok_scale;
                const uint32_t crop_h = 19u * ok_scale;
                const uint32_t crop_bytes = crop_w * crop_h * 4u;
                uint8_t *ok_crop;

                /* 62x38 cabe no buffer pequeno usado pelas cargas N64.
                   Resolucao maior (ex.: 248x152) nao cabe nos 8192 bytes,
                   entao usamos o buffer HD grande. O OK completo ja foi
                   copiado para x360_hdram quando veio do PAK, portanto
                   x360_hd_buf pode ser reutilizado como destino do recorte. */
                if (crop_bytes <= sizeof(tile_data))
                    ok_crop = tile_data;
                else if (crop_bytes <= sizeof(x360_hd_buf))
                    ok_crop = x360_hd_buf;
                else
                    ok_crop = NULL;

                if (ok_crop) {
                    for (uint32_t y = 0; y < crop_h; ++y) {
                        memcpy(ok_crop + y * crop_w * 4,
                               ok_pixels + (y * ok_w + crop_x) * 4,
                               crop_w * 4);
                    }

                    gfx_rapi->upload_texture(ok_crop, crop_w, crop_h);
                    hd_found = true;
                    hd_used_hash = hd_hash;
                }
            }
        }

        node->x360_hd_loaded = hd_found ? hd_used_hash : 0;
        node->x360_hd_pending = 0;
        if (!hd_found) {
            if (x360_hdtex_lookup(hd_hash) == X360_HDTEX_ST_PENDING)
                node->x360_hd_pending = hd_hash;
            else if (hd_logical_hash &&
                     x360_hdtex_lookup(hd_logical_hash) == X360_HDTEX_ST_PENDING)
                node->x360_hd_pending = hd_logical_hash;
        }

        /* Trace de diagnostico (desligado). Para reativar, troque o 0 por 1
           abaixo: grava em game:\hdtex-trace.log o hash calculado em runtime
           de cada textura indexada de 2048 bytes (as metades dos sprites de
           kart), para cruzar com os manifests via CROSS_CHECK.py/SCAN_HALVES.py.
           Mantido desligado porque escreve em disco durante o jogo. */
#define X360_HDTEX_TRACE 0
/* Alvo do trace: 0 = sprites CI8 de 2048 bytes (metades de kart)
                  1 = faixas RGBA16 (imagens grandes de menu)
                  2 = qualquer textura CI (ex: sprites do Lakitu) */
#define X360_HDTEX_TRACE_MENU 1
#if X360_HDTEX_TRACE
#if X360_HDTEX_TRACE_MENU == 1
        if (fmt == G_IM_FMT_RGBA && siz == G_IM_SIZ_16b) {
#elif X360_HDTEX_TRACE_MENU == 2
        if (fmt == G_IM_FMT_CI) {
#else
        if (fmt == G_IM_FMT_CI && source_size == 2048) {
#endif
            static uint32_t kt_seen[4000];
            static unsigned kt_n;
            bool kt_dup = false;
            for (unsigned i = 0; i < kt_n; ++i)
                if (kt_seen[i] == hd_hash) { kt_dup = true; break; }
            if (!kt_dup && kt_n < 4000) {
                kt_seen[kt_n++] = hd_hash;
                char msg[220];
                /* Primeiros 12 bytes da origem: permite identificar EXATAMENTE
                   qual regiao da imagem o jogo esta hasheando, comparando com
                   o que o extrator ve no PC (ver KART_DEBUG.py). */
                _snprintf(msg, sizeof(msg) - 1,
                    "MK64: TKMK_TILE_TRACE fmt=%u siz=%u w=%u h=%u srcsz=%u hash=%08x found=%u m=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X pitch=%u lx=%u ly=%u tw=%u\n",
                    (unsigned)fmt, (unsigned)siz, node->width, node->height, (unsigned)source_size,
                    hd_hash, hd_found ? 1u : 0u,
                    /* amostra do MEIO do bloco: o inicio do sprite e
                       transparente (zeros) e nao distingue nada. */
                    source[source_size/2 + 0], source[source_size/2 + 1],
                    source[source_size/2 + 2], source[source_size/2 + 3],
                    source[source_size/2 + 4], source[source_size/2 + 5],
                    source[source_size/2 + 6], source[source_size/2 + 7],
                    source[source_size/2 + 8], source[source_size/2 + 9],
                    source[source_size/2 + 10], source[source_size/2 + 11],
                    /* geometria real do carregamento: largura da imagem-fonte
                       (pitch em bytes), canto do bloco e largura declarada */
                    (unsigned)(*gfx_loaded_texture(tile)).source_pitch,
                    (unsigned)(rdp.tiles[(*gfx_loaded_texture(tile)).load_tile & 7].uls >> 2),
                    (unsigned)(rdp.tiles[(*gfx_loaded_texture(tile)).load_tile & 7].ult >> 2),
                    (unsigned)rdp.texture_to_load.width);
                msg[sizeof(msg) - 1] = 0;
                HANDLE tf = CreateFileA("game:\\hdtex-trace.log", GENERIC_WRITE,
                                         FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
                if (tf != INVALID_HANDLE_VALUE) {
                    SetFilePointer(tf, 0, NULL, FILE_END);
                    DWORD wr = 0;
                    WriteFile(tf, msg, (DWORD)strlen(msg), &wr, NULL);
                    CloseHandle(tf);
                }
            }
        }

#endif

        if (hd_found) {
            return;
        }
    }

    if (fmt == G_IM_FMT_RGBA) {
        if (siz == G_IM_SIZ_16b) {
            import_texture_rgba16(tile);
        } else if (siz == G_IM_SIZ_32b) {
            import_texture_rgba32(tile);
        } else {
            abort();
        }
    } else if (fmt == G_IM_FMT_IA) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_ia4(tile);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_ia8(tile);
        } else if (siz == G_IM_SIZ_16b) {
            import_texture_ia16(tile);
        } else {
            abort();
        }
    } else if (fmt == G_IM_FMT_CI) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_ci4(tile);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_ci8(tile);
        } else {
            abort();
        }
    } else if (fmt == G_IM_FMT_I) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_i4(tile);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_i8(tile);
        } else {
            abort();
        }
    } else {
        abort();
    }
    (*gfx_loaded_texture(tile)).addr = source;
    (*gfx_loaded_texture(tile)).size_bytes = source_size;
    (*gfx_loaded_texture(tile)).row_bytes = original_row_bytes;
}

static inline float rsqrtf(const float x) {
    const float x2 = x * 0.5f;
    float y = x;
    int32_t i = *(int32_t *)&y;
    i = 0x5f3759df - (i >> 1);
    y = *(float *)&i;
    y = y * (1.5f - (x2 * y * y));
    return y;
}

static inline void gfx_normalize_vector(float v[3]) {
    const float s = rsqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] *= s;
    v[1] *= s;
    v[2] *= s;
}

static inline void gfx_transposed_matrix_mul(float *restrict res, const float *restrict a, const float (*restrict b)[4]) {
    res[0] = a[0] * b[0][0] + a[1] * b[0][1] + a[2] * b[0][2];
    res[1] = a[0] * b[1][0] + a[1] * b[1][1] + a[2] * b[1][2];
    res[2] = a[0] * b[2][0] + a[1] * b[2][1] + a[2] * b[2][2];
}

static inline void calculate_normal_dir(const Light_t *light, float coeffs[3]) {
    float light_dir[3] = {
        light->dir[0] / 127.0f,
        light->dir[1] / 127.0f,
        light->dir[2] / 127.0f
    };
    gfx_transposed_matrix_mul(coeffs, light_dir, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
    gfx_normalize_vector(coeffs);
}

static inline void gfx_matrix_mul_inplace(const float (*restrict a)[4], float (*restrict res)[4]) {
    float tmp[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[i][j] = a[i][0] * res[0][j] +
                        a[i][1] * res[1][j] +
                        a[i][2] * res[2][j] +
                        a[i][3] * res[3][j];
        }
    }
    memcpy(res, tmp, sizeof(tmp));
}

static inline void gfx_matrix_mul(float (*restrict res)[4], const float (*restrict a)[4], const float (*restrict b)[4]) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            res[i][j] = a[i][0] * b[0][j] +
                        a[i][1] * b[1][j] +
                        a[i][2] * b[2][j] +
                        a[i][3] * b[3][j];
        }
    }
}

static void gfx_sp_matrix(uint8_t parameters, const int32_t *addr) {
    float matrix[4][4];
#ifndef GBI_FLOATS
    // Original GBI where fixed point matrices are used
    register int idx;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j += 2) {
            idx = (i << 1) + (j >> 1);
            const int32_t int_part = addr[idx];
            const uint32_t frac_part = addr[8 + idx];
            matrix[i][j] = (int32_t)((int_part & 0xffff0000) | (frac_part >> 16)) / 65536.f;
            matrix[i][j + 1] = (int32_t)((int_part << 16) | (frac_part & 0xffff)) / 65536.f;
        }
    }
#else
    // For a modified GBI where fixed point values are replaced with floats
    memcpy(matrix, addr, sizeof(matrix));
#endif

    if ((parameters & G_MTX_PROJECTION) && x360_projection_log_count < 2) {
        char message[512];
        _snprintf(message, sizeof(message) - 1,
                  "MK64: projection[%u] flags=%02X "
                  "[%g %g %g %g] [%g %g %g %g] "
                  "[%g %g %g %g] [%g %g %g %g]\n",
                  x360_projection_log_count, parameters,
                  matrix[0][0], matrix[0][1], matrix[0][2], matrix[0][3],
                  matrix[1][0], matrix[1][1], matrix[1][2], matrix[1][3],
                  matrix[2][0], matrix[2][1], matrix[2][2], matrix[2][3],
                  matrix[3][0], matrix[3][1], matrix[3][2], matrix[3][3]);
        message[sizeof(message) - 1] = 0;
        x360_log(message);
        ++x360_projection_log_count;
    }

    if (parameters & G_MTX_PROJECTION) {
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.P_matrix, matrix, sizeof(matrix));
        } else {
            gfx_matrix_mul_inplace(matrix, rsp.P_matrix);
        }
    } else { // G_MTX_MODELVIEW
        if ((parameters & G_MTX_PUSH) && rsp.modelview_matrix_stack_size < 11) {
            ++rsp.modelview_matrix_stack_size;
            memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 2], sizeof(matrix));
        }
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], matrix, sizeof(matrix));
        } else {
            gfx_matrix_mul_inplace(matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
        }
        rsp.lights_changed = 1;
    }
    gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.P_matrix);
}

static void gfx_sp_pop_matrix(uint32_t count) {
    while (count--) {
        if (rsp.modelview_matrix_stack_size > 0) {
            --rsp.modelview_matrix_stack_size;
            if (rsp.modelview_matrix_stack_size > 0) {
                gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.P_matrix);
            }
        }
    }
}

static inline bool x360_legacy_wide_display(void) {
    return x360_display_aspect() > 1.5f;
}

static inline float gfx_adjust_x_for_aspect_ratio(float x) {
    if (!x360_legacy_wide_display()) {
        return x;
    }

    const float physical_aspect =
        gfx_current_dimensions.aspect_ratio > 0.1f
            ? (float)gfx_current_dimensions.aspect_ratio
            : (16.0f / 9.0f);

    /* Exact pre-display-option 16:9 behavior. */
    return x * (4.0f / 3.0f) / physical_aspect;
}

static void gfx_sp_vertex(size_t n_vertices, size_t dest_index, const Vtx *vertices) {
    float object_min[3] = { 1.0e30f, 1.0e30f, 1.0e30f };
    float object_max[3] = { -1.0e30f, -1.0e30f, -1.0e30f };
    float clip_min[4] = { 1.0e30f, 1.0e30f, 1.0e30f, 1.0e30f };
    float clip_max[4] = { -1.0e30f, -1.0e30f, -1.0e30f, -1.0e30f };
    const size_t first_dest_index = dest_index;
    for (size_t i = 0; i < n_vertices; i++, dest_index++) {
        const Vtx_t *v = &vertices[i].v;
        const Vtx_tn *vn = &vertices[i].n;
        struct LoadedVertex *d = &rsp.loaded_vertices[dest_index];
        x360_logo_vertex[dest_index].valid = 0;

        float x = v->ob[0] * rsp.MP_matrix[0][0] + v->ob[1] * rsp.MP_matrix[1][0] + v->ob[2] * rsp.MP_matrix[2][0] + rsp.MP_matrix[3][0];
        float y = v->ob[0] * rsp.MP_matrix[0][1] + v->ob[1] * rsp.MP_matrix[1][1] + v->ob[2] * rsp.MP_matrix[2][1] + rsp.MP_matrix[3][1];
        float z = v->ob[0] * rsp.MP_matrix[0][2] + v->ob[1] * rsp.MP_matrix[1][2] + v->ob[2] * rsp.MP_matrix[2][2] + rsp.MP_matrix[3][2];
        float w = v->ob[0] * rsp.MP_matrix[0][3] + v->ob[1] * rsp.MP_matrix[1][3] + v->ob[2] * rsp.MP_matrix[2][3] + rsp.MP_matrix[3][3];

        x = gfx_adjust_x_for_aspect_ratio(x);

        if(x360_logging_enabled() && x360_transform_log_count<6) {
        for (int axis = 0; axis < 3; ++axis) {
            float value = (float)v->ob[axis];
            if (value < object_min[axis]) object_min[axis] = value;
            if (value > object_max[axis]) object_max[axis] = value;
        }
        const float clip_values[4] = { x, y, z, w };
        for (int axis = 0; axis < 4; ++axis) {
            if (clip_values[axis] < clip_min[axis]) clip_min[axis] = clip_values[axis];
            if (clip_values[axis] > clip_max[axis]) clip_max[axis] = clip_values[axis];
        }

        }

        short U = v->tc[0] * rsp.texture_scaling_factor.s >> 16;
        short V = v->tc[1] * rsp.texture_scaling_factor.t >> 16;

        if (rsp.geometry_mode & G_LIGHTING) {
            if (rsp.lights_changed) {
                for (int i = 0; i < rsp.current_num_lights - 1; i++) {
                    calculate_normal_dir(&rsp.current_lights[i], rsp.current_lights_coeffs[i]);
                }
                if (rsp.lookat_enabled) {
                    calculate_normal_dir(&rsp.lookat[0], rsp.current_lookat_coeffs[0]);
                    calculate_normal_dir(&rsp.lookat[1], rsp.current_lookat_coeffs[1]);
                } else {
                    static const Light_t lookat_x = {{0, 0, 0}, 0, {0, 0, 0}, 0, {127, 0, 0}, 0};
                    static const Light_t lookat_y = {{0, 0, 0}, 0, {0, 0, 0}, 0, {0, 127, 0}, 0};
                    calculate_normal_dir(&lookat_x, rsp.current_lookat_coeffs[0]);
                    calculate_normal_dir(&lookat_y, rsp.current_lookat_coeffs[1]);
                }
                rsp.lights_changed = false;
            }

            int r = rsp.current_lights[rsp.current_num_lights - 1].col[0];
            int g = rsp.current_lights[rsp.current_num_lights - 1].col[1];
            int b = rsp.current_lights[rsp.current_num_lights - 1].col[2];

            for (int i = 0; i < rsp.current_num_lights - 1; i++) {
                float intensity = 0;
                intensity += vn->n[0] * rsp.current_lights_coeffs[i][0];
                intensity += vn->n[1] * rsp.current_lights_coeffs[i][1];
                intensity += vn->n[2] * rsp.current_lights_coeffs[i][2];
                intensity /= 127.0f;
                if (intensity > 0.0f) {
                    r += intensity * rsp.current_lights[i].col[0];
                    g += intensity * rsp.current_lights[i].col[1];
                    b += intensity * rsp.current_lights[i].col[2];
                }
            }

            d->color.r = r > 255 ? 255 : r;
            d->color.g = g > 255 ? 255 : g;
            d->color.b = b > 255 ? 255 : b;

            if (rsp.geometry_mode & G_TEXTURE_GEN) {
                float dotx = 0, doty = 0;
                dotx += vn->n[0] * rsp.current_lookat_coeffs[0][0];
                dotx += vn->n[1] * rsp.current_lookat_coeffs[0][1];
                dotx += vn->n[2] * rsp.current_lookat_coeffs[0][2];
                doty += vn->n[0] * rsp.current_lookat_coeffs[1][0];
                doty += vn->n[1] * rsp.current_lookat_coeffs[1][1];
                doty += vn->n[2] * rsp.current_lookat_coeffs[1][2];

                U = (int32_t)((dotx / 127.0f + 1.0f) / 4.0f * rsp.texture_scaling_factor.s);
                V = (int32_t)((doty / 127.0f + 1.0f) / 4.0f * rsp.texture_scaling_factor.t);
                if (x360_logo_count < 8) { /* B16a: capture first G_TEXTURE_GEN vertices */
                    x360_logo_vertex[dest_index].valid = 1;
                    for (int k=0;k<3;k++) x360_logo_vertex[dest_index].normal[k]=vn->n[k];
                    x360_logo_vertex[dest_index].dot[0]=dotx;
                    x360_logo_vertex[dest_index].dot[1]=doty;
                }
            }
        } else {
            d->color.r = v->cn[0];
            d->color.g = v->cn[1];
            d->color.b = v->cn[2];
        }

        d->u = U;
        d->v = V;

        // trivial clip rejection
        d->clip_rej = 0;
        if (x < -w) d->clip_rej |= CLIP_LEFT;
        if (x >  w) d->clip_rej |= CLIP_RIGHT;
        if (y < -w) d->clip_rej |= CLIP_BOTTOM;
        if (y >  w) d->clip_rej |= CLIP_TOP;
        if (z < -w) d->clip_rej |= CLIP_FAR;
        if (z >  w) d->clip_rej |= CLIP_NEAR;

        d->x = x;
        d->y = y;
        d->z = z;
        d->w = w;

        if (rsp.geometry_mode & G_FOG) {
            w = (w == 0.f) ? 1.f / 0.001f : 1.f / w;
            const float winv = w < 0.0f ? 32767.0f : w;
            float fog_z = z * winv * rsp.fog_mul + rsp.fog_offset;
            if (fog_z < 0) fog_z = 0;
            if (fog_z > 255) fog_z = 255;
            d->color.a = fog_z; // Use alpha variable to store fog factor
        } else {
            d->color.a = v->cn[3];
        }
    }
    if (n_vertices && x360_transform_log_count < 6) {
        char message[448];
        _snprintf(message, sizeof(message) - 1,
                  "MK64: transform[%u] n=%u dst=%u geom=%08X "
                  "obj=[%g..%g,%g..%g,%g..%g] "
                  "clip=[%g..%g,%g..%g,%g..%g,%g..%g]\n",
                  x360_transform_log_count, (unsigned)n_vertices, (unsigned)first_dest_index,
                  (unsigned)rsp.geometry_mode,
                  object_min[0], object_max[0], object_min[1], object_max[1], object_min[2], object_max[2],
                  clip_min[0], clip_max[0], clip_min[1], clip_max[1],
                  clip_min[2], clip_max[2], clip_min[3], clip_max[3]);
        message[sizeof(message) - 1] = 0;
        x360_log(message);
        ++x360_transform_log_count;
    }
}

static inline struct ColorCombiner *gfx_pick_combiner(bool *out_use_fog, bool *out_use_alpha) {
    uint64_t cc_id = rdp.combine_mode;

    bool use_alpha = (rdp.other_mode_l & (G_BL_A_MEM << 18)) == 0;
    const bool use_fog = (rsp.geometry_mode & G_FOG) && (rdp.other_mode_l >> 30) == G_BL_CLR_FOG;
    const bool texture_edge = (rdp.other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA;
    const bool use_noise = (rdp.other_mode_l & G_AC_DITHER) == G_AC_DITHER;

    if (texture_edge) {
        use_alpha = true;
    }

    uint32_t options=0;
    if(use_alpha)options|=SHADER_OPT_ALPHA;
    if(use_fog)options|=SHADER_OPT_FOG;
    if(texture_edge)options|=SHADER_OPT_TEXTURE_EDGE;
    if(use_noise)options|=SHADER_OPT_NOISE;
    if((rdp.other_mode_h&(3U<<G_MDSFT_CYCLETYPE))==G_CYC_2CYCLE)options|=SHADER_OPT_2CYCLE;
    struct ColorCombiner *comb=gfx_lookup_or_create_color_combiner(cc_id,options);
    struct ShaderProgram *prg = comb->prg;
    if (prg != rendering_state.shader_program) {
        gfx_flush();
        gfx_rapi->unload_shader(rendering_state.shader_program);
        gfx_rapi->load_shader(prg);
        rendering_state.shader_program = prg;
    }
    if (use_alpha != rendering_state.alpha_blend) {
        gfx_flush();
        gfx_rapi->set_use_alpha(use_alpha);
        rendering_state.alpha_blend = use_alpha;
    }

    if (out_use_fog) *out_use_fog = use_fog;
    if (out_use_alpha) *out_use_alpha = use_alpha;

    return comb;
}

static inline bool gfx_update_textures(const bool used_textures[2], const bool linear_filter) {
    /* Reserve both slots together. Recycling between slot 0 and slot 1 would
     * leave slot 0 referring to a pool node (and GPU ID) reused by slot 1. */
    if (gfx_texture_cache.pool_pos > sizeof(gfx_texture_cache.pool) / sizeof(gfx_texture_cache.pool[0]) - 2) {
        gfx_flush();
        memset(gfx_texture_cache.hashmap, 0, sizeof(gfx_texture_cache.hashmap));
        gfx_texture_cache.pool_pos = 0;
        rdp.textures_changed[0] = rdp.textures_changed[1] = true;
    }
    for (int i = 0; i < 2; i++) {
        if (used_textures[i]) {
            if (rdp.textures_changed[i]) {
                gfx_flush();
                import_texture(i);
                rdp.textures_changed[i] = false;
                rendering_state.sampler[i].valid = false;
            }
            if (!rendering_state.sampler[i].valid || linear_filter != rendering_state.sampler[i].linear ||
                gfx_texture_tile(i)->cms != rendering_state.sampler[i].cms ||
                gfx_texture_tile(i)->cmt != rendering_state.sampler[i].cmt) {
                gfx_flush();
                gfx_rapi->set_sampler_parameters(i, linear_filter, gfx_texture_tile(i)->cms, gfx_texture_tile(i)->cmt);
                rendering_state.sampler[i].valid = true;
                rendering_state.sampler[i].linear = linear_filter;
                rendering_state.sampler[i].cms = gfx_texture_tile(i)->cms;
                rendering_state.sampler[i].cmt = gfx_texture_tile(i)->cmt;
                rendering_state.textures[i]->linear_filter = linear_filter;
                rendering_state.textures[i]->cms = gfx_texture_tile(i)->cms;
                rendering_state.textures[i]->cmt = gfx_texture_tile(i)->cmt;
            }
        }
    }
    return used_textures[0] || used_textures[1];
}

static float gfx_shift_texcoord(float coord, uint8_t shift) {
    return shift <= 10 ? coord / (float)(1U << shift)
                       : coord * (float)(1U << (16 - shift));
}

static inline void gfx_push_triangle(const struct LoadedVertex *restrict v1, const struct LoadedVertex *restrict v2, const struct LoadedVertex *restrict v3) {
    const struct LoadedVertex *v_arr[3] = {v1, v2, v3};

    const bool depth_test = (rsp.geometry_mode & G_ZBUFFER) == G_ZBUFFER;
    if (depth_test != rendering_state.depth_test) {
        gfx_flush();
        gfx_rapi->set_depth_test(depth_test);
        rendering_state.depth_test = depth_test;
    }

    const bool z_upd = (rdp.other_mode_l & Z_UPD) == Z_UPD;
    if (z_upd != rendering_state.depth_mask) {
        gfx_flush();
        gfx_rapi->set_depth_mask(z_upd);
        rendering_state.depth_mask = z_upd;
    }

    const bool zmode_decal = (rdp.other_mode_l & ZMODE_DEC) == ZMODE_DEC;
    if (zmode_decal != rendering_state.decal_mode) {
        gfx_flush();
        gfx_rapi->set_zmode_decal(zmode_decal);
        rendering_state.decal_mode = zmode_decal;
    }

    if (rdp.viewport_or_scissor_changed) {
        if (memcmp(&rdp.viewport, &rendering_state.viewport, sizeof(rdp.viewport)) != 0) {
            gfx_flush();
            gfx_rapi->set_viewport(rdp.viewport.x, rdp.viewport.y, rdp.viewport.width, rdp.viewport.height);
            rendering_state.viewport = rdp.viewport;
        }
        if (memcmp(&rdp.scissor, &rendering_state.scissor, sizeof(rdp.scissor)) != 0) {
            gfx_flush();
            gfx_rapi->set_scissor(rdp.scissor.x, rdp.scissor.y, rdp.scissor.width, rdp.scissor.height);
            rendering_state.scissor = rdp.scissor;
        }
        rdp.viewport_or_scissor_changed = false;
    }

    uint8_t num_inputs;
    bool used_textures[2], use_fog, use_alpha;

    struct ColorCombiner *comb = gfx_pick_combiner(&use_fog, &use_alpha);
    gfx_rapi->shader_get_info(rendering_state.shader_program, &num_inputs, used_textures);

    const bool linear_filter = true && (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;
    const bool use_texture = gfx_update_textures(used_textures, linear_filter);
    /* B17G14C TERRAIN-ROAD-RECORDER: sparse draw-time snapshot for the same course texture family. */
    if (use_texture && used_textures[0] && rendering_state.textures[0] != NULL) {
        static int last_q = -1;
        static uintptr_t seen[8];
        static unsigned seen_n;
        const struct TextureHashmapNode *dn = rendering_state.textures[0];
        const uintptr_t sp = (uintptr_t)dn->texture_addr;
        const int q = (int)(gCourseTimer * 4.0f);
        const bool course_surface =
            x360_logging_enabled() && gGamestate == 4 && gCourseTimer >= 0.0f && gCourseTimer < 30.0f &&
            sp >= 0x82880000u && sp < 0x82890000u && dn->source_size == 4096u;
        if (course_surface) {
            if (q != last_q) {
                last_q = q;
                seen_n = 0;
                for (unsigned si = 0; si < 8; ++si) seen[si] = 0;
            }
            bool already = false;
            for (unsigned si = 0; si < seen_n; ++si)
                if (seen[si] == sp) already = true;
            if (!already && seen_n < 8) {
                const struct X360TextureTile *dt = gfx_texture_tile(0);
                const unsigned t100 = (unsigned)(gCourseTimer * 100.0f);
                char m[1024];
                _snprintf(m, sizeof(m) - 1,
                    "MK64: B17G14C DRAW t100=%u src=%08X id=%u fmt/siz=%u/%u "
                    "wh=%ux%u line=%u raw=%08X shader=%08X comb=%08X geom=%08X "
                    "filter=%u tmem=%u mask=%u,%u shift=%u,%u wrap=%u,%u "
                    "V0=(%g,%g,%g,%g uv=%g,%g) V1=(%g,%g,%g,%g uv=%g,%g) "
                    "V2=(%g,%g,%g,%g uv=%g,%g)\n",
                    t100, (unsigned)sp, (unsigned)dn->texture_id,
                    (unsigned)dn->fmt, (unsigned)dn->siz,
                    (unsigned)dn->width, (unsigned)dn->height,
                    (unsigned)dn->line_bytes, (unsigned)dn->content_hash,
                    (unsigned)(uintptr_t)rendering_state.shader_program,
                    (unsigned)rdp.combine_mode, (unsigned)rsp.geometry_mode,
                    linear_filter ? 1u : 0u, (unsigned)dt->tmem,
                    (unsigned)dt->masks, (unsigned)dt->maskt,
                    (unsigned)dt->shifts, (unsigned)dt->shiftt,
                    (unsigned)dt->cms, (unsigned)dt->cmt,
                    v1->x, v1->y, v1->z, v1->w, v1->u, v1->v,
                    v2->x, v2->y, v2->z, v2->w, v2->u, v2->v,
                    v3->x, v3->y, v3->z, v3->w, v3->u, v3->v);
                m[sizeof(m) - 1] = 0;
                x360_log(m);
                seen[seen_n++] = sp;
            }
        }
    }
    const uint32_t floats_per_vertex = 4 + (used_textures[0]?2:0) + (used_textures[1]?2:0) + (use_fog ? 4 : 0) + num_inputs * 4;
    if (buf_vbo_len != 0 &&
        (x360_gfx_batch_layout.program != rendering_state.shader_program ||
         x360_gfx_batch_layout.floats_per_vertex != floats_per_vertex ||
         x360_gfx_batch_layout.num_inputs != num_inputs ||
         x360_gfx_batch_layout.use_texture != use_texture ||
         x360_gfx_batch_layout.use_fog != use_fog ||
         x360_gfx_batch_layout.use_alpha != use_alpha)) {
        gfx_flush();
    }
    if (buf_vbo_len == 0) {
        x360_gfx_batch_layout.program = rendering_state.shader_program;
        x360_gfx_batch_layout.floats_per_vertex = floats_per_vertex;
        x360_gfx_batch_layout.geometry_mode = rsp.geometry_mode;
        x360_gfx_batch_layout.last_vtx_w0 = x360_last_vtx_w0;
        x360_gfx_batch_layout.last_vtx_w1 = x360_last_vtx_w1;
        x360_gfx_batch_layout.last_tri_w0 = x360_last_tri_w0;
        x360_gfx_batch_layout.last_tri_w1 = x360_last_tri_w1;
        x360_gfx_batch_layout.num_inputs = num_inputs;
        x360_gfx_batch_layout.use_texture = use_texture;
        x360_gfx_batch_layout.use_fog = use_fog;
        x360_gfx_batch_layout.use_alpha = use_alpha;
    }

#ifndef GFX_W_PREMULT
    const bool z_is_from_0_to_1 = gfx_rapi->z_is_from_0_to_1();
#endif

    for (int i = 0; i < 3; i++) {
#ifdef GFX_W_PREMULT
        const float w = v_arr[i]->w;
        const float w_inv = 1.f / w;
        buf_vbo[buf_vbo_len++] = v_arr[i]->x * w_inv;
        buf_vbo[buf_vbo_len++] = v_arr[i]->y * w_inv;
        buf_vbo[buf_vbo_len++] = (v_arr[i]->z + w) * 0.5f * w_inv;
        buf_vbo[buf_vbo_len++] = w_inv; // store inverted W right away to save softrast the trouble
#else
        float z = v_arr[i]->z, w = v_arr[i]->w;
        if (z_is_from_0_to_1) {
            z = (z + w) / 2.0f;
        }
        buf_vbo[buf_vbo_len++] = v_arr[i]->x;
        buf_vbo[buf_vbo_len++] = v_arr[i]->y;
        buf_vbo[buf_vbo_len++] = z;
        buf_vbo[buf_vbo_len++] = w;
#endif

        for(int unit=0;unit<2;++unit)if(used_textures[unit]) {
            const struct TextureHashmapNode *texture=rendering_state.textures[unit];
            const struct X360TextureTile *tile=gfx_texture_tile(unit);
            const float width=texture&&texture->width?(float)texture->width:1.0f;
            const float height=texture&&texture->height?(float)texture->height:1.0f;
            float u=(gfx_shift_texcoord(v_arr[i]->u,tile->shifts)-tile->uls*8)/32.0f;
            float v=(gfx_shift_texcoord(v_arr[i]->v,tile->shiftt)-tile->ult*8)/32.0f;
            if(linear_filter){u+=0.5f;v+=0.5f;}
            buf_vbo[buf_vbo_len++]=GFX_OUT_PROP(u/width);
            buf_vbo[buf_vbo_len++]=GFX_OUT_PROP(v/height);
        }

        if (use_fog) {
#ifndef GFX_NO_FOG_COLOR
            buf_vbo[buf_vbo_len++] = GFX_OUT_PROP(GFX_COLOR_CONVERT(rdp.fog_color.r));
            buf_vbo[buf_vbo_len++] = GFX_OUT_PROP(GFX_COLOR_CONVERT(rdp.fog_color.g));
            buf_vbo[buf_vbo_len++] = GFX_OUT_PROP(GFX_COLOR_CONVERT(rdp.fog_color.b));
#endif
            buf_vbo[buf_vbo_len++] = GFX_OUT_PROP(GFX_COLOR_CONVERT(v_arr[i]->color.a)); // fog factor (not alpha)
        }

        for(int j=0;j<num_inputs;++j){
            struct RGBA color;
            if(j==0)color=rdp.prim_color;
            else if(j==1){color=v_arr[i]->color;if(rsp.geometry_mode&G_FOG)color.a=255;}
            else if(j==2)color=rdp.env_color;
            else {float lod=(v_arr[i]->w-3000.0f)/3000.0f;if(lod<0)lod=0;if(lod>1)lod=1;color.r=(uint8_t)(lod*255);color.g=rdp.prim_lod_fraction;color.b=color.a=0;}
            buf_vbo[buf_vbo_len++]=GFX_OUT_PROP(GFX_COLOR_CONVERT(color.r));
            buf_vbo[buf_vbo_len++]=GFX_OUT_PROP(GFX_COLOR_CONVERT(color.g));
            buf_vbo[buf_vbo_len++]=GFX_OUT_PROP(GFX_COLOR_CONVERT(color.b));
            buf_vbo[buf_vbo_len++]=GFX_OUT_PROP(GFX_COLOR_CONVERT(color.a));
        }
    }

    if (++buf_vbo_num_tris == MAX_BUFFERED) {
        gfx_flush();
    }
}

#ifdef GFX_MANUAL_CLIPPING
static inline float flerp(const float v0, const float v1, const float t) {
    return v0 + t * (v1 - v0);
}

static inline struct RGBA rgba_lerp(const struct RGBA c0, const struct RGBA c1, const float t) {
    struct RGBA result;
    result.r = (uint8_t)(c0.r + (c1.r - c0.r) * t);
    result.g = (uint8_t)(c0.g + (c1.g - c0.g) * t);
    result.b = (uint8_t)(c0.b + (c1.b - c0.b) * t);
    result.a = (uint8_t)(c0.a + (c1.a - c0.a) * t);
    return result;
}

static inline bool gfx_clip_triangle(struct LoadedVertex *v1, struct LoadedVertex *v2, struct LoadedVertex *v3, const uint8_t clip_and) {
    static const float c_planes[][4] = {
        {  0.0f,  0.0f, -1.0f,  1.0f }, // near
        {  0.0f,  0.0f,  1.0f,  1.0f }, // far
        {  0.0f, -1.0f,  0.0f,  1.0f }, // top
        {  0.0f,  1.0f,  0.0f,  1.0f }, // bottom
        { -1.0f,  0.0f,  0.0f,  1.0f }, // left
        {  1.0f,  0.0f,  0.0f,  1.0f }, // right
    };

    const uint8_t clip_or = v1->clip_rej | v2->clip_rej | v3->clip_rej;

    if (!clip_or) return false; // triangle fully in frustum

    struct LoadedVertex v_buf[2][12] = { { *v1, *v2, *v3 } };
    int v_num[2] = { 3, 0 };
    int v_idx = 0;

    uint8_t plane_idx = 0;
    for (uint8_t clip_mask = 1; clip_mask < 64; clip_mask <<= 1, ++plane_idx) {
        if (!(clip_or & clip_mask)) continue;

        const int num_verts = v_num[v_idx];
        const int outidx = !v_idx;
        const struct LoadedVertex *v_in = v_buf[v_idx];
        struct LoadedVertex *v_out = v_buf[outidx];
        const float *plane = c_planes[plane_idx];

        for (int i = 0; i < num_verts; ++i) {
            const struct LoadedVertex *vthis = &v_in[i];
            const struct LoadedVertex *vnext = &v_in[(i + 1) % num_verts];
            const float d1 = plane[0] * vthis->x + plane[1] * vthis->y + plane[2] * vthis->z + vthis->w;
            const float d2 = plane[0] * vnext->x + plane[1] * vnext->y + plane[2] * vnext->z + vnext->w;
            const bool this_in = d1 >= 0.0f;
            const bool next_in = d2 >= 0.0f;
            // current is inside clipping plane, push it into output
            if (this_in) v_out[v_num[outidx]++] = *vthis;
            // one of the vertices is outside, clip the edge and push intersection
            if (this_in ^ next_in) {
                struct LoadedVertex *xv = &v_out[v_num[outidx]++];
                if (this_in) {
                    const float t = d1 / (d1 - d2);
                    xv->x = flerp(vthis->x, vnext->x, t);
                    xv->y = flerp(vthis->y, vnext->y, t);
                    xv->z = flerp(vthis->z, vnext->z, t);
                    xv->w = flerp(vthis->w, vnext->w, t);
                    xv->u = flerp(vthis->u, vnext->u, t);
                    xv->v = flerp(vthis->v, vnext->v, t);
                    xv->color = rgba_lerp(vthis->color, vnext->color, t);
                    xv->clip_rej = 0;
                } else {
                    const float t = d2 / (d2 - d1);
                    xv->x = flerp(vnext->x, vthis->x, t);
                    xv->y = flerp(vnext->y, vthis->y, t);
                    xv->z = flerp(vnext->z, vthis->z, t);
                    xv->w = flerp(vnext->w, vthis->w, t);
                    xv->u = flerp(vnext->u, vthis->u, t);
                    xv->v = flerp(vnext->v, vthis->v, t);
                    xv->color = rgba_lerp(vnext->color, vthis->color, t);
                }
            }
        }

        if (v_num[outidx] < 3) return true; // not enough for a triangle

        v_idx = outidx;
        v_num[!v_idx] = 0;
    }

    // make a triangle fan
    const int n = v_num[v_idx] - 1;
    const struct LoadedVertex *in = v_buf[v_idx];
    for (int i = 1; i < n; ++i)
        gfx_push_triangle(in + 0, in + i, in + i + 1);

    return true;
}
#endif

static void gfx_sp_tri1(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx) {
    struct LoadedVertex *v1 = &rsp.loaded_vertices[vtx1_idx];
    struct LoadedVertex *v2 = &rsp.loaded_vertices[vtx2_idx];
    struct LoadedVertex *v3 = &rsp.loaded_vertices[vtx3_idx];

    //if (rand()%2) return;

    const uint8_t clip_and = v1->clip_rej & v2->clip_rej & v3->clip_rej;
    if (clip_and) {
        // The whole triangle lies outside the visible area
        return;
    }

    if ((rsp.geometry_mode & G_CULL_BOTH) != 0) {
        float dx1 = v1->x / (v1->w) - v2->x / (v2->w);
        float dy1 = v1->y / (v1->w) - v2->y / (v2->w);
        float dx2 = v3->x / (v3->w) - v2->x / (v2->w);
        float dy2 = v3->y / (v3->w) - v2->y / (v2->w);
        float cross = dx1 * dy2 - dy1 * dx2;

        if ((v1->w < 0) ^ (v2->w < 0) ^ (v3->w < 0)) {
            // If one vertex lies behind the eye, negating cross will give the correct result.
            // If all vertices lie behind the eye, the triangle will be rejected anyway.
            cross = -cross;
        }

        switch (rsp.geometry_mode & G_CULL_BOTH) {
            case G_CULL_FRONT:
                if (cross <= 0) return;
                break;
            case G_CULL_BACK:
                if (cross >= 0) return;
                break;
            case G_CULL_BOTH:
                // Why is this even an option?
                return;
        }
    }

#ifdef GFX_MANUAL_CLIPPING
    // clip the triangle and put the resulting triangles into the buffer
    // otherwise put the current triangle
    if (!gfx_clip_triangle(v1, v2, v3, clip_and))
#endif

    gfx_push_triangle(v1, v2, v3);
}

static void gfx_sp_geometry_mode(uint32_t clear, uint32_t set) {
    rsp.geometry_mode &= ~clear;
    rsp.geometry_mode |= set;
}

static void gfx_calc_and_set_viewport(const Vp_t *viewport) {
    // 2 bits fraction
    float width = 2.0f * viewport->vscale[0] / 4.0f;
    float height = 2.0f * viewport->vscale[1] / 4.0f;
    float x = (viewport->vtrans[0] / 4.0f) - width / 2.0f;
    float y = SCREEN_HEIGHT - ((viewport->vtrans[1] / 4.0f) + height / 2.0f);

    width *= ratio_x;
    height *= ratio_y;
    x *= ratio_x;
    y *= ratio_y;

    if (x360_viewport_log_count < 8) {
        char message[256];
        _snprintf(message, sizeof(message) - 1,
                  "MK64: viewport[%u] raw scale=%d,%d trans=%d,%d -> bottomleft=%g,%g %gx%g ratios=%g,%g\n",
                  x360_viewport_log_count, viewport->vscale[0], viewport->vscale[1],
                  viewport->vtrans[0], viewport->vtrans[1], x, y, width, height, ratio_x, ratio_y);
        message[sizeof(message) - 1] = 0;
        x360_log(message);
        ++x360_viewport_log_count;
    }

    rdp.viewport.x = x;
    rdp.viewport.y = y;
    rdp.viewport.width = width;
    rdp.viewport.height = height;

    rdp.viewport_or_scissor_changed = true;
}

static void gfx_sp_movemem(uint8_t index, uint8_t offset, const void* data) {
    switch (index) {
        case G_MV_VIEWPORT:
            gfx_calc_and_set_viewport((const Vp_t *) data);
            break;
        case G_MV_LOOKATY:
        case G_MV_LOOKATX: {
            const unsigned lookat_index = (unsigned)!((index - G_MV_LOOKATY) / 2);
            memcpy(&rsp.lookat[lookat_index], data, sizeof(Light_t));
            rsp.lookat_enabled = true;
            rsp.lights_changed = true;
            static unsigned x360_b17g3_lookat_logs = 0;
            if (x360_b17g3_lookat_logs++ < 8) {
                char message[160];
                _snprintf(message,sizeof(message)-1,
                          "MK64: B17G3 LOOKAT[%u] dir=%d,%d,%d\n",
                          lookat_index,
                          (int)rsp.lookat[lookat_index].dir[0],
                          (int)rsp.lookat[lookat_index].dir[1],
                          (int)rsp.lookat[lookat_index].dir[2]);
                message[sizeof(message)-1]=0; x360_log(message);
            }
            break;
        }
#ifdef F3DEX_GBI_2
        case G_MV_LIGHT: {
            int lightidx = offset / 24 - 2;
            if (lightidx >= 0 && lightidx <= MAX_LIGHTS) { // skip lookat
                // NOTE: reads out of bounds if it is an ambient light
                memcpy(rsp.current_lights + lightidx, data, sizeof(Light_t));
            }
            break;
        }
#else
        case G_MV_L0:
        case G_MV_L1:
        case G_MV_L2:
            // NOTE: reads out of bounds if it is an ambient light
            memcpy(rsp.current_lights + (index - G_MV_L0) / 2, data, sizeof(Light_t));
            break;
#endif
    }
}

static void gfx_sp_moveword(uint8_t index, uint16_t offset, uint32_t data) {
    switch (index) {
        case G_MW_SEGMENT:
            if((offset/4)<16)rspSegments[offset/4]=data;
            break;
        case G_MW_NUMLIGHT:
#ifdef F3DEX_GBI_2
            rsp.current_num_lights = data / 24 + 1; // add ambient light
#else
            // Ambient light is included
            // The 31th bit is a flag that lights should be recalculated
            rsp.current_num_lights = (data - 0x80000000U) / 32;
#endif
            rsp.lights_changed = 1;
            break;
        case G_MW_FOG:
            rsp.fog_mul = (int16_t)(data >> 16);
            rsp.fog_offset = (int16_t)data;
            break;
    }
}

static void gfx_sp_texture(uint16_t sc, uint16_t tc, uint8_t level, uint8_t tile, uint8_t on) {
    /*
     * B16a: MK64 commonly emits gSPTexture(..., tile=1, G_OFF). B14/B15
     * treated that disabled-texturing command as an active render-tile switch.
     * Since this PC renderer does not otherwise model G_OFF, later draws could
     * import an uninitialized tile-1 descriptor with zero pitch/bounds.
     */
    if (on != 0 && rdp.render_tile != (tile & 7)) {
        rdp.render_tile = tile & 7;
        rdp.textures_changed[0] = rdp.textures_changed[1] = true;
    }
    rsp.texture_scaling_factor.s = sc;
    rsp.texture_scaling_factor.t = tc;
}

static void gfx_dp_set_scissor(uint32_t mode, uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    float x = ulx / 4.0f * ratio_x;
    float y = (SCREEN_HEIGHT - lry / 4.0f) * ratio_y;
    float width = (lrx - ulx) / 4.0f * ratio_x;
    float height = (lry - uly) / 4.0f * ratio_y;


    if (x360_scissor_log_count < 8) {
        char message[224];
        _snprintf(message, sizeof(message) - 1,
                  "MK64: scissor[%u] raw=%u,%u..%u,%u mode=%u -> bottomleft=%g,%g %gx%g\n",
                  x360_scissor_log_count, (unsigned)ulx, (unsigned)uly,
                  (unsigned)lrx, (unsigned)lry, (unsigned)mode, x, y, width, height);
        message[sizeof(message) - 1] = 0;
        x360_log(message);
        ++x360_scissor_log_count;
    }

    rdp.scissor.x = x;
    rdp.scissor.y = y;
    rdp.scissor.width = width;
    rdp.scissor.height = height;

    rdp.viewport_or_scissor_changed = true;
}

static void gfx_dp_set_texture_image(uint32_t format, uint32_t size, uint32_t width, const void* addr) {
    rdp.texture_to_load.addr = (const uint8_t *)addr;
    rdp.texture_to_load.siz = size;
    rdp.texture_to_load.width = width + 1;
}

static void gfx_dp_set_tile(uint8_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint8_t tile, uint32_t palette, uint32_t cmt, uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks, uint32_t shifts) {
    struct X360TextureTile *desc = &rdp.tiles[tile & 7];
    desc->fmt = fmt; desc->siz = siz; desc->line_size_bytes = line * 8;
    desc->tmem = tmem; desc->palette = palette;
    desc->cms = cms; desc->cmt = cmt;
    desc->masks = masks; desc->maskt = maskt;
    desc->shifts = shifts; desc->shiftt = shiftt;
    /* B17G11 COURSE-TMEM-BANK-RECORDER: observe only course-style render-tile bank switches.
     * No renderer state is changed here. */
    {
        static unsigned x360_b17g11_select_logs;
        static uint32_t last_tmem = 0xFFFFFFFFu;
        static uint32_t last_g0 = 0xFFFFFFFFu, last_g1 = 0xFFFFFFFFu;
        const bool course_select =
            tile == G_TX_RENDERTILE && fmt == G_IM_FMT_RGBA && siz == G_IM_SIZ_16b &&
            line * 8u == 64u && (tmem == 0u || tmem == 256u);
        const uint32_t g0 = rdp.loaded_texture[0].generation;
        const uint32_t g1 = rdp.loaded_texture[1].generation;
        if (x360_logging_enabled() && gGamestate == 4 && course_select && x360_b17g11_select_logs < 12 &&
            (tmem != last_tmem || g0 != last_g0 || g1 != last_g1)) {
            char m[512];
            _snprintf(m, sizeof(m) - 1,
                "MK64: B17G11 BANKSELECT rtile=%u tmem=%u bank=%u lineB=%u | b0 gen=%u src=%08X bytes=%u hash=%08X | b1 gen=%u src=%08X bytes=%u hash=%08X\n",
                (unsigned)tile, (unsigned)tmem, (unsigned)(tmem / 256u),
                (unsigned)(line * 8u),
                (unsigned)rdp.loaded_texture[0].generation,
                (unsigned)(uintptr_t)rdp.loaded_texture[0].addr,
                (unsigned)rdp.loaded_texture[0].size_bytes,
                (unsigned)rdp.loaded_texture[0].load_capture_hash,
                (unsigned)rdp.loaded_texture[1].generation,
                (unsigned)(uintptr_t)rdp.loaded_texture[1].addr,
                (unsigned)rdp.loaded_texture[1].size_bytes,
                (unsigned)rdp.loaded_texture[1].load_capture_hash);
            m[sizeof(m) - 1] = 0; x360_log(m);
            ++x360_b17g11_select_logs;
            last_tmem = tmem; last_g0 = g0; last_g1 = g1;
        }
    }
    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
}

static void gfx_dp_set_tile_size(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
    struct X360TextureTile *desc = &rdp.tiles[tile & 7];
    desc->uls = uls; desc->ult = ult; desc->lrs = lrs; desc->lrt = lrt;
    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
}

static void gfx_dp_load_tlut(uint8_t tile, uint32_t high_index) {
    const uint32_t tmem = rdp.tiles[tile & 7].tmem;
    if (!rdp.texture_to_load.addr || rdp.texture_to_load.siz != G_IM_SIZ_16b ||
        tmem < 256 || tmem >= 512) return;
    const uint32_t first = tmem - 256;
    uint32_t count = high_index + 1;
    if (count > 256 - first) count = 256 - first;
    memcpy(rdp.palette_data + first * 2, rdp.texture_to_load.addr, count * 2);
    rdp.palette = rdp.palette_data;
    rdp.palette_bytes = sizeof(rdp.palette_data);
    ++rdp.palette_generation;
    rdp.palette_load_capture_hash =
        (x360_logging_enabled() && rdp.palette && rdp.palette_bytes && rdp.palette_bytes <= 512)
        ? x360_texture_hash(rdp.palette, rdp.palette_bytes) : 0;

    /* B17G6C diagnostic only: fingerprint palette RAM at LOADTLUT time during gGamestate==4.
     * Compare this hash to B15-TEX phash with the same pgen. If it differs,
     * the live RAM backing the emulated TLUT changed after the load. */
    {
        static unsigned x360_b17g6b_tlut_logs;
        if (x360_logging_enabled() && gGamestate == 4 && x360_b17g6b_tlut_logs++ < 8 && rdp.palette) {
            uint32_t bytes = rdp.palette_bytes;
            if (bytes > 512) bytes = 512;
            const uint32_t hash = x360_texture_hash(rdp.palette, bytes);
            char message[256];
            _snprintf(message, sizeof(message) - 1,
                "MK64: B17G6C RACE-TLUT-LOAD pgen=%u tile=%u tmem=%u high=%u bytes=%u addr=%08X hash=%08X\n",
                (unsigned)rdp.palette_generation, (unsigned)tile,
                (unsigned)rdp.tiles[tile & 7].tmem, (unsigned)high_index,
                (unsigned)bytes, (unsigned)(uintptr_t)rdp.palette,
                (unsigned)hash);
            message[sizeof(message) - 1] = 0;
            x360_log(message);
        }
    }

    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
}

static void gfx_dp_load_block(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {
    const uint32_t load_bank = rdp.tiles[tile & 7].tmem / 256;
    rdp.loaded_texture[load_bank].generation = ++x360_texture_generation;
    rdp.loaded_texture[load_bank].command = G_LOADBLOCK;
    rdp.loaded_texture[load_bank].command_address = x360_texture_command_address[2];
    memcpy(rdp.loaded_texture[load_bank].load_words,x360_texture_commands[2],sizeof(rdp.loaded_texture[load_bank].load_words));
    rdp.loaded_texture[load_bank].load_tile = tile;
    rdp.loaded_texture[load_bank].tmem = rdp.tiles[tile & 7].tmem;
    /* B17G7D LOADBLOCK-DXT: preserve exact G_LOADBLOCK dxt for diagnostics. */
    rdp.loaded_texture[load_bank].load_dxt = (uint16_t)(dxt & 0x0FFFu);
    SUPPORT_CHECK(uls == 0);
    SUPPORT_CHECK(ult == 0);

    // The lrs field rather seems to be number of pixels to load
    uint32_t word_size_shift;
    switch (rdp.texture_to_load.siz) {
        case G_IM_SIZ_4b:
            word_size_shift = 0; // Or -1? It's unused in SM64 anyway.
            break;
        case G_IM_SIZ_8b:
            word_size_shift = 0;
            break;
        case G_IM_SIZ_16b:
            word_size_shift = 1;
            break;
        case G_IM_SIZ_32b:
            word_size_shift = 2;
            break;
    }
    uint32_t size_bytes = ((lrs + 1) * (4u << rdp.texture_to_load.siz) + 7) / 8;
    rdp.loaded_texture[load_bank].rows = 0;
    rdp.loaded_texture[load_bank].source_pitch = 0;
    rdp.loaded_texture[load_bank].row_bytes = 0;
    rdp.loaded_texture[load_bank].size_bytes = size_bytes;
    assert(size_bytes <= 4096 && "bug: too big texture");
    rdp.loaded_texture[load_bank].addr = rdp.texture_to_load.addr;
    rdp.loaded_texture[load_bank].load_capture_hash =
        (x360_logging_enabled() && rdp.loaded_texture[load_bank].addr && size_bytes)
        ? x360_texture_hash(rdp.loaded_texture[load_bank].addr, size_bytes) : 0;

    /* B17G11 COURSE-TMEM-BANK-RECORDER: Moo Moo dual-bank RGBA16 loads only. */
    {
        static unsigned x360_b17g11_load_logs;
        const struct X360TextureTile *lt = &rdp.tiles[tile & 7];
        if (x360_logging_enabled() && gGamestate == 4 && x360_b17g11_load_logs < 8 &&
            lt->fmt == G_IM_FMT_RGBA && rdp.texture_to_load.siz == G_IM_SIZ_16b &&
            size_bytes == 2048u && rdp.loaded_texture[load_bank].load_dxt == 256u &&
            (lt->tmem == 0u || lt->tmem == 256u)) {
            char m[320];
            _snprintf(m, sizeof(m) - 1,
                "MK64: B17G11 BANKLOAD bank=%u tile=%u tmem=%u gen=%u src=%08X bytes=%u dxt=%u hash=%08X imgW=%u\n",
                (unsigned)load_bank, (unsigned)tile, (unsigned)lt->tmem,
                (unsigned)rdp.loaded_texture[load_bank].generation,
                (unsigned)(uintptr_t)rdp.loaded_texture[load_bank].addr,
                (unsigned)size_bytes,
                (unsigned)rdp.loaded_texture[load_bank].load_dxt,
                (unsigned)rdp.loaded_texture[load_bank].load_capture_hash,
                (unsigned)rdp.texture_to_load.width);
            m[sizeof(m) - 1] = 0; x360_log(m);
            ++x360_b17g11_load_logs;
        }
    }

    /* B17G7E DXT-TMEM-LAYOUT-VERIFY: diagnostic only. Compare current linear RAM hash with the
     * DXT/TMEM odd-line layout hash. Hard-capped to 24 records. */
    {
        static unsigned x360_b17g7e_logs;
        if (x360_logging_enabled() && gGamestate == 4 && x360_b17g7e_logs < 24 &&
            rdp.texture_to_load.siz == G_IM_SIZ_8b &&
            size_bytes == 2048 &&
            rdp.loaded_texture[load_bank].addr &&
            rdp.loaded_texture[load_bank].load_dxt) {
            uint32_t dxt_row_bytes = 0, dxt_rows = 0;
            const uint32_t tmem_hash = x360_b17g7e_tmem_hash(
                rdp.loaded_texture[load_bank].addr, size_bytes,
                rdp.loaded_texture[load_bank].load_dxt,
                &dxt_row_bytes, &dxt_rows);
            char message[288];
            _snprintf(message, sizeof(message) - 1,
                "MK64: B17G7E TMEM gen=%u addr=%08X dxt=%u raw=%08X tmem=%08X dxtRowB=%u dxtRows=%u size=%u imgW=%u\n",
                (unsigned)rdp.loaded_texture[load_bank].generation,
                (unsigned)(uintptr_t)rdp.loaded_texture[load_bank].addr,
                (unsigned)rdp.loaded_texture[load_bank].load_dxt,
                (unsigned)rdp.loaded_texture[load_bank].load_capture_hash,
                (unsigned)tmem_hash,
                (unsigned)dxt_row_bytes,
                (unsigned)dxt_rows,
                (unsigned)size_bytes,
                (unsigned)rdp.texture_to_load.width);
            message[sizeof(message) - 1] = 0;
            x360_log(message);
            ++x360_b17g7e_logs;
        }
    }

    /* B17G7D LOADBLOCK-DXT: bounded diagnostic only; texture bytes are untouched. */
    {
        static unsigned x360_b17g7d_logs;
        if (x360_logging_enabled() && gGamestate == 4 && x360_b17g7d_logs < 32 &&
            /* B17G7D-v3 DXT-FILTER-FIX: classify LOADBLOCK by SetTextureImage fmt/siz. */
            /* B17G7D-v4 DXT-FILTER-FIX: texture_to_load has no fmt field; capture all 8-bit 2048-byte LOADBLOCKs and correlate with CI8 import logs. */
            rdp.texture_to_load.siz == G_IM_SIZ_8b &&
            rdp.loaded_texture[load_bank].size_bytes == 2048) {
            char message[256];
            _snprintf(message, sizeof(message) - 1,
                "MK64: B17G7D DXT gen=%u tile=%u tmem=%u dxt=%u/0x%03X lrs=%u bytes=%u imgW=%u addr=%08X hash=%08X\n",
                (unsigned)rdp.loaded_texture[load_bank].generation,
                (unsigned)tile,
                (unsigned)rdp.loaded_texture[load_bank].tmem,
                (unsigned)rdp.loaded_texture[load_bank].load_dxt,
                (unsigned)rdp.loaded_texture[load_bank].load_dxt,
                (unsigned)lrs,
                (unsigned)rdp.loaded_texture[load_bank].size_bytes,
                (unsigned)rdp.texture_to_load.width,
                (unsigned)(uintptr_t)rdp.loaded_texture[load_bank].addr,
                (unsigned)rdp.loaded_texture[load_bank].load_capture_hash);
            message[sizeof(message) - 1] = 0;
            x360_log(message);
            ++x360_b17g7d_logs;
        }
    }

    /* B17G6C diagnostic only: fingerprint source RAM at G_LOADBLOCK time during gGamestate==4.
     * Compare this hash to B15-TEX hash with the same gen. A difference
     * proves the current live-pointer model observed modified RAM after load. */
    {
        static unsigned x360_b17g6b_loadblock_logs;
        if (x360_logging_enabled() && gGamestate == 4 && x360_b17g6b_loadblock_logs++ < 8 &&
            rdp.loaded_texture[load_bank].addr && size_bytes) {
            const uint32_t hash =
                x360_texture_hash(rdp.loaded_texture[load_bank].addr, size_bytes);
            char message[256];
            _snprintf(message, sizeof(message) - 1,
                "MK64: B17G6C RACE-LOADBLOCK-CAP gen=%u bank=%u tmem=%u bytes=%u addr=%08X hash=%08X\n",
                (unsigned)rdp.loaded_texture[load_bank].generation,
                (unsigned)load_bank,
                (unsigned)rdp.loaded_texture[load_bank].tmem,
                (unsigned)size_bytes,
                (unsigned)(uintptr_t)rdp.loaded_texture[load_bank].addr,
                (unsigned)hash);
            message[sizeof(message) - 1] = 0;
            x360_log(message);
        }
    }

    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
}

static void gfx_dp_load_tile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    const uint32_t load_bank = rdp.tiles[tile & 7].tmem / 256;
    rdp.loaded_texture[load_bank].generation = ++x360_texture_generation;
    rdp.loaded_texture[load_bank].command = G_LOADTILE;
    rdp.loaded_texture[load_bank].command_address = x360_texture_command_address[3];
    memcpy(rdp.loaded_texture[load_bank].load_words,x360_texture_commands[3],sizeof(rdp.loaded_texture[load_bank].load_words));
    rdp.loaded_texture[load_bank].load_tile = tile;
    rdp.loaded_texture[load_bank].tmem = rdp.tiles[tile & 7].tmem;
    uint32_t n = load_bank;
    uint32_t bits = 4u << rdp.texture_to_load.siz;
    uint32_t x = uls >> 2, y = ult >> 2;
    uint32_t width = lrs >= uls ? ((lrs >> 2) - x + 1) : 0;
    uint32_t height = lrt >= ult ? ((lrt >> 2) - y + 1) : 0;
    uint32_t source_pitch = (rdp.texture_to_load.width * bits + 7) / 8;

    /* B17G6A: G_LOADTILE uses inclusive coordinates, but never let a row
     * claim texels beyond the source image width.  The debugger caught
     * imageWidth=320 with uls=0/lrs=1280 producing width=321 (642 bytes)
     * against a 640-byte source stride. */
    {
        const uint32_t requested_width = width;
        const uint32_t available_width =
            x < rdp.texture_to_load.width ? rdp.texture_to_load.width - x : 0;
        if (width > available_width) {
            static unsigned x360_loadtile_width_clamp_logs;
            width = available_width;
            if (x360_loadtile_width_clamp_logs++ < 8) {
                char message[224];
                _snprintf(message, sizeof(message) - 1,
                    "MK64: B17G6A LOADTILE width clamp requested=%u available=%u uls=%u lrs=%u imageWidth=%u\n",
                    (unsigned)requested_width, (unsigned)available_width,
                    (unsigned)uls, (unsigned)lrs,
                    (unsigned)rdp.texture_to_load.width);
                message[sizeof(message) - 1] = 0;
                x360_log(message);
            }
        }
    }

    /* Keep the original image stride, not the packed destination tile stride. */
    rdp.loaded_texture[n].source_pitch = source_pitch;
    rdp.loaded_texture[n].row_bytes = (width * bits + 7) / 8;
    rdp.loaded_texture[n].rows = height;
    rdp.loaded_texture[n].size_bytes = rdp.loaded_texture[n].row_bytes * height;
    rdp.loaded_texture[n].addr = rdp.texture_to_load.addr + y * source_pitch + x * bits / 8;
    /* LOADTILE is a different path; don't inherit a prior LOADBLOCK fingerprint. */
    rdp.loaded_texture[n].load_capture_hash = 0;
    rdp.tiles[tile & 7].uls = uls;
    rdp.tiles[tile & 7].ult = ult;
    rdp.tiles[tile & 7].lrs = lrs;
    rdp.tiles[tile & 7].lrt = lrt;

    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
}


static uint8_t color_comb_component(uint32_t v) {
    switch(v){case 0:return CC_COMBINED;case 1:return CC_TEXEL0;case 2:return CC_TEXEL1;
    case 3:return CC_PRIM;case 4:return CC_SHADE;case 5:return CC_ENV;default:return CC_0;}
}
static uint8_t color_mul_component(uint32_t v){
    switch(v){case 7:return CC_COMBINEDA;case 8:return CC_TEXEL0A;case 9:return CC_TEXEL1A;
    case 10:return CC_PRIMA;case 11:return CC_SHADEA;case 12:return CC_ENVA;
    case 13:return CC_LOD;case 14:return CC_PRIMLOD;default:return color_comb_component(v);}
}
static inline uint32_t color_comb(uint32_t a,uint32_t b,uint32_t c,uint32_t d){
    return (a==6?CC_ONE:color_comb_component(a))|(color_comb_component(b)<<4)|
        (color_mul_component(c)<<8)|((d==6?CC_ONE:color_comb_component(d))<<12);
}
static inline uint32_t alpha_comb(uint32_t a,uint32_t b,uint32_t c,uint32_t d){
    return (a==6?CC_ONE:color_comb_component(a))|((b==6?CC_ONE:color_comb_component(b))<<4)|
        ((c==0?CC_LOD:c==6?CC_PRIMLOD:color_comb_component(c))<<8)|((d==6?CC_ONE:color_comb_component(d))<<12);
}
static void gfx_dp_set_combine_mode(uint32_t rgb,uint32_t alpha){
    uint64_t stage=rgb|(alpha<<16);rdp.combine_mode=stage|(stage<<32);
}

static void gfx_dp_set_env_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.env_color.r = r;
    rdp.env_color.g = g;
    rdp.env_color.b = b;
    rdp.env_color.a = a;
}

static void gfx_dp_set_prim_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.prim_color.r = r;
    rdp.prim_color.g = g;
    rdp.prim_color.b = b;
    rdp.prim_color.a = a;
}

static void gfx_dp_set_fog_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.fog_color.r = r;
    rdp.fog_color.g = g;
    rdp.fog_color.b = b;
    rdp.fog_color.a = a;
    if (gfx_rapi->set_fog_color) gfx_rapi->set_fog_color(&rdp.fog_color.r);
}

static void gfx_dp_set_fill_color(uint32_t packed_color) {
    uint16_t col16 = (uint16_t)packed_color;
    uint32_t r = col16 >> 11;
    uint32_t g = (col16 >> 6) & 0x1f;
    uint32_t b = (col16 >> 1) & 0x1f;
    uint32_t a = col16 & 1;
    rdp.fill_color.r = SCALE_5_8(r);
    rdp.fill_color.g = SCALE_5_8(g);
    rdp.fill_color.b = SCALE_5_8(b);
    rdp.fill_color.a = a * 255;
}

static void gfx_draw_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, bool textured) {
    uint32_t saved_other_mode_h = rdp.other_mode_h;
    uint32_t cycle_type = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));

    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = (rdp.other_mode_h & ~(3U << G_MDSFT_TEXTFILT)) | G_TF_POINT;
    }

    // U10.2 coordinates
    float ulxf = ulx;
    float ulyf = uly;
    float lrxf = lrx;
    float lryf = lry;

    ulxf = ulxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
    ulyf = -(ulyf / (4.0f * HALF_SCREEN_HEIGHT)) + 1.0f;
    lrxf = lrxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
    lryf = -(lryf / (4.0f * HALF_SCREEN_HEIGHT)) + 1.0f;
    mkview::Rect source;
    const bool local=!x360_gfx_online_hud && x360_net_active() && !x360_net8_active() && gGamestate==4 &&
        gPlayerCountSelection1==x360_net_player_count() &&
        mkview::crop(gActiveScreenMode,x360_net_player_count(),x360_net_local_slot(),source);

    if (local) {
        if (textured) {
            mkview::hud_rect(
                ulxf, ulyf, lrxf, lryf, source,
                mkview::output(x360_legacy_wide_display()));
        }
        /*
         * Preserve the current local online-race fill behavior: clears/fades
         * cover the selected local scissor and are handled by the local-view
         * presentation path.
         */
    } else {
        /*
         * PRE-DISPLAY-OPTION BEHAVIOR:
         * normal/offline textured AND fill rectangles use the same X aspect.
         * This is required for MK64 menu panels to stay aligned.
         */
        ulxf = gfx_adjust_x_for_aspect_ratio(ulxf);
        lrxf = gfx_adjust_x_for_aspect_ratio(lrxf);
    }

    struct LoadedVertex* ul = &rsp.loaded_vertices[MAX_VERTICES + 0];
    struct LoadedVertex* ll = &rsp.loaded_vertices[MAX_VERTICES + 1];
    struct LoadedVertex* lr = &rsp.loaded_vertices[MAX_VERTICES + 2];
    struct LoadedVertex* ur = &rsp.loaded_vertices[MAX_VERTICES + 3];

    ul->x = ulxf;
    ul->y = ulyf;
    ul->z = -1.0f;
    ul->w = 1.0f;

    ll->x = ulxf;
    ll->y = lryf;
    ll->z = -1.0f;
    ll->w = 1.0f;

    lr->x = lrxf;
    lr->y = lryf;
    lr->z = -1.0f;
    lr->w = 1.0f;

    ur->x = lrxf;
    ur->y = ulyf;
    ur->z = -1.0f;
    ur->w = 1.0f;

    for (int i=0;i<4;++i) {
        LoadedVertex *v=&rsp.loaded_vertices[MAX_VERTICES+i];
        v->clip_rej=0;
        if(v->x < -v->w)v->clip_rej|=CLIP_LEFT;
        if(v->x >  v->w)v->clip_rej|=CLIP_RIGHT;
        if(v->y < -v->w)v->clip_rej|=CLIP_BOTTOM;
        if(v->y >  v->w)v->clip_rej|=CLIP_TOP;
    }
    // The coordinates for texture rectangle shall bypass the viewport setting
    struct XYWidthHeight default_viewport = {0, 0, gfx_current_dimensions.width, gfx_current_dimensions.height};
    struct XYWidthHeight viewport_saved = rdp.viewport;
    uint32_t geometry_mode_saved = rsp.geometry_mode;

    rdp.viewport = default_viewport;
    rdp.viewport_or_scissor_changed = true;
    rsp.geometry_mode = 0;

    gfx_sp_tri1(MAX_VERTICES + 0, MAX_VERTICES + 1, MAX_VERTICES + 3);
    gfx_sp_tri1(MAX_VERTICES + 1, MAX_VERTICES + 2, MAX_VERTICES + 3);

    rsp.geometry_mode = geometry_mode_saved;
    rdp.viewport = viewport_saved;
    rdp.viewport_or_scissor_changed = true;

    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = saved_other_mode_h;
    }
}

static void gfx_dp_texture_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls, int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) {
    uint8_t saved_render_tile = rdp.render_tile;
    rdp.render_tile = tile & 7;
    if (rdp.render_tile != saved_render_tile)
        rdp.textures_changed[0] = rdp.textures_changed[1] = true;
    uint64_t saved_combine_mode = rdp.combine_mode;
    if ((rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_COPY) {
        // Per RDP Command Summary Set Tile's shift s and this dsdx should be set to 4 texels
        // Divide by 4 to get 1 instead
        dsdx >>= 2;

        // Color combiner is turned off in copy mode
        gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_TEXEL0), alpha_comb(7, 7, 7, G_ACMUX_TEXEL0));

        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }

    // uls and ult are S10.5
    // dsdx and dtdy are S5.10
    // lrx, lry, ulx, uly are U10.2
    // lrs, lrt are S10.5
    if (flip) {
        dsdx = -dsdx;
        dtdy = -dtdy;
    }
    int16_t width = !flip ? lrx - ulx : lry - uly;
    int16_t height = !flip ? lry - uly : lrx - ulx;
    float lrs = ((uls << 7) + dsdx * width) >> 7;
    float lrt = ((ult << 7) + dtdy * height) >> 7;

    if (gfx_rapi->tex_rect) {
        float ulxf = ulx * ratio_x;
        float ulyf = uly * ratio_y;
        float lrxf = lrx * ratio_x;
        float lryf = lry * ratio_y;
        const float dudx = ((lrs - (float)uls) / (lrxf - ulxf));
        const float dvdy = ((lrt - (float)ult) / (lryf - ulyf));
        const bool used_textures[2] = { true, false };
        gfx_pick_combiner(NULL, NULL);
        gfx_update_textures(used_textures, false);
        ulxf = HALF_SCREEN_WIDTH + gfx_adjust_x_for_aspect_ratio(ulxf / 4.0f - HALF_SCREEN_WIDTH);
        lrxf = HALF_SCREEN_WIDTH + gfx_adjust_x_for_aspect_ratio(lrxf / 4.0f - HALF_SCREEN_WIDTH);
        ulyf = ulyf / 4.0f;
        lryf = lryf / 4.0f;
        gfx_rapi->tex_rect(ulxf, ulyf, lrxf, lryf, uls / 32.f, ult / 32.f, dudx / 8.f, dvdy / 8.f, &rdp.env_color.r);
    } else {
        struct LoadedVertex* ul = &rsp.loaded_vertices[MAX_VERTICES + 0];
        struct LoadedVertex* ll = &rsp.loaded_vertices[MAX_VERTICES + 1];
        struct LoadedVertex* lr = &rsp.loaded_vertices[MAX_VERTICES + 2];
        struct LoadedVertex* ur = &rsp.loaded_vertices[MAX_VERTICES + 3];
        ul->u = uls;
        ul->v = ult;
        lr->u = lrs;
        lr->v = lrt;
        if (!flip) {
            ll->u = uls;
            ll->v = lrt;
            ur->u = lrs;
            ur->v = ult;
        } else {
            ll->u = lrs;
            ll->v = ult;
            ur->u = uls;
            ur->v = lrt;
        }
        gfx_draw_rectangle(ulx, uly, lrx, lry, true);
    }

    rdp.combine_mode = saved_combine_mode;
    if (rdp.render_tile != saved_render_tile)
        rdp.textures_changed[0] = rdp.textures_changed[1] = true;
    rdp.render_tile = saved_render_tile;
}

/* ===== HD FULL-IMAGE DRAW (bypassa particionamento TMEM) ================
   Para imagens 2D de menu grandes demais para uma carga so de TMEM (ex:
   retratos de selecao de personagem, botoes grandes tipo "press start"):
   o jogo original as particiona em varias faixas pequenas, cada uma
   passando por import_texture() separadamente -- so ali nunca existe "a
   imagem inteira" de uma vez, so pedacos, o que impede uma troca HD por
   hash de conteudo (cada pedaco tem hash diferente e o numero de pedacos
   e dificil de replicar exatamente).
   Esta funcao e chamada ANTES desse particionamento comecar (a partir de
   render_menu_textures em menu_items.c, que ainda tem o ponteiro pra
   imagem INTEIRA e a dimensao real declarada). Se existir HD para o hash
   da imagem completa, desenha ela de uma vez via gfx_dp_texture_rectangle
   (o mesmo caminho interno usado por todo retangulo texturizado 2D deste
   port), num tamanho de textura livre (nao precisa caber em 4KB) -- e
   devolve 1 para o chamador pular o caminho original particionado.
   Se nao existir HD, devolve 0 sem nenhum efeito colateral (a chamada e
   so um hash + tentativa de leitura de arquivo, barata). */

/* Cache de texturas HD de menu ja carregadas: hash -> texture_id na GPU.
   Sem isto a funcao reabria o arquivo do disco e reenviava a textura para a
   GPU a CADA QUADRO de CADA imagem de menu -- causa direta dos engasgos nos
   menus de selecao. */
#define X360_HDMENU_MAX 512
static uint32_t x360_hdmenu_hash[X360_HDMENU_MAX];
static uint32_t x360_hdmenu_texid[X360_HDMENU_MAX];
static uint32_t x360_hdmenu_w[X360_HDMENU_MAX];
static uint32_t x360_hdmenu_h[X360_HDMENU_MAX];
static unsigned x360_hdmenu_count;
static struct TextureHashmapNode x360_hd_menu_node;

static int x360_hdmenu_find(uint32_t hash) {
    for (unsigned i = 0; i < x360_hdmenu_count; ++i)
        if (x360_hdmenu_hash[i] == hash) return (int)i;
    return -1;
}

/* TKMK00 tile-mode bridge: the original menu code loads these images in
   TMEM bands. Keep the proven menu renderer untouched for TKMK00 so the
   normal texture pipeline can resolve each band from tex.pak by its original hash. */
static bool x360_is_tkmk00_full_hash(uint32_t hash) {
    switch (hash) {
        case 0x55a88dcau:
        case 0x151886fbu:
        case 0x59a45e1bu:
        case 0x916164e7u:
        case 0xcd2d2e87u:
        case 0x830c0246u:
        case 0x3e97679au:
        case 0x5e434358u:
        case 0x111abc49u:
        case 0xf4a27dedu:
        case 0x9fcdea9du:
        case 0x7104183au:
        case 0x3762a661u:
        case 0x41d314b4u:
        case 0xa51bf048u:
        case 0x307a3c33u:
        case 0x7e4e34f7u:
        case 0x2b187347u:
        case 0xa74f0fc1u:
        case 0x6dc8dfd1u:
        case 0x13f49498u:
        case 0xe0806470u:
        case 0x5217cf79u:
        case 0xf88adb50u:
        case 0xd1a7e1cdu:
        case 0xdca32287u:
        case 0xf1db2348u:
        case 0x37014dc3u:
        case 0x5fa1fa92u:
        case 0x9b2de0e5u:
        case 0xc9c80027u:
        case 0x88635819u:
        case 0x7fd56f8eu:
        case 0xb1ac6d62u:
        case 0xc08c182eu:
        case 0x5544cbbau:
        case 0x79fcbc0fu:
        case 0xd1eda2e8u:
        case 0x9b105ea5u:
        case 0xe700a09du:
        case 0xb9cc6440u:
        case 0xa9a455efu:
        case 0x6d9b6735u:
        case 0xe011eed4u:
        case 0xa5b3f189u:
        case 0x9d556ae0u:
        case 0x8eb95a8cu:
        case 0xbc096deau:
        case 0x87b03a70u:
        case 0xf4fffd39u:
        case 0xb2d74335u:
        case 0xe40a7d4au:
        case 0x5317a1a8u:
        case 0x02453e44u:
        case 0xecaffb70u:
        case 0xdff91b13u:
        case 0xa5e96f2cu:
        case 0x6e5b02e9u:
        case 0x67f0069du:
        case 0x8991dad6u:
        case 0xaf1ef0cbu:
        case 0xb329eb0bu:
        case 0x9c2106c8u:
            return true;
        default:
            return false;
    }
}

extern "C" int x360_try_draw_hd_menu_quad(const uint8_t *fullImageSource, uint32_t texDeclaredW,
                                           uint32_t texDeclaredH, int32_t dstX, int32_t dstY,
                                           int32_t dstW, int32_t dstH) {
    if (!fullImageSource || !texDeclaredW || !texDeclaredH || dstW <= 0 || dstH <= 0) return 0;

    /* RGBA16 e o formato fixo usado por render_menu_textures. */
    uint32_t source_size = texDeclaredW * texDeclaredH * 2;
    uint32_t hash = x360_texture_hash(fullImageSource, source_size);

    /* TKMK00 is deliberately left to the original func_80095E10() TMEM
       path. Its individual tile hashes are supplied by tex.pak. */
    if (x360_is_tkmk00_full_hash(hash)) return 0;

    int slot = x360_hdmenu_find(hash);
    if (slot < 0) {
        /* Ja sabemos que nao existe HD para este hash: nada de tocar no disco. */
        if (x360_hdtex_lookup(hash) == X360_HDTEX_ST_MISS) return 0;
        if (x360_hdmenu_count >= X360_HDMENU_MAX) return 0;

        char path[64];
        /* Subpasta pelos 2 primeiros digitos do hash: o FATX do Xbox 360 nao
       aceita mais de 4096 entradas por pasta, e o elenco completo passa
       disso. Com 256 subpastas sobram ~20 arquivos em cada. */
    _snprintf(path, sizeof(path) - 1, "game:\\tex\\%02x\\%08x.tex",
              (unsigned)((hash >> 24) & 0xFF), hash);
        path[sizeof(path) - 1] = 0;
        HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (f == INVALID_HANDLE_VALUE) {
            x360_hdtex_remember(hash, X360_HDTEX_ST_MISS);
            return 0;
        }
        struct X360HDTexHeader hdr;
        DWORD got = 0;
        bool ok = ReadFile(f, &hdr, sizeof(hdr), &got, NULL) && got == sizeof(hdr)
                  && hdr.magic == X360_HDTEX_MAGIC
                  && hdr.width > 0 && hdr.height > 0
                  && hdr.width <= 2048 && hdr.height <= 2048;
        if (ok) {
            uint32_t need = hdr.width * hdr.height * 4;
            DWORD readBytes = 0;
            ok = ReadFile(f, x360_hd_buf, need, &readBytes, NULL) && readBytes == need;
        }
        CloseHandle(f);
        if (!ok) {
            x360_hdtex_remember(hash, X360_HDTEX_ST_MISS);
            return 0;
        }

        /* Carrega uma unica vez para a GPU e guarda o id. */
        uint32_t tid = gfx_rapi->new_texture();
        gfx_rapi->select_texture(0, tid);
        gfx_rapi->upload_texture(x360_hd_buf, hdr.width, hdr.height);
        gfx_rapi->set_sampler_parameters(0, true, G_TX_CLAMP, G_TX_CLAMP);
        slot = (int)x360_hdmenu_count++;
        x360_hdmenu_hash[slot] = hash;
        x360_hdmenu_texid[slot] = tid;
        x360_hdmenu_w[slot] = hdr.width;
        x360_hdmenu_h[slot] = hdr.height;
        x360_hdtex_remember(hash, X360_HDTEX_ST_HIT);
    } else {
        gfx_rapi->select_texture(0, x360_hdmenu_texid[slot]);
        gfx_rapi->set_sampler_parameters(0, true, G_TX_CLAMP, G_TX_CLAMP);
    }

    /* width/height do node ficam com a dimensao DECLARADA original: e o que a
       normalizacao de UV (u/width, v/height) espera para mapear a imagem
       inteira no retangulo de destino, independente da resolucao do arquivo HD. */
    x360_hd_menu_node.fmt = G_IM_FMT_RGBA;
    x360_hd_menu_node.siz = G_IM_SIZ_16b;
    x360_hd_menu_node.texture_unit = 0;
    x360_hd_menu_node.width = texDeclaredW;
    x360_hd_menu_node.height = texDeclaredH;
    x360_hd_menu_node.cms = G_TX_CLAMP;
    x360_hd_menu_node.cmt = G_TX_CLAMP;
    x360_hd_menu_node.linear_filter = true;
    x360_hd_menu_node.texture_id = x360_hdmenu_texid[slot];
    rendering_state.textures[0] = &x360_hd_menu_node;
    rendering_state.sampler[0].valid = true;
    rendering_state.sampler[0].linear = true;
    rendering_state.sampler[0].cms = G_TX_CLAMP;
    rendering_state.sampler[0].cmt = G_TX_CLAMP;
    /* Nao marcar como "mudou": isso faria o pipeline chamar import_texture(0)
       e sobrescrever a textura HD com a original. */
    rdp.textures_changed[0] = false;

    /* O tile do RDP e de onde a normalizacao de UV le uls/ult/shifts -- sem
       zerar aqui, herdaria o offset do desenho anterior. */
    {
        struct X360TextureTile *t = &rdp.tiles[0];
        t->fmt = G_IM_FMT_RGBA;
        t->siz = G_IM_SIZ_16b;
        t->cms = G_TX_CLAMP;
        t->cmt = G_TX_CLAMP;
        t->palette = 0;
        t->masks = 0; t->maskt = 0;
        t->shifts = 0; t->shiftt = 0;
        t->uls = 0; t->ult = 0;
        t->lrs = (uint16_t)((texDeclaredW - 1) << 2);
        t->lrt = (uint16_t)((texDeclaredH - 1) << 2);
        t->tmem = 0;
        t->line_size_bytes = texDeclaredW * 2;
    }

    int16_t dsdx = (int16_t)((texDeclaredW << 10) / (uint32_t)dstW);
    int16_t dtdy = (int16_t)((texDeclaredH << 10) / (uint32_t)dstH);

    /* Desenhamos de forma sincrona, antes dos gSPDisplayList que o codigo
       original emite para configurar o estado -- entao configuramos tudo
       explicitamente aqui, senao o desenho sai invisivel (combiner/alpha
       zerados, ou descartado pelo teste de profundidade do 3D anterior). */
    uint64_t saved_combine = rdp.combine_mode;
    uint32_t saved_other_h = rdp.other_mode_h;
    uint32_t saved_other_l = rdp.other_mode_l;
    struct RGBA saved_env = rdp.env_color;
    struct RGBA saved_prim = rdp.prim_color;

    gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_TEXEL0),
                             alpha_comb(0, 0, 0, G_ACMUX_TEXEL0));
    rdp.env_color.r = rdp.env_color.g = rdp.env_color.b = rdp.env_color.a = 255;
    rdp.prim_color.r = rdp.prim_color.g = rdp.prim_color.b = rdp.prim_color.a = 255;
    rdp.other_mode_h = (rdp.other_mode_h & ~(3U << G_MDSFT_CYCLETYPE)) | G_CYC_1CYCLE;
    rdp.other_mode_h = (rdp.other_mode_h & ~(3U << G_MDSFT_TEXTFILT)) | G_TF_BILERP;
    /* 2D transparente, sem teste/escrita de profundidade. */
    rdp.other_mode_l = G_RM_XLU_SURF | G_RM_XLU_SURF2;

    gfx_dp_texture_rectangle(dstX << 2, dstY << 2, (dstX + dstW) << 2, (dstY + dstH) << 2,
                              0, 0, 0, dsdx, dtdy, false);
    gfx_flush();

    rdp.combine_mode = saved_combine;
    rdp.other_mode_h = saved_other_h;
    rdp.other_mode_l = saved_other_l;
    rdp.env_color = saved_env;
    rdp.prim_color = saved_prim;

    /* Invalida o binding: o node HD e temporario, fora do pool do cache. */
    rendering_state.textures[0] = NULL;
    rendering_state.sampler[0].valid = false;
    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
    return 1;
}
/* ===== HD FULL-IMAGE DRAW (fim) ========================================== */

static void gfx_dp_fill_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    if (rdp.color_image_address == rdp.z_buf_address) {
        // Don't clear Z buffer here since we already did it with glClear
        return;
    }
    uint32_t mode = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));

    if (mode == G_CYC_COPY || mode == G_CYC_FILL) {
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }

    const uint64_t saved_combine_mode = rdp.combine_mode;
    gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_SHADE), alpha_comb(7, 7, 7, G_ACMUX_SHADE));

    if (gfx_rapi->fill_rect) {
        float ulxf = ulx * ratio_x;
        float ulyf = uly * ratio_y;
        float lrxf = lrx * ratio_x;
        float lryf = lry * ratio_y;
        gfx_pick_combiner(NULL, NULL);
        ulxf = HALF_SCREEN_WIDTH + gfx_adjust_x_for_aspect_ratio(ulxf / 4.0f - HALF_SCREEN_WIDTH);
        lrxf = HALF_SCREEN_WIDTH + gfx_adjust_x_for_aspect_ratio(lrxf / 4.0f - HALF_SCREEN_WIDTH);
        ulyf = ulyf / 4.0f;
        lryf = lryf / 4.0f;
        gfx_rapi->fill_rect(ulxf, ulyf, lrxf, lryf, &rdp.fill_color.r);
    } else {
        for (int i = MAX_VERTICES; i < MAX_VERTICES + 4; i++) {
            struct LoadedVertex* v = &rsp.loaded_vertices[i];
            v->color = rdp.fill_color;
        }
        gfx_draw_rectangle(ulx, uly, lrx, lry, false);
    }

    rdp.combine_mode = saved_combine_mode;
}

static void gfx_dp_set_z_image(void *z_buf_address) {
    rdp.z_buf_address = z_buf_address;
}

static void gfx_dp_set_color_image(uint32_t format, uint32_t size, uint32_t width, void* address) {
    rdp.color_image_address = address;
}

static void gfx_sp_set_other_mode(uint32_t shift, uint32_t num_bits, uint64_t mode) {
    uint64_t mask = (((uint64_t)1 << num_bits) - 1) << shift;
    uint64_t om = rdp.other_mode_l | ((uint64_t)rdp.other_mode_h << 32);
    om = (om & ~mask) | mode;
    if (((rdp.other_mode_h ^ (uint32_t)(om >> 32)) & (3U << G_MDSFT_TEXTLUT)) != 0)
        rdp.textures_changed[0] = rdp.textures_changed[1] = true;
    rdp.other_mode_l = (uint32_t)om;
    rdp.other_mode_h = (uint32_t)(om >> 32);
}

static inline void *seg_addr(uintptr_t w1) {
    return x360_resolve_address(w1,rspSegments);
}

/*
 * B8 race-transition diagnostic.
 *
 * B7 hardware faulted inside gfx_sp_vertex while reading 0x62626262. Catch
 * obviously poisoned/repeated-byte vertex addresses before dereferencing them
 * and record the raw G_VTX word plus the segment state which produced it.
 */
static bool x360_vtx_address_looks_poisoned(uintptr_t address) {
    const uint32_t a = (uint32_t) address;
    const uint32_t b0 = (a >> 24) & 0xFFU;
    const uint32_t b1 = (a >> 16) & 0xFFU;
    const uint32_t b2 = (a >> 8) & 0xFFU;

    /*
     * Current Xbox native code/data/heap observed by this port is in the
     * 0x82/0x83... range. B8 proved that 0x62626262 was raw command-stream
     * garbage; the next fault was 0xA0000000. Keep this as diagnostic
     * containment, not as the final pointer model.
     */
    if (a < 0x80000000U || a >= 0xA0000000U) {
        return true;
    }

    /* Also catches nearby repeated-byte poison patterns. */
    return b0 == b1 && b1 == b2;
}

static void x360_record_gdl(Gfx *source, uint32_t raw_target,
                            uintptr_t resolved_target, unsigned push) {
    struct X360GdlHistoryEntry *entry =
        &x360_gdl_history[x360_gdl_history_head];
    const uint32_t segment = raw_target >> 24;

    entry->sequence = ++x360_gdl_sequence;
    entry->source = (uintptr_t)source;
    entry->source_w0 = source->words.w0;
    entry->raw_target = raw_target;
    entry->resolved_target = resolved_target;
    entry->segment = (uint8_t)segment;
    entry->segment_base = segment < 16 ? rspSegments[segment] : 0;
    entry->push = (uint8_t)push;

    x360_gdl_history_head =
        (x360_gdl_history_head + 1) % X360_GDL_HISTORY_COUNT;
    if (x360_gdl_history_used < X360_GDL_HISTORY_COUNT) {
        ++x360_gdl_history_used;
    }
}

static void x360_dump_gdl_history(void) {
    char message[320];
    const unsigned used = x360_gdl_history_used;
    const unsigned start =
        (x360_gdl_history_head + X360_GDL_HISTORY_COUNT - used) %
        X360_GDL_HISTORY_COUNT;

    _snprintf(message, sizeof(message) - 1,
              "MK64: GDL-HIST begin count=%u\n", used);
    message[sizeof(message) - 1] = 0;
    x360_log(message);

    for (unsigned i = 0; i < used; ++i) {
        const struct X360GdlHistoryEntry *entry =
            &x360_gdl_history[(start + i) % X360_GDL_HISTORY_COUNT];
        _snprintf(message, sizeof(message) - 1,
                  "MK64: GDL-HIST seq=%u src=%08X w0=%08X raw=%08X "
                  "resolved=%08X push=%u seg=%u segbase=%08X\n",
                  entry->sequence,
                  (unsigned)entry->source,
                  (unsigned)entry->source_w0,
                  (unsigned)entry->raw_target,
                  (unsigned)entry->resolved_target,
                  (unsigned)entry->push,
                  (unsigned)entry->segment,
                  (unsigned)entry->segment_base);
        message[sizeof(message) - 1] = 0;
        x360_log(message);
    }
    x360_log("MK64: GDL-HIST end\n");
}

static bool x360_is_quarantined_dl(Gfx *root) {
    for (unsigned i = 0; i < x360_bad_dl_root_count; ++i) {
        if (x360_bad_dl_roots[i] == root) {
            Gfx *bad = x360_bad_dl_bad_cmd[i];
            const uintptr_t addr = (uintptr_t)bad;
            if (bad != NULL && addr >= 0x80000000U && addr < 0xA0000000U &&
                bad->words.w0 == x360_bad_dl_bad_w0[i] &&
                bad->words.w1 == x360_bad_dl_bad_w1[i]) {
                return true;
            }

            const unsigned last = x360_bad_dl_root_count - 1;
            Gfx *released = x360_bad_dl_roots[i];
            x360_bad_dl_roots[i] = x360_bad_dl_roots[last];
            x360_bad_dl_bad_cmd[i] = x360_bad_dl_bad_cmd[last];
            x360_bad_dl_bad_w0[i] = x360_bad_dl_bad_w0[last];
            x360_bad_dl_bad_w1[i] = x360_bad_dl_bad_w1[last];
            --x360_bad_dl_root_count;

            char message[160];
            _snprintf(message,sizeof(message)-1,
                      "MK64: RELEASE-DL root=%08X remaining=%u\n",
                      (unsigned)(uintptr_t)released,x360_bad_dl_root_count);
            message[sizeof(message)-1]=0; x360_log(message);
            return false;
        }
    }
    return false;
}

static void x360_quarantine_dl(Gfx *root, Gfx *bad_cmd) {
    if (root == NULL) return;

    for (unsigned i = 0; i < x360_bad_dl_root_count; ++i) {
        if (x360_bad_dl_roots[i] == root) {
            x360_bad_dl_bad_cmd[i] = bad_cmd;
            x360_bad_dl_bad_w0[i] = bad_cmd ? bad_cmd->words.w0 : 0;
            x360_bad_dl_bad_w1[i] = bad_cmd ? bad_cmd->words.w1 : 0;
            return;
        }
    }

    if (x360_bad_dl_root_count < X360_BAD_DL_ROOT_MAX) {
        const unsigned slot = x360_bad_dl_root_count++;
        x360_bad_dl_roots[slot] = root;
        x360_bad_dl_bad_cmd[slot] = bad_cmd;
        x360_bad_dl_bad_w0[slot] = bad_cmd ? bad_cmd->words.w0 : 0;
        x360_bad_dl_bad_w1[slot] = bad_cmd ? bad_cmd->words.w1 : 0;
    }

    char message[272];
    _snprintf(message,sizeof(message)-1,
              "MK64: QUARANTINE-DL root=%08X badcmd=%08X w0=%08X raw=%08X count=%u content-lock=1\n",
              (unsigned)(uintptr_t)root,
              (unsigned)(uintptr_t)bad_cmd,
              bad_cmd ? (unsigned)bad_cmd->words.w0 : 0,
              bad_cmd ? (unsigned)bad_cmd->words.w1 : 0,
              x360_bad_dl_root_count);
    message[sizeof(message)-1]=0; x360_log(message);
}

static bool x360_validate_vtx_pointer(Gfx *cmd, size_t n_vertices,
                                      size_t dest_index, const Vtx *vertices) {
    const uintptr_t resolved = (uintptr_t) vertices;

    if (!x360_vtx_address_looks_poisoned(resolved)) {
        return true;
    }

    if (x360_bad_vtx_log_count < 16) {
        const uint32_t raw = cmd->words.w1;
        const uint32_t segment = raw >> 24;
        const uintptr_t raw_segment_base = segment < 16 ? rspSegments[segment] : 0;
        char message[512];

        _snprintf(message, sizeof(message) - 1,
                  "MK64: BAD-VTX[%u] cmd=%08X w0=%08X raw=%08X resolved=%08X "
                  "seg=%u segbase=%08X n=%u dst=%u "
                  "S4=%08X S5=%08X S6=%08X S7=%08X S9=%08X SF=%08X\n",
                  x360_bad_vtx_log_count,
                  (unsigned)(uintptr_t)cmd,
                  (unsigned)cmd->words.w0,
                  (unsigned)raw,
                  (unsigned)resolved,
                  (unsigned)segment,
                  (unsigned)raw_segment_base,
                  (unsigned)n_vertices,
                  (unsigned)dest_index,
                  (unsigned)rspSegments[4],
                  (unsigned)rspSegments[5],
                  (unsigned)rspSegments[6],
                  (unsigned)rspSegments[7],
                  (unsigned)rspSegments[9],
                  (unsigned)rspSegments[15]);
        message[sizeof(message) - 1] = 0;
        x360_log(message);
        ++x360_bad_vtx_log_count;
    }

    /*
     * Diagnostic containment only. The BAD-VTX line tells us whether the bad
     * address came directly from the command or from segment resolution.
     */
    return false;
}

/* B17G4: reject poisoned top-level lists before earlier commands in the same
 * list can mutate textures, combiner state, or buffered triangles. */
/* B17G12 GEOMETRY-GUARD-AND-RECORDER
 * The captured video shows giant random/translucent triangles. Record and
 * reject impossible display-list geometry before it can index outside
 * rsp.loaded_vertices. This runs only when a bad command is found.
 */
static unsigned x360_b17g12_bad_geom_logs;

static void x360_b17g12_log_bad_geom(Gfx *root, Gfx *cmd, const char *kind,
                                     unsigned a, unsigned b, unsigned c,
                                     unsigned d, unsigned e, unsigned f) {
    if (!x360_logging_enabled() || x360_b17g12_bad_geom_logs >= 12 || root == NULL || cmd == NULL) return;

    char m[512];
    const int index = (int)(cmd - root);

    _snprintf(m, sizeof(m) - 1,
              "MK64: B17G12 BAD-GEOM kind=%s root=%08X cmd=%08X idx=%d op=%02X "
              "w0=%08X w1=%08X vals=%u,%u,%u,%u,%u,%u\n",
              kind,
              (unsigned)(uintptr_t)root,
              (unsigned)(uintptr_t)cmd,
              index,
              (unsigned)(cmd->words.w0 >> 24),
              (unsigned)cmd->words.w0,
              (unsigned)cmd->words.w1,
              a, b, c, d, e, f);
    m[sizeof(m) - 1] = 0;
    x360_log(m);

    /* B17G13 RACE-TIMER-CORRELATION: exact HUD timer at corruption event. */
    {
        const unsigned t100 = gCourseTimer > 0.0f
            ? (unsigned)(gCourseTimer * 100.0f) : 0u;
        _snprintf(m, sizeof(m) - 1,
                  "MK64: B17G13 EVENTTIME t100=%u min=%u sec=%u hund=%u\n",
                  t100, t100 / 6000u, (t100 / 100u) % 60u, t100 % 100u);
        m[sizeof(m) - 1] = 0;
        x360_log(m);
    }

    for (int rel = -3; rel <= 3; ++rel) {
        if (index + rel < 0) continue;
        Gfx *p = cmd + rel;
        _snprintf(m, sizeof(m) - 1,
                  "MK64: B17G12 GDLCTX rel=%d at=%08X w0=%08X w1=%08X op=%02X\n",
                  rel,
                  (unsigned)(uintptr_t)p,
                  (unsigned)p->words.w0,
                  (unsigned)p->words.w1,
                  (unsigned)(p->words.w0 >> 24));
        m[sizeof(m) - 1] = 0;
        x360_log(m);
    }

    _snprintf(m, sizeof(m) - 1,
              "MK64: B17G12 SEG S0=%08X S1=%08X S2=%08X S3=%08X "
              "S4=%08X S5=%08X S6=%08X S7=%08X\n",
              (unsigned)rspSegments[0], (unsigned)rspSegments[1],
              (unsigned)rspSegments[2], (unsigned)rspSegments[3],
              (unsigned)rspSegments[4], (unsigned)rspSegments[5],
              (unsigned)rspSegments[6], (unsigned)rspSegments[7]);
    m[sizeof(m) - 1] = 0;
    x360_log(m);

    _snprintf(m, sizeof(m) - 1,
              "MK64: B17G12 SEG S8=%08X S9=%08X SA=%08X SB=%08X "
              "SC=%08X SD=%08X SE=%08X SF=%08X\n",
              (unsigned)rspSegments[8], (unsigned)rspSegments[9],
              (unsigned)rspSegments[10], (unsigned)rspSegments[11],
              (unsigned)rspSegments[12], (unsigned)rspSegments[13],
              (unsigned)rspSegments[14], (unsigned)rspSegments[15]);
    m[sizeof(m) - 1] = 0;
    x360_log(m);

    ++x360_b17g12_bad_geom_logs;
}

static Gfx *x360_preflight_bad_vtx(Gfx *root) {
    if (root == NULL) return NULL;

    for (unsigned i = 0; i < 256; ++i) {
        Gfx *p = root + i;
        const uint32_t op = p->words.w0 >> 24;

        if (op == (uint8_t)G_ENDDL) break;

        if (op == G_VTX) {
#ifdef F3DEX_GBI_2
            const unsigned n = (unsigned)((p->words.w0 >> 12) & 0xFFu);
            const unsigned encoded = (unsigned)((p->words.w0 >> 1) & 0x7Fu);
            const unsigned dst = encoded >= n ? encoded - n : MAX_VERTICES + 1u;
#elif defined(F3DEX_GBI) || defined(F3DLP_GBI)
            const unsigned n = (unsigned)((p->words.w0 >> 10) & 0x3Fu);
            const unsigned dst = (unsigned)(((p->words.w0 >> 16) & 0xFFu) / 2u);
#else
            const unsigned n = (unsigned)((p->words.w0 & 0xFFFFu) / sizeof(Vtx));
            const unsigned dst = (unsigned)((p->words.w0 >> 16) & 0xFu);
#endif
            const uintptr_t resolved = (uintptr_t)seg_addr(p->words.w1);
            const bool range_ok =
                dst <= MAX_VERTICES &&
                n <= MAX_VERTICES &&
                n <= (unsigned)MAX_VERTICES - dst;

            if (!range_ok || x360_vtx_address_looks_poisoned(resolved)) {
                x360_b17g12_log_bad_geom(root, p, "VTX",
                                         n, dst, (unsigned)resolved, 0, 0, 0);
                return p;
            }
        }

        if (op == (uint8_t)G_TRI1) {
#ifdef F3DEX_GBI_2
            const unsigned a = ((p->words.w0 >> 16) & 0xFFu) / 2u;
            const unsigned b = ((p->words.w0 >> 8) & 0xFFu) / 2u;
            const unsigned c = (p->words.w0 & 0xFFu) / 2u;
#elif defined(F3DEX_GBI) || defined(F3DLP_GBI)
            const unsigned a = ((p->words.w1 >> 16) & 0xFFu) / 2u;
            const unsigned b = ((p->words.w1 >> 8) & 0xFFu) / 2u;
            const unsigned c = (p->words.w1 & 0xFFu) / 2u;
#else
            const unsigned a = ((p->words.w1 >> 16) & 0xFFu) / 10u;
            const unsigned b = ((p->words.w1 >> 8) & 0xFFu) / 10u;
            const unsigned c = (p->words.w1 & 0xFFu) / 10u;
#endif
            if (a >= MAX_VERTICES || b >= MAX_VERTICES || c >= MAX_VERTICES) {
                x360_b17g12_log_bad_geom(root, p, "TRI1", a, b, c, 0, 0, 0);
                return p;
            }
        }

#if defined(F3DEX_GBI) || defined(F3DLP_GBI)
        if (op == (uint8_t)G_TRI2) {
            const unsigned a = ((p->words.w0 >> 16) & 0xFFu) / 2u;
            const unsigned b = ((p->words.w0 >> 8) & 0xFFu) / 2u;
            const unsigned c = (p->words.w0 & 0xFFu) / 2u;
            const unsigned d = ((p->words.w1 >> 16) & 0xFFu) / 2u;
            const unsigned e = ((p->words.w1 >> 8) & 0xFFu) / 2u;
            const unsigned f = (p->words.w1 & 0xFFu) / 2u;

            if (a >= MAX_VERTICES || b >= MAX_VERTICES || c >= MAX_VERTICES ||
                d >= MAX_VERTICES || e >= MAX_VERTICES || f >= MAX_VERTICES) {
                x360_b17g12_log_bad_geom(root, p, "TRI2", a, b, c, d, e, f);
                return p;
            }
        }
#ifdef F3D_OLD
        if (op == (uint8_t)G_QUAD) {
            const unsigned v3 = ((p->words.w1 >> 24) & 0xFFu) / 2u;
            const unsigned v0 = ((p->words.w1 >> 16) & 0xFFu) / 2u;
            const unsigned v1 = ((p->words.w1 >> 8) & 0xFFu) / 2u;
            const unsigned v2 = (p->words.w1 & 0xFFu) / 2u;

            if (v0 >= MAX_VERTICES || v1 >= MAX_VERTICES ||
                v2 >= MAX_VERTICES || v3 >= MAX_VERTICES) {
                x360_b17g12_log_bad_geom(root, p, "QUAD",
                                         v0, v1, v2, v3, 0, 0);
                return p;
            }
        }
#endif
#endif
    }

    return NULL;
}


#define C0(pos, width) ((cmd->words.w0 >> (pos)) & ((1U << width) - 1))
#define C1(pos, width) ((cmd->words.w1 >> (pos)) & ((1U << width) - 1))

static void gfx_run_dl(Gfx* cmd) {
    Gfx *const dl_root = cmd;
    if (x360_is_quarantined_dl(dl_root)) return;

    /* Preflight is a diagnostic-only second walk. Runtime validation below
     * still rejects invalid vertices/triangles with logging disabled. */
    Gfx *const preflight_bad = x360_logging_enabled() ? x360_preflight_bad_vtx(dl_root) : NULL;
    if (preflight_bad != NULL) {
        /* MK64_SPLITGFX1_BAD_VTX_CONTINUE
         * Runtime validation below remains authoritative. */
        if (x360_bad_vtx_log_count < 16) {
            char message[256];
            _snprintf(message,sizeof(message)-1,
                      "MK64: SPLITGFX1 PREFLIGHT-NONFATAL root=%08X cmd=%08X w0=%08X raw=%08X\n",
                      (unsigned)(uintptr_t)dl_root,
                      (unsigned)(uintptr_t)preflight_bad,
                      (unsigned)preflight_bad->words.w0,
                      (unsigned)preflight_bad->words.w1);
            message[sizeof(message)-1]=0; x360_log(message);
        }
    }

    const struct RDP rdp_entry = rdp;
    const size_t buf_entry_len = buf_vbo_len;
    const size_t buf_entry_tris = buf_vbo_num_tris;
    bool x360_splitgfx_vertex_stream_valid = true; /* MK64_SPLITGFX1_BAD_VTX_CONTINUE */
    static unsigned x360_splitgfx3_bad_tri_logs = 0; /* MK64_SPLITGFX3_BAD_TRI_ABORT_NO_QUARANTINE */
    unsigned x360_splitgfx_logs = 0;

    for (;;) {
        uint32_t opcode = cmd->words.w0 >> 24;
        int trace = opcode == G_SETTIMG ? 0 : opcode == G_SETTILE ? 1 :
            opcode == G_LOADBLOCK ? 2 : opcode == G_LOADTILE ? 3 : opcode == G_SETTILESIZE ? 4 : -1;
        if (trace >= 0 && x360_logging_enabled()) {
            x360_texture_command_address[trace] = (uintptr_t)cmd;
            x360_texture_commands[trace][0] = cmd->words.w0;
            x360_texture_commands[trace][1] = cmd->words.w1;
        }

        switch (opcode) {
            case G_NOOP:
                if (cmd->words.w1 == X360_ONLINE_HUD_BEGIN ||
                    cmd->words.w1 == X360_ONLINE_HUD_END) {
                    /* Flush with the OLD crop policy before changing phase. */
                    gfx_flush();
                    x360_gfx_online_hud = cmd->words.w1 == X360_ONLINE_HUD_BEGIN;
                }
                break;
            // RSP commands:
            case G_MTX:
#ifdef F3DEX_GBI_2
                gfx_sp_matrix(C0(0, 8) ^ G_MTX_PUSH, (const int32_t *) seg_addr(cmd->words.w1));
#else
                gfx_sp_matrix(C0(16, 8), (const int32_t *) seg_addr(cmd->words.w1));
#endif
                break;
            case (uint8_t)G_POPMTX:
#ifdef F3DEX_GBI_2
                gfx_sp_pop_matrix(cmd->words.w1 / 64);
#else
                gfx_sp_pop_matrix(1);
#endif
                break;
            case G_MOVEMEM:
#ifdef F3DEX_GBI_2
                gfx_sp_movemem(C0(0, 8), C0(8, 8) * 8, seg_addr(cmd->words.w1));
#else
                gfx_sp_movemem(C0(16, 8), 0, seg_addr(cmd->words.w1));
#endif
                break;
            case (uint8_t)G_MOVEWORD:
#ifdef F3DEX_GBI_2
                gfx_sp_moveword(C0(16, 8), C0(0, 16), cmd->words.w1);
#else
                gfx_sp_moveword(C0(0, 8), C0(8, 16), cmd->words.w1);
#endif
                break;
            case (uint8_t)G_TEXTURE:
#ifdef F3DEX_GBI_2
                gfx_sp_texture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(1, 7));
#else
                gfx_sp_texture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(0, 8));
#endif
                break;
            case G_VTX:
                x360_last_vtx_w0 = cmd->words.w0;
                x360_last_vtx_w1 = cmd->words.w1;
#ifdef F3DEX_GBI_2
                gfx_sp_vertex(C0(12, 8), C0(1, 7) - C0(12, 8), (const Vtx *)seg_addr(cmd->words.w1));
#elif defined(F3DEX_GBI) || defined(F3DLP_GBI)
                {
                    const size_t n_vertices = C0(10, 6);
                    const size_t dest_index = C0(16, 8) / 2;
                    const Vtx *vertices = (const Vtx *)seg_addr(cmd->words.w1);
                    const bool x360_b17g12_range_ok =
                        dest_index <= MAX_VERTICES && n_vertices <= MAX_VERTICES &&
                        n_vertices <= MAX_VERTICES - dest_index;
                    if (!x360_b17g12_range_ok) {
                        x360_b17g12_log_bad_geom(dl_root, cmd, "VTX-RANGE",
                            (unsigned)n_vertices, (unsigned)dest_index,
                            (unsigned)(uintptr_t)vertices, 0, 0, 0);
                    }
                    if (x360_b17g12_range_ok &&
                                            x360_validate_vtx_pointer(cmd, n_vertices, dest_index, vertices)) {
                                            if (x360_logging_enabled() && !x360_splitgfx_vertex_stream_valid && x360_splitgfx_logs < 8U) {
                                                char message[224];
                                                _snprintf(message,sizeof(message)-1,
                                                          "MK64: SPLITGFX1 RECOVER-VTX root=%08X cmd=%08X n=%u dst=%u\n",
                                                          (unsigned)(uintptr_t)dl_root,
                                                          (unsigned)(uintptr_t)cmd,
                                                          (unsigned)n_vertices,
                                                          (unsigned)dest_index);
                                                message[sizeof(message)-1]=0; x360_log(message);
                                                ++x360_splitgfx_logs;
                                            }
                                            gfx_sp_vertex(n_vertices, dest_index, vertices);
                                            x360_splitgfx_vertex_stream_valid = true;
                                        } else {
                                            /*
                                             * Keep parsing the dynamic display list. TRI/QUAD draws
                                             * are suppressed until another valid G_VTX establishes
                                             * a safe vertex stream.
                                             */
                                            x360_splitgfx_vertex_stream_valid = false;
                                            if (x360_logging_enabled() && x360_splitgfx_logs < 8U) {
                                                char message[320];
                                                _snprintf(message,sizeof(message)-1,
                                                          "MK64: SPLITGFX1 SKIP-BAD-VTX root=%08X cmd=%08X "
                                                          "w0=%08X raw=%08X n=%u dst=%u\n",
                                                          (unsigned)(uintptr_t)dl_root,
                                                          (unsigned)(uintptr_t)cmd,
                                                          (unsigned)cmd->words.w0,
                                                          (unsigned)cmd->words.w1,
                                                          (unsigned)n_vertices,
                                                          (unsigned)dest_index);
                                                message[sizeof(message)-1]=0; x360_log(message);
                                                ++x360_splitgfx_logs;
                                            }
                                        }
                }
#else
                gfx_sp_vertex((C0(0, 16)) / sizeof(Vtx), C0(16, 4), (const Vtx *)seg_addr(cmd->words.w1));
#endif
                break;
            case G_DL:
                {
                    const uint32_t raw_target = cmd->words.w1;
                    Gfx *resolved_target = (Gfx *)seg_addr(raw_target);
                    if (C0(16, 1) == 0) {
                        gfx_run_dl(resolved_target);
                    } else {
                        if (x360_is_quarantined_dl(resolved_target)) return;
                        cmd = resolved_target;
                        --cmd; // increase after break
                    }
                }
                break;
            case (uint8_t)G_ENDDL:
                return;
#ifdef F3DEX_GBI_2
            case G_GEOMETRYMODE:
                gfx_sp_geometry_mode(~C0(0, 24), cmd->words.w1);
                break;
#else
            case (uint8_t)G_SETGEOMETRYMODE:
                gfx_sp_geometry_mode(0, cmd->words.w1);
                break;
            case (uint8_t)G_CLEARGEOMETRYMODE:
                gfx_sp_geometry_mode(cmd->words.w1, 0);
                break;
#endif
            case (uint8_t)G_TRI1: {
                x360_last_tri_w0 = cmd->words.w0;
                x360_last_tri_w1 = cmd->words.w1;
                unsigned a, b, c;
#ifdef F3DEX_GBI_2
                a=C0(16,8)/2; b=C0(8,8)/2; c=C0(0,8)/2;
#elif defined(F3DEX_GBI) || defined(F3DLP_GBI)
                a=C1(16,8)/2; b=C1(8,8)/2; c=C1(0,8)/2;
#else
                a=C1(16,8)/10; b=C1(8,8)/10; c=C1(0,8)/10;
#endif
                if (a >= MAX_VERTICES || b >= MAX_VERTICES || c >= MAX_VERTICES) {
                    x360_b17g12_log_bad_geom(dl_root, cmd, "TRI1-RUNTIME", a,b,c,0,0,0);
                    buf_vbo_len = buf_entry_len;
                    buf_vbo_num_tris = buf_entry_tris;
                    rdp = rdp_entry;
                    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
                    rdp.viewport_or_scissor_changed = true;
                    if (x360_logging_enabled() && x360_splitgfx3_bad_tri_logs < 16) {
                        char message[256];
                        _snprintf(message, sizeof(message)-1,
                                  "MK64: SPLITGFX3 ABORT-BAD-TRI1 root=%08X cmd=%08X w0=%08X w1=%08X idx=%u,%u,%u no-quarantine=1\n",
                                  (unsigned)(uintptr_t)dl_root,
                                  (unsigned)(uintptr_t)cmd,
                                  (unsigned)cmd->words.w0,
                                  (unsigned)cmd->words.w1,
                                  a,b,c);
                        message[sizeof(message)-1]=0;
                        x360_log(message);
                        ++x360_splitgfx3_bad_tri_logs;
                    }
                    return;
                }
                if (x360_splitgfx_vertex_stream_valid) {
                    gfx_sp_tri1((uint8_t)a, (uint8_t)b, (uint8_t)c);
                }
                break;
            }
#if defined(F3DEX_GBI) || defined(F3DLP_GBI)
            case (uint8_t)G_TRI2: {
                x360_last_tri_w0 = cmd->words.w0;
                x360_last_tri_w1 = cmd->words.w1;
                const unsigned a=C0(16,8)/2, b=C0(8,8)/2, c=C0(0,8)/2;
                const unsigned d=C1(16,8)/2, e=C1(8,8)/2, f=C1(0,8)/2;
                if (a>=MAX_VERTICES || b>=MAX_VERTICES || c>=MAX_VERTICES ||
                    d>=MAX_VERTICES || e>=MAX_VERTICES || f>=MAX_VERTICES) {
                    x360_b17g12_log_bad_geom(dl_root, cmd, "TRI2-RUNTIME", a,b,c,d,e,f);
                    buf_vbo_len = buf_entry_len;
                    buf_vbo_num_tris = buf_entry_tris;
                    rdp = rdp_entry;
                    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
                    rdp.viewport_or_scissor_changed = true;
                    if (x360_logging_enabled() && x360_splitgfx3_bad_tri_logs < 16) {
                        char message[320];
                        _snprintf(message, sizeof(message)-1,
                                  "MK64: SPLITGFX3 ABORT-BAD-TRI2 root=%08X cmd=%08X w0=%08X w1=%08X idx=%u,%u,%u/%u,%u,%u no-quarantine=1\n",
                                  (unsigned)(uintptr_t)dl_root,
                                  (unsigned)(uintptr_t)cmd,
                                  (unsigned)cmd->words.w0,
                                  (unsigned)cmd->words.w1,
                                  a,b,c,d,e,f);
                        message[sizeof(message)-1]=0;
                        x360_log(message);
                        ++x360_splitgfx3_bad_tri_logs;
                    }
                    return;
                }
                if (x360_splitgfx_vertex_stream_valid) {
                    gfx_sp_tri1((uint8_t)a, (uint8_t)b, (uint8_t)c);
                    gfx_sp_tri1((uint8_t)d, (uint8_t)e, (uint8_t)f);
                }
                break;
            }
#ifdef F3D_OLD
            case (uint8_t)G_QUAD: {
                /* Early MK64 Fast3D/F3DEX encodes startup-logo quads as
                 * v3:v0:v1:v2 in w1, with vertex indices multiplied by 2. */
                const unsigned v3 = C1(24, 8) / 2;
                const unsigned v0 = C1(16, 8) / 2;
                const unsigned v1 = C1(8, 8) / 2;
                const unsigned v2 = C1(0, 8) / 2;
                x360_last_tri_w0 = cmd->words.w0;
                x360_last_tri_w1 = cmd->words.w1;
                if (x360_splitgfx_vertex_stream_valid) {
                    gfx_sp_tri1(v0, v1, v2);
                    gfx_sp_tri1(v0, v2, v3);
                }
                static unsigned x360_b17g3_quad_logs = 0;
                if (x360_b17g3_quad_logs++ < 8) {
                    char message[160];
                    _snprintf(message,sizeof(message)-1,
                              "MK64: B17G3 G_QUAD v=%u,%u,%u,%u w1=%08X\n",
                              v0,v1,v2,v3,(unsigned)cmd->words.w1);
                    message[sizeof(message)-1]=0; x360_log(message);
                }
                break;
            }
#endif
#endif
            case (uint8_t)G_SETOTHERMODE_L:
#ifdef F3DEX_GBI_2
                gfx_sp_set_other_mode(31 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, cmd->words.w1);
#else
                gfx_sp_set_other_mode(C0(8, 8), C0(0, 8), cmd->words.w1);
#endif
                break;
            case (uint8_t)G_SETOTHERMODE_H:
#ifdef F3DEX_GBI_2
                gfx_sp_set_other_mode(63 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, (uint64_t) cmd->words.w1 << 32);
#else
                gfx_sp_set_other_mode(C0(8, 8) + 32, C0(0, 8), (uint64_t) cmd->words.w1 << 32);
#endif
                break;

            // RDP Commands:
            case G_SETTIMG:
                gfx_dp_set_texture_image(C0(21, 3), C0(19, 2), C0(0, 12), seg_addr(cmd->words.w1));
                break;
            case G_LOADBLOCK:
                gfx_dp_load_block(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_LOADTILE:
                gfx_dp_load_tile(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETTILE:
                gfx_dp_set_tile(C0(21, 3), C0(19, 2), C0(9, 9), C0(0, 9), C1(24, 3), C1(20, 4), C1(18, 2), C1(14, 4), C1(10, 4), C1(8, 2), C1(4, 4), C1(0, 4));
                break;
            case G_SETTILESIZE:
                gfx_dp_set_tile_size(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_LOADTLUT:
                gfx_dp_load_tlut(C1(24, 3), C1(14, 10));
                break;
            case G_SETENVCOLOR:
                gfx_dp_set_env_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETPRIMCOLOR:
                rdp.prim_lod_fraction=C0(0,8);
                gfx_dp_set_prim_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFOGCOLOR:
                gfx_dp_set_fog_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFILLCOLOR:
                gfx_dp_set_fill_color(cmd->words.w1);
                break;
            case G_SETCOMBINE:
                rdp.combine_mode=(uint64_t)(color_comb(C0(20,4),C1(28,4),C0(15,5),C1(15,3))|
                    (alpha_comb(C0(12,3),C1(12,3),C0(9,3),C1(9,3))<<16))|
                    ((uint64_t)(color_comb(C0(5,4),C1(24,4),C0(0,5),C1(6,3))|
                    (alpha_comb(C1(21,3),C1(3,3),C1(18,3),C1(0,3))<<16))<<32);
                break;
            // G_SETPRIMCOLOR, G_CCMUX_PRIMITIVE, G_ACMUX_PRIMITIVE, is used by Goddard
            // G_CCMUX_TEXEL1, LOD_FRACTION is used in Bowser room 1
            case G_TEXRECT:
            case G_TEXRECTFLIP:
            {
                int32_t lrx, lry, tile, ulx, uly;
                uint32_t uls, ult, dsdx, dtdy;
#ifdef F3DEX_GBI_2E
                lrx = (int32_t)(C0(0, 24) << 8) >> 8;
                lry = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                ulx = (int32_t)(C0(0, 24) << 8) >> 8;
                uly = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                uls = C0(16, 16);
                ult = C0(0, 16);
                dsdx = C1(16, 16);
                dtdy = C1(0, 16);
#else
                lrx = C0(12, 12);
                lry = C0(0, 12);
                tile = C1(24, 3);
                ulx = C1(12, 12);
                uly = C1(0, 12);
                ++cmd;
                uls = C1(16, 16);
                ult = C1(0, 16);
                ++cmd;
                dsdx = C1(16, 16);
                dtdy = C1(0, 16);
#endif
                gfx_dp_texture_rectangle(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, opcode == G_TEXRECTFLIP);
                break;
            }
            case G_FILLRECT:
#ifdef F3DEX_GBI_2E
            {
                int32_t lrx, lry, ulx, uly;
                lrx = (int32_t)(C0(0, 24) << 8) >> 8;
                lry = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                ulx = (int32_t)(C0(0, 24) << 8) >> 8;
                uly = (int32_t)(C1(0, 24) << 8) >> 8;
                gfx_dp_fill_rectangle(ulx, uly, lrx, lry);
                break;
            }
#else
                gfx_dp_fill_rectangle(C1(12, 12), C1(0, 12), C0(12, 12), C0(0, 12));
                break;
#endif
            case G_SETSCISSOR:
                gfx_dp_set_scissor(C1(24, 2), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETZIMG:
                gfx_dp_set_z_image(seg_addr(cmd->words.w1));
                break;
            case G_SETCIMG:
                gfx_dp_set_color_image(C0(21, 3), C0(19, 2), C0(0, 11), seg_addr(cmd->words.w1));
                break;
        }
        ++cmd;
    }
}

static void gfx_sp_reset() {
    rsp.modelview_matrix_stack_size = 1;
    rsp.current_num_lights = 2;
    rsp.lights_changed = true;
}

void gfx_get_dimensions(uint32_t *width, uint32_t *height) {
    gfx_wapi->get_dimensions(width, height);
}

void gfx_init(struct GfxWindowManagerAPI *wapi, struct GfxRenderingAPI *rapi, const char *game_name, bool start_in_fullscreen) {
    gfx_wapi = wapi;
    gfx_rapi = rapi;
    gfx_wapi->init(game_name, start_in_fullscreen);
    gfx_wapi->get_dimensions(&gfx_current_dimensions.width, &gfx_current_dimensions.height);
    gfx_rapi->init();

    memset(rsp.lookat, 0, sizeof(rsp.lookat));
    rsp.lookat[0].dir[0] = 127;
    rsp.lookat[1].dir[1] = 127;
    rsp.lookat_enabled = true;
    rsp.lights_changed = true;

    // Used in the 120 star TAS
    static uint32_t precomp_shaders[] = {
        0x01200200,
        0x00000045,
        0x00000200,
        0x01200a00,
        0x00000a00,
        0x01a00045,
        0x00000551,
        0x01045045,
        0x05a00a00,
        0x01200045,
        0x05045045,
        0x01045a00,
        0x01a00a00,
        0x0000038d,
        0x01081081,
        0x0120038d,
        0x03200045,
        0x03200a00,
        0x01a00a6f,
        0x01141045,
        0x07a00a00,
        0x05200200,
        0x03200200,
        0x09200200,
        0x0920038d,
        0x09200045
    };
    for (size_t i = 0; i < sizeof(precomp_shaders) / sizeof(uint32_t); i++) {
        gfx_lookup_or_create_shader_program(precomp_shaders[i]);
    }
}

void gfx_shutdown(void) {
    if (gfx_rapi && gfx_rapi->shutdown) gfx_rapi->shutdown();
    if (gfx_wapi && gfx_wapi->shutdown) gfx_wapi->shutdown();
    gfx_rapi = NULL;
    gfx_wapi = NULL;
}

struct GfxRenderingAPI *gfx_get_current_rendering_api(void) {
    return gfx_rapi;
}

void gfx_start_frame(void) {
    x360_async_drain();
    x360_preload_step();
    /* B17G13 RACE-TIMER-CORRELATION: sparse timeline keyed to the exact on-screen race timer.
     * At most four lines per GAME second. */
    {
        static int last_q = -1;
        if (x360_logging_enabled() && gGamestate == 4 && gCourseTimer >= 0.0f && gCourseTimer < 120.0f) {
            const int q = (int)(gCourseTimer * 4.0f);
            if (q != last_q) {
                const unsigned t100 = (unsigned)(gCourseTimer * 100.0f);
                const struct TextureHashmapNode *n0 = rendering_state.textures[0];
                const struct TextureHashmapNode *n1 = rendering_state.textures[1];
                char m[640];
                _snprintf(m, sizeof(m) - 1,
                    "MK64: B17G13 TIME t100=%u min=%u sec=%u hund=%u "
                    "gen0=%u src0=%08X bytes0=%u hash0=%08X dxt0=%u "
                    "gen1=%u src1=%08X bytes1=%u hash1=%08X dxt1=%u "
                    "node0=%u/%08X/%08X node1=%u/%08X/%08X "
                    "pgen=%u lastV=%08X/%08X lastT=%08X/%08X quarantine=%u\n",
                    t100, t100 / 6000u, (t100 / 100u) % 60u, t100 % 100u,
                    (unsigned)rdp.loaded_texture[0].generation,
                    (unsigned)(uintptr_t)rdp.loaded_texture[0].addr,
                    (unsigned)rdp.loaded_texture[0].size_bytes,
                    (unsigned)rdp.loaded_texture[0].load_capture_hash,
                    (unsigned)rdp.loaded_texture[0].load_dxt,
                    (unsigned)rdp.loaded_texture[1].generation,
                    (unsigned)(uintptr_t)rdp.loaded_texture[1].addr,
                    (unsigned)rdp.loaded_texture[1].size_bytes,
                    (unsigned)rdp.loaded_texture[1].load_capture_hash,
                    (unsigned)rdp.loaded_texture[1].load_dxt,
                    (unsigned)(n0 ? n0->texture_id : 0),
                    (unsigned)(n0 ? n0->content_hash : 0),
                    (unsigned)(n0 ? n0->palette_hash : 0),
                    (unsigned)(n1 ? n1->texture_id : 0),
                    (unsigned)(n1 ? n1->content_hash : 0),
                    (unsigned)(n1 ? n1->palette_hash : 0),
                    (unsigned)rdp.palette_generation,
                    (unsigned)x360_last_vtx_w0, (unsigned)x360_last_vtx_w1,
                    (unsigned)x360_last_tri_w0, (unsigned)x360_last_tri_w1,
                    (unsigned)x360_bad_dl_root_count);
                m[sizeof(m) - 1] = 0;
                x360_log(m);
                last_q = q;
            }
        } else {
            last_q = -1;
        }
    }
    gfx_wapi->handle_events();
    gfx_wapi->get_dimensions(&gfx_current_dimensions.width, &gfx_current_dimensions.height);
    if (gfx_current_dimensions.height == 0) {
        // Avoid division by zero
        gfx_current_dimensions.height = 1;
    }
    ratio_x = (float)gfx_current_dimensions.width / (float)SCREEN_WIDTH;
    ratio_y = (float)gfx_current_dimensions.height / (float)SCREEN_HEIGHT;
    inv_ratio_x = (float)SCREEN_WIDTH / (float)gfx_current_dimensions.width;
    inv_ratio_y = (float)SCREEN_HEIGHT / (float)gfx_current_dimensions.height;
    gfx_current_dimensions.aspect_ratio = (float)gfx_current_dimensions.width / (float)gfx_current_dimensions.height;
}

void gfx_run(Gfx *commands) {
    static bool x360_b17g3_logged = false;
    if (!x360_b17g3_logged) {
        x360_log("MK64: B17G3 old-G_QUAD + lookat + content quarantine + invalid-texture isolation\n");
        x360_log("MK64: B18 graphics fixes: sampler slots, palette banks, cache isolation, crop state, UV shifts\n");
        x360_log("MK64: B17G4 preflight poison containment + cache rollover reset\n");
        x360_b17g3_logged = true;
    }
    static bool x360_b17g2_logged = false;
    if (!x360_b17g2_logged) {
        x360_log("MK64: B17G2 texture-content refresh + safe crop clamp\n");
    x360_log("MK64: B17G8 low-overhead graphics flight recorder; legacy graphics logs throttled\n");
    x360_log("MK64: B17G11 course TMEM bank recorder; render behavior unchanged\n");
    x360_log("MK64: B17G12 geometry guard + corrupt-DL recorder enabled\n");
    x360_log("MK64: B17G13 HUD race-timer correlation enabled\n");
    x360_log("MK64: B17G14C terrain/road timer recorder enabled; kart ignored\n");
    x360_log("MK64: SPLITGFX1 bad VTX skips dependent triangles; dynamic root continues\n");
    x360_log("MK64: SPLITGFX3 bad TRI aborts current DL invocation; no persistent quarantine\n");
        x360_b17g2_logged = true;
    }
    memcpy(rspSegments,gSegmentTable,sizeof(rspSegments));
    gfx_sp_reset();

    if (!x360_b10_marker_logged) {
        x360_log("MK64: B16a render-tile-off + fast-cache + 26800Hz; B12 combiner/B10 quarantine retained\n");
        x360_b10_marker_logged = true;
    }

    //puts("New frame");

    if (!gfx_wapi->start_frame()) {
        dropped_frame = true;
        return;
    }
    dropped_frame = false;

    gfx_rapi->start_frame();
    x360_gfx_online_hud = 0;
    gfx_run_dl(commands);
    gfx_flush();
    x360_gfx_online_hud = 0;
    gfx_rapi->end_frame();
    gfx_wapi->swap_buffers_begin();
}

void gfx_end_frame(void) {
#if X360_HDTEX_STATS
    {
        LARGE_INTEGER agora;
        QueryPerformanceCounter(&agora);
        if (!x360_st_freq.QuadPart) QueryPerformanceFrequency(&x360_st_freq);
        if (x360_st_last_frame.QuadPart) {
            uint32_t us = (uint32_t)((agora.QuadPart - x360_st_last_frame.QuadPart) * 1000000
                                     / x360_st_freq.QuadPart);
            if (us > x360_st_fmax_us) x360_st_fmax_us = us;
            if (us > 16700) ++x360_st_f17;
            if (us > 33300) ++x360_st_f34;
        }
        x360_st_last_frame = agora;
    }
    if (++x360_st_frames >= 60) {
        char line[512];   /* era 200: a linha cresceu com os campos novos e, ao
                             nao caber, _snprintf devolvia -1 e nada era gravado */
        int n = _snprintf(line, sizeof(line) - 1,
            "pak=%u reads=%u rahit=%u ramhit=%u evict=%u uploads=%u uploadKB=%u ramMB=%u "
            "diskMS=%u decMS=%u uploadMS=%u frameMaxMS=%u acima17ms=%u acima33ms=%u "
            "estado=%d pre=%u preTotal=%u preMB=%u preJa=%u preFim=%u grupo=%u/%u\r\n",
            x360_st_pak, x360_st_reads, x360_st_rahit, x360_st_ramhit, x360_st_evict,
            x360_st_up, x360_st_upkb, (unsigned)(x360_hdram_bytes >> 20),
            x360_st_disk_us / 1000, x360_st_dec_us / 1000, x360_st_up_us / 1000,
            x360_st_fmax_us / 1000, x360_st_f17, x360_st_f34,
            gGamestate, x360_st_pre, x360_pre_total, x360_pre_kb >> 10, x360_pre_ja,
            x360_pre_done ? 1u : 0u, x360_pre_pos, x360_pak_count);
        HANDLE f = CreateFileA("game:\\hdtex-stats.log", GENERIC_WRITE, FILE_SHARE_READ,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            SetFilePointer(f, 0, NULL, FILE_END);
            if (n < 0) {   /* nao coube: grava o que couber, terminado em quebra */
                n = (int)sizeof(line) - 3;
                line[n] = '\r'; line[n + 1] = '\n'; n += 2;
            }
            if (n > 0) WriteFile(f, line, (DWORD)n, &w, NULL);
            CloseHandle(f);
        }
        x360_st_pak = x360_st_ramhit = x360_st_evict = x360_st_up = x360_st_upkb = 0;
        x360_st_reads = x360_st_rahit = x360_st_disk_us = x360_st_dec_us = x360_st_up_us = 0;
        x360_st_fmax_us = x360_st_f17 = x360_st_f34 = 0;
        x360_st_pre = 0;
        x360_st_frames = 0;
    }
#endif
    if (!dropped_frame) {
        gfx_rapi->finish_render();
        gfx_wapi->swap_buffers_end();
    }
}
