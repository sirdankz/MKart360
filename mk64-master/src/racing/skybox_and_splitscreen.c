#include "xbox360/race8.h"
#include <ultra64.h>
#include <macros.h>
#include <PR/gbi.h>
#include <mk64.h>
#include <course.h>

#include "skybox_and_splitscreen.h"
#ifdef XBOX360_PORT
#include "xbox360/netplay.h"
#include "xbox360/platform.h"
#include <xtl.h>
#include <stdio.h>
#include <string.h>
#endif
#include "code_800029B0.h"
#include <common_structs.h>
#include "memory.h"
#include "camera.h"
#include <assets/common_data.h>
#include "render_player.h"
#include "code_80057C60.h"
#include "menu_items.h"
#include "actors.h"
#include "render_courses.h"
#include "math_util.h"
#include "main.h"
#include "menus.h"


/* MK64 R37 render-only 1P camera framing.
 *
 * R36.1 hardware logs proved the remaining fullscreen mismatch is NOT the
 * projection matrix.  Offline 1P uses a ~120.375-unit eye->target distance:
 *      eye offset:    50 behind, 9.5 high
 *      target offset: 70 ahead
 * Horizontal 2P uses only ~65.705 units:
 *      eye offset:    35 behind, 9.6 high
 *      target offset: 30 ahead
 * 3P/4P uses 40 behind / 18 ahead / 9.0 high.
 *
 * Do not touch cameras[] itself.  R28 proved camera state can feed back into
 * deterministic simulation.  This helper remaps only the eye/target values
 * passed to the render look-at matrix for the one local online view.
 */
static int r37_local_render_camera(s32 camId) {
    return (x360_net_active() &&
            gGamestate == RACING &&
            x360_net_local_count() == 1 &&
            gPlayerCountSelection1 > 1 &&
            camId == x360_net_local_slot() &&
            gModeSelection != BATTLE);
}

static void r37_render_eye_at(s32 camId,
                              const f32 eye[3], const f32 at[3],
                              f32 outEye[3], f32 outAt[3]) {
    f32 behind, ahead, eyeY;
    f32 t;
    f32 anchorX, anchorZ;

    outEye[0] = eye[0];
    outEye[1] = eye[1];
    outEye[2] = eye[2];
    outAt[0] = at[0];
    outAt[1] = at[1];
    outAt[2] = at[2];

    if (!r37_local_render_camera(camId)) {
        return;
    }

    switch (gActiveScreenMode) {
        case SCREEN_MODE_2P_SPLITSCREEN_HORIZONTAL:
            behind = 35.0f;
            ahead = 30.0f;
            eyeY = 9.6f;
            break;
        case SCREEN_MODE_3P_4P_SPLITSCREEN:
            behind = 40.0f;
            ahead = 18.0f;
            eyeY = 9.0f;
            break;
        case SCREEN_MODE_2P_SPLITSCREEN_VERTICAL:
            return;
        default:
            return;
    }

    t = behind / (behind + ahead);
    anchorX = eye[0] + ((at[0] - eye[0]) * t);
    anchorZ = eye[2] + ((at[2] - eye[2]) * t);

    outEye[0] = anchorX + ((eye[0] - anchorX) * (50.0f / behind));
    outEye[2] = anchorZ + ((eye[2] - anchorZ) * (50.0f / behind));
    outAt[0] = anchorX + ((at[0] - anchorX) * (70.0f / ahead));
    outAt[2] = anchorZ + ((at[2] - anchorZ) * (70.0f / ahead));

    outAt[1] = at[1];
    outEye[1] = at[1] + ((eye[1] - at[1]) * (9.5f / eyeY));
}

static void r37_guLookAt(Mtx *mtx,
                         f32 xEye, f32 yEye, f32 zEye,
                         f32 xAt, f32 yAt, f32 zAt,
                         f32 xUp, f32 yUp, f32 zUp) {
    Vec3f eye;
    Vec3f at;
    Vec3f renderEye;
    Vec3f renderAt;
    s32 camId = -1;
    s32 i;

    eye[0] = xEye; eye[1] = yEye; eye[2] = zEye;
    at[0] = xAt; at[1] = yAt; at[2] = zAt;

    if (gGfxPool) {
        for (i = 0; i < 4; ++i) {
            if (mtx == &gGfxPool->mtxLookAt[i]) {
                camId = i;
                break;
            }
        }
    }

    r37_render_eye_at(camId, eye, at, renderEye, renderAt);
    guLookAt(mtx,
             renderEye[0], renderEye[1], renderEye[2],
             renderAt[0], renderAt[1], renderAt[2],
             xUp, yUp, zUp);
}


/* MK64 R36.1 Xbox 360 offline-vs-online view reference logger.
 *
 * Diagnostic only. The wrapper calls the original guPerspective first and
 * records the resulting projection/camera/view state. It does not alter any
 * projection, camera, gameplay, network, RNG, or hash value.
 *
 * Files:
 *   game:\mk64-view-360-offline.log
 *   game:\mk64-view-360-online.log
 */
extern int x360_net_diagnostics_enabled(void);
static u32 r361_f32_bits(f32 v) {
    union { f32 f; u32 u; } x;
    x.f = v;
    return x.u;
}

static HANDLE r361_view_file(int online) {
    static HANDLE off = INVALID_HANDLE_VALUE;
    static HANDLE on = INVALID_HANDLE_VALUE;
    static int offTried = 0;
    static int onTried = 0;
    HANDLE *slot = online ? &on : &off;
    int *tried = online ? &onTried : &offTried;
    const char *path = online ? "game:\\mk64-view-360-online.log"
                              : "game:\\mk64-view-360-offline.log";
    const char *fallback = online ? "mk64-view-360-online.log"
                                  : "mk64-view-360-offline.log";

    if (*slot != INVALID_HANDLE_VALUE) return *slot;
    if (*tried) return INVALID_HANDLE_VALUE;
    *tried = 1;

    *slot = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (*slot == INVALID_HANDLE_VALUE) {
        *slot = CreateFileA(fallback, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    }

    if (*slot != INVALID_HANDLE_VALUE) {
        char hdr[256];
        DWORD wrote = 0;
        int n = sprintf(hdr,
            "R36_VIEW_HEADER mode=%s side=360 build=R36.1-360-VIEW-REFERENCE "
            "sampleEvery=30 maxSamples=90\r\n",
            online ? "ONLINE" : "OFFLINE");
        if (n > 0) WriteFile(*slot, hdr, (DWORD)n, &wrote, NULL);
        FlushFileBuffers(*slot);
    }
    return *slot;
}

static void r361_write_line(HANDLE h, const char *line) {
    DWORD wrote = 0;
    DWORD n;
    const char *p;
    if (h == INVALID_HANDLE_VALUE || !line) return;
    p = line;
    n = (DWORD)strlen(line);
    if (n) WriteFile(h, p, n, &wrote, NULL);
    FlushFileBuffers(h);
}

static void r361_crop_info(int online, int mode, int players, int slot,
                           int *cropX, int *cropY, int *cropW, int *cropH,
                           int *scaleX1000, int *scaleY1000) {
    *cropX = 0; *cropY = 0; *cropW = 640; *cropH = 480;
    *scaleX1000 = 1000; *scaleY1000 = 1000;
    if (!online || players <= 1) return;

    if (mode == SCREEN_MODE_2P_SPLITSCREEN_HORIZONTAL && players == 2) {
        *cropW = 640; *cropH = 240;
        *cropY = slot * 240;
    } else if (mode == SCREEN_MODE_2P_SPLITSCREEN_VERTICAL && players == 2) {
        *cropW = 320; *cropH = 480;
        *cropX = slot * 320;
    } else if (mode == SCREEN_MODE_3P_4P_SPLITSCREEN && players >= 3) {
        *cropW = 320; *cropH = 240;
        *cropX = (slot & 1) * 320;
        *cropY = (slot >> 1) * 240;
    }

    if (*cropW > 0) *scaleX1000 = 640000 / *cropW;
    if (*cropH > 0) *scaleY1000 = 480000 / *cropH;
}

static void r361_log_projection(Mtx *mtx, f32 fovy, f32 aspect,
                                f32 nearp, f32 farp, f32 scale) {
    static u32 offlineCalls = 0, onlineCalls = 0;
    static u32 offlineSamples = 0, onlineSamples = 0;
    int online = x360_net_active() ? 1 : 0;
    u32 *calls = online ? &onlineCalls : &offlineCalls;
    u32 *samples = online ? &onlineSamples : &offlineSamples;
    int camId;
    int localSlot;
    int localCount;
    int players;
    int cropX, cropY, cropW, cropH, sx1000, sy1000;
    struct UnkStruct_800DC5EC *view;
    Camera *cam;
    HANDLE f;
    const u32 *mw;
    char line[4096];
    int n, i;

    if (!x360_net_diagnostics_enabled()) return;

    if (gGamestate != RACING || !gGfxPool) return;

    camId = (int)(mtx - &gGfxPool->mtxPersp[0]);
    if (camId < 0 || camId > 3) return;

    localSlot = online ? x360_net_local_slot() : 0;
    localCount = online ? x360_net_local_count() : 1;
    players = online ? x360_net_player_count() : 1;

    if (online) {
        if (localCount != 1) return;
        if (camId != localSlot) return;
    } else {
        if (gActiveScreenMode != SCREEN_MODE_1P ||
            gPlayerCountSelection1 != 1 ||
            camId != 0) {
            return;
        }
    }

    (*calls)++;
    if (((*calls) - 1u) % 30u != 0u) return;
    if (*samples >= 90u) return;
    (*samples)++;

    f = r361_view_file(online);
    if (f == INVALID_HANDLE_VALUE) return;

    view = &D_8015F480[camId];
    cam = &cameras[camId];

    r361_crop_info(online, gActiveScreenMode, players, localSlot,
                   &cropX, &cropY, &cropW, &cropH, &sx1000, &sy1000);

    n = sprintf(line,
        "VIEW n=%u mode=%s side=360 cam=%d local=%d localCount=%d "
        "activeMode=%d selectedMode=%d players=%d "
        "zoom=%08X aspectGlobal=%08X aspectRender=%08X near=%08X far=%08X scale=%08X "
        "camFov=%08X displayAspect=%08X displayWide=%d video=%u,%u "
        "pos=%08X,%08X,%08X look=%08X,%08X,%08X up=%08X,%08X,%08X "
        "rot=%d,%d,%d "
        "vpRaw=%d,%d,%d,%d "
        "crop=%d,%d,%d,%d cropScale1000=%d,%d ",
        (unsigned)*samples,
        online ? "ONLINE" : "OFFLINE",
        camId, localSlot, localCount,
        (int)gActiveScreenMode, (int)gScreenModeSelection, players,
        (unsigned)r361_f32_bits(fovy),
        (unsigned)r361_f32_bits(gScreenAspect),
        (unsigned)r361_f32_bits(aspect),
        (unsigned)r361_f32_bits(nearp),
        (unsigned)r361_f32_bits(farp),
        (unsigned)r361_f32_bits(scale),
        (unsigned)r361_f32_bits(cam->unk_B4),
        (unsigned)r361_f32_bits(x360_display_aspect()),
        x360_display_widescreen(),
        (unsigned)x360_video_width(), (unsigned)x360_video_height(),
        (unsigned)r361_f32_bits(cam->pos[0]),
        (unsigned)r361_f32_bits(cam->pos[1]),
        (unsigned)r361_f32_bits(cam->pos[2]),
        (unsigned)r361_f32_bits(cam->lookAt[0]),
        (unsigned)r361_f32_bits(cam->lookAt[1]),
        (unsigned)r361_f32_bits(cam->lookAt[2]),
        (unsigned)r361_f32_bits(cam->up[0]),
        (unsigned)r361_f32_bits(cam->up[1]),
        (unsigned)r361_f32_bits(cam->up[2]),
        (int)cam->rot[0], (int)cam->rot[1], (int)cam->rot[2],
        (int)view->screenStartX, (int)view->screenStartY,
        (int)view->screenWidth, (int)view->screenHeight,
        cropX, cropY, cropW, cropH, sx1000, sy1000);

    if (n < 0 || n >= (int)sizeof(line) - 200) return;

    mw = (const u32 *)mtx;
    n += sprintf(line + n, "mtx=");
    for (i = 0; i < 16 && n < (int)sizeof(line) - 32; ++i) {
        n += sprintf(line + n, "%08X%s",
                     (unsigned)mw[i], (i == 15) ? "" : ",");
    }
    n += sprintf(line + n, "\r\n");
    r361_write_line(f, line);
}

static void r361_guPerspective(Mtx *mtx, u16 *perspNorm, f32 fovy, f32 aspect,
                               f32 nearp, f32 farp, f32 scale) {
    guPerspective(mtx, perspNorm, fovy, aspect, nearp, farp, scale);
    r361_log_projection(mtx, fovy, aspect, nearp, farp, scale);
}


Vp D_802B8880[] = {
    { { { 640, 480, 511, 0 }, { 640, 480, 511, 0 } } },
};

static Vtx sSkyboxP1[] = {
    { { { SCREEN_WIDTH, SCREEN_HEIGHT, -1 }, 0, { 0, 0 }, { 0xC8, 0xC8, 0xFF, 0xFF } } },
    { { { SCREEN_WIDTH, 120, -1 }, 0, { 0, 0 }, { 0x1E, 0x1E, 0xFF, 0xFF } } },
    { { { 0, 120, -1 }, 0, { 0, 0 }, { 0x1E, 0x1E, 0xFF, 0xFF } } },
    { { { 0, SCREEN_HEIGHT, -1 }, 0, { 0, 0 }, { 0xC8, 0xC8, 0xFF, 0xFF } } },
    { { { SCREEN_WIDTH, 120, -1 }, 0, { 0, 0 }, { 0x00, 0xDC, 0x00, 0xFF } } },
    { { { SCREEN_WIDTH, 0, -1 }, 0, { 0, 0 }, { 0x78, 0xFF, 0x78, 0xFF } } },
    { { { 0, 0, -1 }, 0, { 0, 0 }, { 0x78, 0xFF, 0x78, 0xFF } } },
    { { { 0, 120, -1 }, 0, { 0, 0 }, { 0x00, 0xDC, 0x00, 0xFF } } },
};

static Vtx sSkyboxP2[] = {
    { { { SCREEN_WIDTH, SCREEN_HEIGHT, -1 }, 0, { 0, 0 }, { 0xC8, 0xC8, 0xFF, 0xFF } } },
    { { { SCREEN_WIDTH, 120, -1 }, 0, { 0, 0 }, { 0x1E, 0x1E, 0xFF, 0xFF } } },
    { { { 0, 120, -1 }, 0, { 0, 0 }, { 0x1E, 0x1E, 0xFF, 0xFF } } },
    { { { 0, SCREEN_HEIGHT, -1 }, 0, { 0, 0 }, { 0xC8, 0xC8, 0xFF, 0xFF } } },
    { { { SCREEN_WIDTH, 120, -1 }, 0, { 0, 0 }, { 0x00, 0xDC, 0x00, 0xFF } } },
    { { { SCREEN_WIDTH, 0, -1 }, 0, { 0, 0 }, { 0x78, 0xFF, 0x78, 0xFF } } },
    { { { 0, 0, -1 }, 0, { 0, 0 }, { 0x78, 0xFF, 0x78, 0xFF } } },
    { { { 0, 120, -1 }, 0, { 0, 0 }, { 0x00, 0xDC, 0x00, 0xFF } } },
};

static Vtx sSkyboxP3[] = {
    { { { SCREEN_WIDTH, SCREEN_HEIGHT, -1 }, 0, { 0, 0 }, { 0xC8, 0xC8, 0xFF, 0xFF } } },
    { { { SCREEN_WIDTH, 120, -1 }, 0, { 0, 0 }, { 0x1E, 0x1E, 0xFF, 0xFF } } },
    { { { 0, 120, -1 }, 0, { 0, 0 }, { 0x1E, 0x1E, 0xFF, 0xFF } } },
    { { { 0, SCREEN_HEIGHT, -1 }, 0, { 0, 0 }, { 0xC8, 0xC8, 0xFF, 0xFF } } },
    { { { SCREEN_WIDTH, 120, -1 }, 0, { 0, 0 }, { 0x00, 0xDC, 0x00, 0xFF } } },
    { { { SCREEN_WIDTH, 0, -1 }, 0, { 0, 0 }, { 0x78, 0xFF, 0x78, 0xFF } } },
    { { { 0, 0, -1 }, 0, { 0, 0 }, { 0x78, 0xFF, 0x78, 0xFF } } },
    { { { 0, 120, -1 }, 0, { 0, 0 }, { 0x00, 0xDC, 0x00, 0xFF } } },
};

static Vtx sSkyboxP4[] = {
    { { { SCREEN_WIDTH, SCREEN_HEIGHT, -1 }, 0, { 0, 0 }, { 0xC8, 0xC8, 0xFF, 0xFF } } },
    { { { SCREEN_WIDTH, 120, -1 }, 0, { 0, 0 }, { 0x1E, 0x1E, 0xFF, 0xFF } } },
    { { { 0, 120, -1 }, 0, { 0, 0 }, { 0x1E, 0x1E, 0xFF, 0xFF } } },
    { { { 0, SCREEN_HEIGHT, -1 }, 0, { 0, 0 }, { 0xC8, 0xC8, 0xFF, 0xFF } } },
    { { { SCREEN_WIDTH, 120, -1 }, 0, { 0, 0 }, { 0x00, 0xDC, 0x00, 0xFF } } },
    { { { SCREEN_WIDTH, 0, -1 }, 0, { 0, 0 }, { 0x78, 0xFF, 0x78, 0xFF } } },
    { { { 0, 0, -1 }, 0, { 0, 0 }, { 0x78, 0xFF, 0x78, 0xFF } } },
    { { { 0, 120, -1 }, 0, { 0, 0 }, { 0x00, 0xDC, 0x00, 0xFF } } },
};

void func_802A3730(struct UnkStruct_800DC5EC* arg0) {
    s32 ulx;
    s32 uly;
    s32 lrx;
    s32 lry;
    s32 screenWidth = arg0->screenWidth * 2;
    s32 screenHeight = arg0->screenHeight * 2;
    s32 screenStartX = arg0->screenStartX * 4;
    s32 screenStartY = arg0->screenStartY * 4;

    arg0->viewport.vp.vscale[0] = screenWidth;
    arg0->viewport.vp.vscale[1] = screenHeight;
    arg0->viewport.vp.vscale[2] = 511;
    arg0->viewport.vp.vscale[3] = 0;

    arg0->viewport.vp.vtrans[0] = screenStartX;
    arg0->viewport.vp.vtrans[1] = screenStartY;
    arg0->viewport.vp.vtrans[2] = 511;
    arg0->viewport.vp.vtrans[3] = 0;

    gSPViewport(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&arg0->viewport));

    screenWidth /= 4;
    screenHeight /= 4;

    screenStartX /= 4;
    screenStartY /= 4;

    lrx = screenStartX + screenWidth;
    if (lrx > SCREEN_WIDTH) {
        lrx = SCREEN_WIDTH;
    }

    lry = screenStartY + screenHeight;
    if (lry > SCREEN_HEIGHT) {
        lry = SCREEN_HEIGHT;
    }
    ulx = 0;
    uly = 0;

    gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, ulx, uly, lrx, lry);
}

UNUSED void func_802A38AC(void) {
}

void func_802A38B4(void) {
    init_rdp();
    select_framebuffer();

    gDPFullSync(gDisplayListHead++);
    gSPEndDisplayList(gDisplayListHead++);

    if (gQuitToMenuTransitionCounter != 0) {
        gQuitToMenuTransitionCounter--;
        return;
    }
    gGamestateNext = gGotoMode;
    gGamestate = 255;
    gIsInQuitToMenuTransition = 0;
    gQuitToMenuTransitionCounter = 0;
    gFadeModeSelection = FADE_MODE_MAIN;

    switch (gGotoMode) {
        case START_MENU_FROM_QUIT:
            if (gMenuSelection != LOGO_INTRO_MENU) {
                gMenuSelection = START_MENU;
            }
            break;
        case MAIN_MENU_FROM_QUIT:
            gMenuSelection = MAIN_MENU;
            break;
        case PLAYER_SELECT_MENU_FROM_QUIT:
            gMenuSelection = CHARACTER_SELECT_MENU;
            break;
        case COURSE_SELECT_MENU_FROM_QUIT:
            gMenuSelection = COURSE_SELECT_MENU;
            break;
    }
}

void func_802A39E0(struct UnkStruct_800DC5EC* arg0) {
    s32 ulx = arg0->screenStartX - (arg0->screenWidth / 2);
    s32 uly = arg0->screenStartY - (arg0->screenHeight / 2);
    s32 lrx = arg0->screenStartX + (arg0->screenWidth / 2);
    s32 lry = arg0->screenStartY + (arg0->screenHeight / 2);

    if (ulx < 0) {
        ulx = 0;
    }
    if (uly < 0) {
        uly = 0;
    }
    if (lrx > SCREEN_WIDTH) {
        lrx = SCREEN_WIDTH;
    }
    if (lry > SCREEN_HEIGHT) {
        lry = SCREEN_HEIGHT;
    }
    if (ulx >= lrx) {
        lrx = ulx + 2;
    }
    if (uly >= lry) {
        lry = uly + 2;
    }

    gDPPipeSync(gDisplayListHead++);
    gDPSetCycleType(gDisplayListHead++, G_CYC_FILL);
    gDPSetDepthImage(gDisplayListHead++, gPhysicalZBuffer);
    gDPSetColorImage(gDisplayListHead++, G_IM_FMT_RGBA, G_IM_SIZ_16b, SCREEN_WIDTH, gPhysicalZBuffer);
    gDPSetFillColor(gDisplayListHead++, 0xFFFCFFFC);
    gDPPipeSync(gDisplayListHead++);
    gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, ulx, uly, lrx, lry);

    gDPFillRectangle(gDisplayListHead++, ulx, uly, lrx - 1, lry - 1);

    gDPPipeSync(gDisplayListHead++);
    gDPSetColorImage(gDisplayListHead++, G_IM_FMT_RGBA, G_IM_SIZ_16b, SCREEN_WIDTH,
                     VIRTUAL_TO_PHYSICAL(gPhysicalFramebuffers[sRenderingFramebuffer])); // 0x1FFFFFFF
    gDPSetCycleType(gDisplayListHead++, G_CYC_1CYCLE);
    gDPSetDepthSource(gDisplayListHead++, G_ZS_PIXEL);
}

/**
 * Initialize the z-buffer for the current frame.
 */
void init_z_buffer(void) {
    gDPPipeSync(gDisplayListHead++);
    gDPSetCycleType(gDisplayListHead++, G_CYC_FILL);
    gDPSetDepthImage(gDisplayListHead++, gPhysicalZBuffer);
    gDPSetColorImage(gDisplayListHead++, G_IM_FMT_RGBA, G_IM_SIZ_16b, SCREEN_WIDTH, gPhysicalZBuffer);
    gDPSetFillColor(gDisplayListHead++, 0xFFFCFFFC);
    gDPPipeSync(gDisplayListHead++);
    gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    gDPFillRectangle(gDisplayListHead++, 0, 0, 319, 239);
    gDPPipeSync(gDisplayListHead++);
    gDPSetColorImage(gDisplayListHead++, G_IM_FMT_RGBA, G_IM_SIZ_16b, SCREEN_WIDTH,
                     VIRTUAL_TO_PHYSICAL(gPhysicalFramebuffers[sRenderingFramebuffer]));
    gDPSetCycleType(gDisplayListHead++, G_CYC_1CYCLE);
    gDPSetDepthSource(gDisplayListHead++, G_ZS_PIXEL);
}

/**
 * Sets the initial RDP (Reality Display Processor) rendering settings.
 **/
void init_rdp(void) {
    gDPPipeSync(gDisplayListHead++);
    gDPPipelineMode(gDisplayListHead++, G_PM_1PRIMITIVE);
    gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    gDPSetCombineLERP(gDisplayListHead++, 0, 0, 0, SHADE, 0, 0, 0, SHADE, 0, 0, 0, SHADE, 0, 0, 0, SHADE);
    gDPSetTextureLOD(gDisplayListHead++, G_TL_TILE);
    gDPSetTextureLUT(gDisplayListHead++, G_TT_NONE);
    gDPSetTextureDetail(gDisplayListHead++, G_TD_CLAMP);
    gDPSetTexturePersp(gDisplayListHead++, G_TP_PERSP);
    gDPSetTextureFilter(gDisplayListHead++, G_TF_BILERP);
    gDPSetTextureConvert(gDisplayListHead++, G_TC_FILT);
    gDPSetCombineKey(gDisplayListHead++, G_CK_NONE);
    gDPSetAlphaCompare(gDisplayListHead++, G_AC_NONE);
    gDPSetRenderMode(gDisplayListHead++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
    gDPSetBlendMask(gDisplayListHead++, 0xFF);
    gDPSetColorDither(gDisplayListHead++, G_CD_DISABLE);
    gDPPipeSync(gDisplayListHead++);
    gSPClipRatio(gDisplayListHead++, FRUSTRATIO_1);
}

UNUSED void func_802A40A4(void) {
}
UNUSED void func_802A40AC(void) {
}
UNUSED void func_802A40B4(void) {
}
UNUSED void func_802A40BC(void) {
}
UNUSED void func_802A40C4(void) {
}
UNUSED void func_802A40CC(void) {
}
UNUSED void func_802A40D4(void) {
}
UNUSED void func_802A40DC(void) {
}

UNUSED s32 set_viewport2(void) {
    gSPViewport(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&D_800DC5EC->viewport));
    gSPClearGeometryMode(gDisplayListHead++, G_CLEAR_ALL_MODES);
    gSPSetGeometryMode(gDisplayListHead++,
                       G_ZBUFFER | G_SHADE | G_CULL_BACK | G_LIGHTING | G_SHADING_SMOOTH | G_CLIPPING);
}

void set_viewport(void) {
    gSPViewport(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&D_802B8880));
    gSPClearGeometryMode(gDisplayListHead++, G_CLEAR_ALL_MODES);
    gSPSetGeometryMode(gDisplayListHead++, G_SHADE | G_CULL_BACK | G_SHADING_SMOOTH);
}

/**
 * Tells the RDP which of the three framebuffers it shall draw to.
 */
void select_framebuffer(void) {
    gDPSetColorImage(gDisplayListHead++, G_IM_FMT_RGBA, G_IM_SIZ_16b, SCREEN_WIDTH,
                     VIRTUAL_TO_PHYSICAL(gPhysicalFramebuffers[sRenderingFramebuffer]));
    gDPSetFillColor(gDisplayListHead++, GPACK_RGBA5551(D_800DC5D0, D_800DC5D4, D_800DC5D8, 1) << 0x10 |
                                            GPACK_RGBA5551(D_800DC5D0, D_800DC5D4, D_800DC5D8, 1));
    gDPPipeSync(gDisplayListHead++);
    gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    gDPFillRectangle(gDisplayListHead++, 0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1);
    gDPPipeSync(gDisplayListHead++);
    gDPSetCycleType(gDisplayListHead++, G_CYC_1CYCLE);
}

void func_802A4300(void) {

    if (gActiveScreenMode == SCREEN_MODE_1P) {
        return;
    }
    if (D_800DC5B0 != 0) {
        return;
    }

    gDPPipeSync(gDisplayListHead++);
    gDPSetCycleType(gDisplayListHead++, G_CYC_FILL);
    gDPSetColorImage(gDisplayListHead++, G_IM_FMT_RGBA, G_IM_SIZ_16b, SCREEN_WIDTH,
                     VIRTUAL_TO_PHYSICAL(gPhysicalFramebuffers[sRenderingFramebuffer]));
    gDPSetFillColor(gDisplayListHead++, 0x00010001);
    gSPViewport(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&D_802B8880));
    gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    gDPPipeSync(gDisplayListHead++);

    switch (gActiveScreenMode) {
        case SCREEN_MODE_2P_SPLITSCREEN_VERTICAL:
            gDPFillRectangle(gDisplayListHead++, 157, 0, 159, 239);
            break;
        case SCREEN_MODE_2P_SPLITSCREEN_HORIZONTAL:
            gDPFillRectangle(gDisplayListHead++, 0, 119, 319, 121);
            break;
        case SCREEN_MODE_3P_4P_SPLITSCREEN:
            gDPFillRectangle(gDisplayListHead++, 157, 0, 159, 239);
            gDPFillRectangle(gDisplayListHead++, 0, 119, 319, 121);
            break;
    }
    gDPPipeSync(gDisplayListHead++);
    gDPSetCycleType(gDisplayListHead++, G_CYC_1CYCLE);
}
/**
 * @note that the second half of the s16 value is truncated (unused). So if you want red, put 255. But the original
 * programmers might have put something like `42,239`, in bytes: b1010010011111111 The extra bits are skipped and the
 * game only reads `11111111` (255)
 */
struct Skybox {
    s16 topRed;
    s16 topGreen;
    s16 topBlue;
    s16 bottomRed;
    s16 bottomGreen;
    s16 bottomBlue;
};

UNUSED Gfx D_802B8A90[] = {
    gsDPPipeSync(),
    gsDPSetRenderMode(G_RM_OPA_SURF, G_RM_OPA_SURF2),
    gsDPSetCycleType(G_CYC_FILL),
    gsDPSetFillColor(0x00000000),
    gsDPFillRectangle(0, 0, 319, 239),
    gsDPPipeSync(),
    gsDPSetCycleType(G_CYC_1CYCLE),
    gsSPEndDisplayList(),
};

struct Skybox sTopSkyBoxColors[] = {
#include "assets/course_metadata/sSkyColors.inc.c"

};

// struct Skybox sTopSkyBoxColors[] = {
//     {128, 4280, 6136, 216, 7144, 32248},
//     {255, 255, 255, 255, 255, 255},
//     {48, 1544, 49528, 0, 0, 0},
//     {0, 0, 0, 0, 0, 0},
//     {113, 70, 255, 255, 184, 99},
//     {28, 11, 90, 0, 99, 164},
//     {48, 1688, 54136, 216, 7144, 32248},
//     {238, 144, 255, 255, 224, 240},
//     {128, 4280, 6136, 216, 7144, 32248},
//     {0, 18, 255, 197, 211, 255},
//     {0, 2, 94, 209, 65, 23},
//     {195, 231, 255, 255, 0xc0, 0},
//     {128, 4280, 6136, 216, 7144, 32248},
//     {0, 0, 0, 0, 0, 0},
//     {20, 30, 56, 40, 60, 110},
//     {128, 4280, 6136, 216, 7144, 32248},
//     {0, 0, 0, 0, 0, 0},
//     {113, 70, 255, 255, 184, 99},
//     {255, 174, 0, 255, 229, 124},
//     {0, 0, 0, 0, 0, 0},
//     {238, 144, 255, 255, 224, 240},
// };

struct Skybox sBottomSkyBoxColors[] = {
#include "assets/course_metadata/sSkyColors2.inc.c"
};

void course_set_skybox_colours(Vtx* skybox) {
    s32 i;

    if (D_800DC5BC != 0) {

        if (D_801625EC < 0) {
            D_801625EC = 0;
        }

        if (D_801625F4 < 0) {
            D_801625F4 = 0;
        }

        if (D_801625F0 < 0) {
            D_801625F0 = 0;
        }

        if (D_801625EC > 255) {
            D_801625EC = 255;
        }

        if (D_801625F4 > 255) {
            D_801625F4 = 255;
        }

        if (D_801625F0 > 255) {
            D_801625F0 = 255;
        }

        for (i = 0; i < 8; i++) {

            skybox[i].v.cn[0] = (s16) D_801625EC;
            skybox[i].v.cn[1] = (s16) D_801625F4;
            skybox[i].v.cn[2] = (s16) D_801625F0;
        }
        return;
    }

#if !ENABLE_CUSTOM_COURSE_ENGINE
    skybox[0].v.cn[0] = sTopSkyBoxColors[gCurrentCourseId].topRed;
    skybox[0].v.cn[1] = sTopSkyBoxColors[gCurrentCourseId].topGreen;
    skybox[0].v.cn[2] = sTopSkyBoxColors[gCurrentCourseId].topBlue;

    skybox[1].v.cn[0] = sTopSkyBoxColors[gCurrentCourseId].bottomRed;
    skybox[1].v.cn[1] = sTopSkyBoxColors[gCurrentCourseId].bottomGreen;
    skybox[1].v.cn[2] = sTopSkyBoxColors[gCurrentCourseId].bottomBlue;

    skybox[2].v.cn[0] = sTopSkyBoxColors[gCurrentCourseId].bottomRed;
    skybox[2].v.cn[1] = sTopSkyBoxColors[gCurrentCourseId].bottomGreen;
    skybox[2].v.cn[2] = sTopSkyBoxColors[gCurrentCourseId].bottomBlue;

    skybox[3].v.cn[0] = sTopSkyBoxColors[gCurrentCourseId].topRed;
    skybox[3].v.cn[1] = sTopSkyBoxColors[gCurrentCourseId].topGreen;
    skybox[3].v.cn[2] = sTopSkyBoxColors[gCurrentCourseId].topBlue;

    skybox[4].v.cn[0] = sBottomSkyBoxColors[gCurrentCourseId].topRed;
    skybox[4].v.cn[1] = sBottomSkyBoxColors[gCurrentCourseId].topGreen;
    skybox[4].v.cn[2] = sBottomSkyBoxColors[gCurrentCourseId].topBlue;

    skybox[5].v.cn[0] = sBottomSkyBoxColors[gCurrentCourseId].bottomRed;
    skybox[5].v.cn[1] = sBottomSkyBoxColors[gCurrentCourseId].bottomGreen;
    skybox[5].v.cn[2] = sBottomSkyBoxColors[gCurrentCourseId].bottomBlue;

    skybox[6].v.cn[0] = sBottomSkyBoxColors[gCurrentCourseId].bottomRed;
    skybox[6].v.cn[1] = sBottomSkyBoxColors[gCurrentCourseId].bottomGreen;
    skybox[6].v.cn[2] = sBottomSkyBoxColors[gCurrentCourseId].bottomBlue;

    skybox[7].v.cn[0] = sBottomSkyBoxColors[gCurrentCourseId].topRed;
    skybox[7].v.cn[1] = sBottomSkyBoxColors[gCurrentCourseId].topGreen;
    skybox[7].v.cn[2] = sBottomSkyBoxColors[gCurrentCourseId].topBlue;
#else

#endif
}

#ifdef XBOX360_PORT
/* B14b: extend only the untextured horizon/ground backdrop. The renderer's
 * existing 4:3-to-16:9 correction maps 0..320 to the middle 75% of each
 * viewport. Extend both edges by ceil(320/6) for the fixed 1280x720 output.
 * Keep horizon Y, colours, cloud coordinates and all projection matrices. */
static void x360_extend_skybox(Vtx* vertices) {
    const s16 margin = (SCREEN_WIDTH + 5) / 6;
    s32 i;
    for (i = 0; i < 8; ++i) {
        vertices[i].v.ob[0] = (i % 4 < 2) ? SCREEN_WIDTH + margin : -margin;
    }
}
#endif

// Almost identical to end of render_skybox
void func_802A487C(Vtx* arg0, UNUSED struct UnkStruct_800DC5EC* arg1, UNUSED s32 arg2, UNUSED s32 arg3,
                   UNUSED f32* arg4) {
#ifdef XBOX360_PORT
    x360_extend_skybox(arg0);
#endif

    init_rdp();
    if (gCurrentCourseId != COURSE_RAINBOW_ROAD) {

        gDPSetRenderMode(gDisplayListHead++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
        gSPClearGeometryMode(gDisplayListHead++, G_ZBUFFER | G_LIGHTING);
        guOrtho(&gGfxPool->mtxScreen, 0.0f, SCREEN_WIDTH, 0.0f, SCREEN_HEIGHT, 0.0f, 5.0f, 1.0f);
        gSPPerspNormalize(gDisplayListHead++, 0xFFFF);
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxScreen),
                  G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&D_0D008E98), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        gSPVertex(gDisplayListHead++, &arg0[4], 4, 0);
        gSP2Triangles(gDisplayListHead++, 0, 3, 1, 0, 1, 3, 2, 0);
    }
}

/**
 * @brief Sets skybox horizon. Some coordinate transformations which can affect game physics and display of player
 * sprite
 * @param skybox player skybox
 * @param arg1 something camera related
 * @param arg2 unused
 * @param arg3 unused
 * @parma arg4 unused
 */
static f32 race_view_aspect(void) {
#ifdef XBOX360_PORT
    /* MK64_RACE8_LOCAL_SPLIT_ASPECT_V3
     * Online simulation stays in the race8 path, but a two-local console
     * presents native horizontal half-height views (320x120 => 8:3). */
    if (x360_net_active() && gGamestate == 4) {
        if (x360_net_local_count() > 1) {
            return 2.66666675f;
        }
        return 1.33333334f;
    }
#endif
    return gScreenAspect;
}

void render_skybox(Vtx* skybox, struct UnkStruct_800DC5EC* arg1, UNUSED s32 arg2, UNUSED s32 arg3, UNUSED f32* arg4) {
    Camera* camera = arg1->camera;
    s16 horizonRow;
    f32 homogFactor;
    UNUSED s32 pad[2];
    UNUSED u16 pad2;
    u16 sp128;
    Mat4 projMtx;
    Mat4 lookAtMtx;
    Mat4 lookAndProjMtx;
    Vec3f horizonPoint;
    f32 homogScale;

    course_set_skybox_colours(skybox);
#ifdef XBOX360_PORT
    x360_extend_skybox(skybox);
#endif

    // horizonPoint is an apparently arbitrary point on the horizon (technically, where y = 0). Used for skybox horizon
    horizonPoint[0] = 0.0f;
    horizonPoint[1] = 0.0f;
    horizonPoint[2] = 30000.0f;
    mtxf_projection(projMtx, &sp128, camera->unk_B4, race_view_aspect(), gCourseNearPersp, gCourseFarPersp, 1.0f);
    {
        Vec3f r37Eye;
        Vec3f r37At;
        r37_render_eye_at((s32)(camera - cameras), camera->pos, camera->lookAt, r37Eye, r37At);
        mtxf_lookat(lookAtMtx, r37Eye, r37At);
    }
    mtxf_multiplication(lookAndProjMtx, projMtx, lookAtMtx);

    /* math would have been simpler if horizonPoint had an additional homogenous coordinate set to 1. Recreated here in
    extra steps */
    homogScale = ((lookAndProjMtx[0][3] * horizonPoint[0]) + (lookAndProjMtx[1][3] * horizonPoint[1]) +
                  (lookAndProjMtx[2][3] * horizonPoint[2])) +
                 lookAndProjMtx[3][3];
    mtxf_transform_vec3f_mat4(horizonPoint, lookAndProjMtx);

    homogFactor = (1.0 / homogScale);

    horizonPoint[0] *= homogFactor;
    horizonPoint[1] *= homogFactor;

    horizonPoint[0] *= 160.0f; // SCREEN_WIDTH / 2
    horizonPoint[1] *= 120.0f; // SCREEN_HEIGHT / 2

    horizonRow = 120 - (s16) horizonPoint[1];
    arg1->cameraHeight = horizonRow;

    skybox[1].v.ob[1] = horizonRow;
    skybox[2].v.ob[1] = horizonRow;
    skybox[4].v.ob[1] = horizonRow;
    skybox[7].v.ob[1] = horizonRow;

    // this section reders the skybox. Unclear if it does anything else
    init_rdp();
    gDPSetRenderMode(gDisplayListHead++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
    gSPClearGeometryMode(gDisplayListHead++, G_ZBUFFER | G_LIGHTING);
    guOrtho(&gGfxPool->mtxScreen, 0.0f, SCREEN_WIDTH, 0.0f, SCREEN_HEIGHT, 0.0f, 5.0f, 1.0f);
    gSPPerspNormalize(gDisplayListHead++, 0xFFFF);
    gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxScreen),
              G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&D_0D008E98), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPVertex(gDisplayListHead++, &skybox[0], 4, 0);
    gSP2Triangles(gDisplayListHead++, 0, 3, 1, 0, 1, 3, 2, 0);
    if (gCurrentCourseId == COURSE_RAINBOW_ROAD) {
        gSPVertex(gDisplayListHead++, &skybox[4], 4, 0);
        gSP2Triangles(gDisplayListHead++, 0, 3, 1, 0, 1, 3, 2, 0);
    }
}

void set_perspective_and_aspect_ratio(void) {
    if (gGamestate != 4) {
        gCourseFarPersp = 6800.0f;
        gCourseNearPersp = 3.0f;
    } else {
        switch (gCurrentCourseId) {
            case COURSE_BOWSER_CASTLE:
            case COURSE_BANSHEE_BOARDWALK:
            case COURSE_RAINBOW_ROAD:
            case COURSE_BLOCK_FORT:
            case COURSE_SKYSCRAPER:
                gCourseFarPersp = 2700.0f;
                gCourseNearPersp = 2.0f;
                break;
            case COURSE_CHOCO_MOUNTAIN:
            case COURSE_DOUBLE_DECK:
                gCourseFarPersp = 1500.0f;
                gCourseNearPersp = 2.0f;
                break;
            case COURSE_KOOPA_BEACH:
                gCourseFarPersp = 5000.0f;
                gCourseNearPersp = 1.0f;
                break;
            case COURSE_WARIO_STADIUM:
                gCourseFarPersp = 4800.0f;
                gCourseNearPersp = 10.0f;
                break;
            case COURSE_MARIO_RACEWAY:
            case COURSE_YOSHI_VALLEY:
            case COURSE_FRAPPE_SNOWLAND:
            case COURSE_ROYAL_RACEWAY:
            case COURSE_LUIGI_RACEWAY:
            case COURSE_MOO_MOO_FARM:
            case COURSE_TOADS_TURNPIKE:
            case COURSE_SHERBET_LAND:
            case COURSE_DK_JUNGLE:
                gCourseFarPersp = 4500.0f;
                gCourseNearPersp = 9.0f;
                break;
            case COURSE_KALAMARI_DESERT:
                gCourseFarPersp = 7000.0f;
                gCourseNearPersp = 10.0f;
                break;
            default:
                gCourseFarPersp = 6800.0f;
                gCourseNearPersp = 3.0f;
                break;
        }
    }
#ifdef XBOX360_PORT
    /* Fullscreen online uses the 1P projection before any clipping takes place.
     * Display aspect stays in the GPU bridge, so peers can choose independently. */
    if (x360_net_active() && gGamestate == 4) {
        gScreenAspect = 1.33333334f;
        return;
    }
#endif
    switch (gScreenModeSelection) { /* switch 1; irregular */
        case SCREEN_MODE_1P:        /* switch 1 */
            gScreenAspect = 1.33333334f;
            return;
        case SCREEN_MODE_2P_SPLITSCREEN_VERTICAL: /* switch 1 */
            gScreenAspect = 0.66666667f;
            return;
        case SCREEN_MODE_2P_SPLITSCREEN_HORIZONTAL: /* switch 1 */
            gScreenAspect = 2.66666667f;
            return;
        case SCREEN_MODE_3P_4P_SPLITSCREEN: /* switch 1 */
            gScreenAspect = 1.33333334f;
            return;
    }
}

void func_802A4EF4(void) {
    if(x360_net8_active()) {
        int i;for(i=0;i<x360_net_player_count();++i)func_8001F394(&gPlayers[i],&gCameraZoom[i]);
        return;
    }
    switch (gActiveScreenMode) {
        case SCREEN_MODE_1P:
            func_8001F394(gPlayerOne, &gCameraZoom[0]);
            break;

        case SCREEN_MODE_2P_SPLITSCREEN_VERTICAL:
            func_8001F394(gPlayerOne, &gCameraZoom[0]);
            func_8001F394(gPlayerTwo, &gCameraZoom[1]);
            break;
        case SCREEN_MODE_2P_SPLITSCREEN_HORIZONTAL:
            func_8001F394(gPlayerOne, &gCameraZoom[0]);
            func_8001F394(gPlayerTwo, &gCameraZoom[1]);
            break;
        case SCREEN_MODE_3P_4P_SPLITSCREEN:
            func_8001F394(gPlayerOne, &gCameraZoom[0]);
            func_8001F394(gPlayerTwo, &gCameraZoom[1]);
            func_8001F394(gPlayerThree, &gCameraZoom[2]);
            func_8001F394(gPlayerFour, &gCameraZoom[3]);
            break;
    }
}
// player 2 vertical
void func_802A5004(void) {

    init_rdp();
    func_802A3730(D_800DC5F0);

    gSPClearGeometryMode(gDisplayListHead++, 0xFFFFFFFF);

    gSPSetGeometryMode(gDisplayListHead++, G_SHADE | G_SHADING_SMOOTH | G_CLIPPING);

    func_802A39E0(D_800DC5F0);
    if (D_800DC5B4 != 0) {
        render_skybox((Vtx*) sSkyboxP2, D_800DC5F0, SCREEN_WIDTH, SCREEN_HEIGHT, &gCameraZoom[1]);
        func_80057FC4(2);
        func_802A487C((Vtx*) sSkyboxP2, D_800DC5F0, SCREEN_WIDTH, SCREEN_HEIGHT, &gCameraZoom[1]);
        func_80093A30(2);
    }
}
// player 1 vertical
void func_802A50EC(void) {

    init_rdp();
    func_802A3730(D_800DC5EC);

    gSPClearGeometryMode(gDisplayListHead++, 0xFFFFFFFF);
    gSPSetGeometryMode(gDisplayListHead++, G_SHADE | G_SHADING_SMOOTH | G_CLIPPING);

    func_802A39E0(D_800DC5EC);
    if (D_800DC5B4 != 0) {
        render_skybox((Vtx*) sSkyboxP1, D_800DC5EC, SCREEN_WIDTH, SCREEN_HEIGHT, &gCameraZoom[0]);
        func_80057FC4(1);
        func_802A487C((Vtx*) sSkyboxP1, D_800DC5EC, SCREEN_WIDTH, SCREEN_HEIGHT, &gCameraZoom[0]);
        func_80093A30(1);
    }
}
// player 1 horizontal
void func_802A51D4(void) {

    init_rdp();
    func_802A39E0(D_800DC5EC);
    func_802A3730(D_800DC5EC);

    gSPClearGeometryMode(gDisplayListHead++, 0xFFFFFFFF);
    gSPSetGeometryMode(gDisplayListHead++, G_SHADE | G_SHADING_SMOOTH | G_CLIPPING);

    if (D_800DC5B4 != 0) {
        render_skybox((Vtx*) sSkyboxP1, D_800DC5EC, SCREEN_WIDTH, SCREEN_HEIGHT, &gCameraZoom[0]);
        func_80057FC4(3);
        func_802A487C((Vtx*) sSkyboxP1, D_800DC5EC, SCREEN_WIDTH, SCREEN_HEIGHT, &gCameraZoom[0]);
        if(!x360_net8_active()) func_80093A30(3);
    }
}
// player 2 horizontal
void func_802A52BC(void) {

    init_rdp();
    func_802A39E0(D_800DC5F0);
    func_802A3730(D_800DC5F0);

    gSPClearGeometryMode(gDisplayListHead++, 0xFFFFFFFF);
    gSPSetGeometryMode(gDisplayListHead++, G_SHADE | G_SHADING_SMOOTH | G_CLIPPING);

    if (D_800DC5B4 != 0) {
        render_skybox((Vtx*) sSkyboxP2, D_800DC5F0, SCREEN_WIDTH, SCREEN_HEIGHT, &gCameraZoom[1]);
        func_80057FC4(4);
        func_802A487C((Vtx*) sSkyboxP2, D_800DC5F0, SCREEN_WIDTH, SCREEN_HEIGHT, &gCameraZoom[1]);
        if(!x360_net8_active()) func_80093A30(4);
    }
}
// player 1 solo
void func_802A53A4(void) {

    move_segment_table_to_dmem();
    init_rdp();
    func_802A3730(D_800DC5EC);

    gSPClearGeometryMode(gDisplayListHead++, 0xFFFFFFFF);
    gSPSetGeometryMode(gDisplayListHead++, G_SHADE | G_SHADING_SMOOTH | G_CLIPPING);

    init_z_buffer();
    select_framebuffer();
    if (D_800DC5B4 != 0) {
        render_skybox((Vtx*) sSkyboxP1, D_800DC5EC, 0x140, 0xF0, &gCameraZoom[0]);
        if (gGamestate != CREDITS_SEQUENCE) {
            func_80057FC4(0);
        }
        func_802A487C((Vtx*) sSkyboxP1, D_800DC5EC, 0x140, 0xF0, &gCameraZoom[0]);
        if(!x360_net8_active()) func_80093A30(0);
    }
}
// player 1 3p 4p
void func_802A54A8(void) {

    init_rdp();
    func_802A39E0(D_800DC5EC);
    func_802A3730(D_800DC5EC);

    gSPClearGeometryMode(gDisplayListHead++, 0xFFFFFFFF);
    gSPSetGeometryMode(gDisplayListHead++, G_SHADE | G_SHADING_SMOOTH | G_CLIPPING);

    if (D_800DC5B4 != 0) {
        render_skybox((Vtx*) sSkyboxP1, D_800DC5EC, 0x140, 0xF0, &gCameraZoom[0]);
        func_80057FC4(8);
        func_802A487C((Vtx*) sSkyboxP1, D_800DC5EC, 0x140, 0xF0, &gCameraZoom[0]);
        func_80093A30(8);
    }
}
// player 2 3p 4p
void func_802A5590(void) {

    init_rdp();
    func_802A39E0(D_800DC5F0);
    func_802A3730(D_800DC5F0);

    gSPClearGeometryMode(gDisplayListHead++, 0xFFFFFFFF);
    gSPSetGeometryMode(gDisplayListHead++, G_SHADE | G_SHADING_SMOOTH | G_CLIPPING);

    if (D_800DC5B4 != 0) {
        render_skybox((Vtx*) sSkyboxP2, D_800DC5F0, SCREEN_WIDTH, SCREEN_HEIGHT, &gCameraZoom[1]);
        func_80057FC4(9);
        func_802A487C((Vtx*) sSkyboxP2, D_800DC5F0, SCREEN_WIDTH, SCREEN_HEIGHT, &gCameraZoom[1]);
        func_80093A30(9);
    }
}
// player 3 3p4p
void func_802A5678(void) {

    init_rdp();
    func_802A39E0(D_800DC5F4);
    func_802A3730(D_800DC5F4);

    gSPClearGeometryMode(gDisplayListHead++, 0xFFFFFFFF);
    gSPSetGeometryMode(gDisplayListHead++, G_SHADE | G_SHADING_SMOOTH | G_CLIPPING);

    if (D_800DC5B4 != 0) {
        render_skybox((Vtx*) sSkyboxP3, D_800DC5F4, SCREEN_WIDTH, SCREEN_HEIGHT, &gCameraZoom[2]);
        func_80057FC4(10);
        func_802A487C((Vtx*) sSkyboxP3, D_800DC5F4, SCREEN_WIDTH, SCREEN_HEIGHT, &gCameraZoom[2]);
        func_80093A30(10);
    }
}

// player 4 3p 4p
void func_802A5760(void) {

    init_rdp();

    gSPClearGeometryMode(gDisplayListHead++, 0xFFFFFFFF);
    gSPSetGeometryMode(gDisplayListHead++, G_SHADE | G_SHADING_SMOOTH | G_CLIPPING);

    if (gPlayerCountSelection1 == 3) {

        gDPPipeSync(gDisplayListHead++);
        func_802A39E0(D_800DC5F8);
        gDPSetCycleType(gDisplayListHead++, G_CYC_FILL);
        gDPSetColorImage(gDisplayListHead++, G_IM_FMT_RGBA, G_IM_SIZ_16b, SCREEN_WIDTH,
                         VIRTUAL_TO_PHYSICAL(gPhysicalFramebuffers[sRenderingFramebuffer]));
        gDPSetFillColor(gDisplayListHead++, 0x00010001);
        gDPPipeSync(gDisplayListHead++);
        gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, 160, 120, SCREEN_WIDTH, SCREEN_HEIGHT);
        gDPFillRectangle(gDisplayListHead++, 160, 120, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1);
        gDPPipeSync(gDisplayListHead++);
        gDPSetCycleType(gDisplayListHead++, G_CYC_1CYCLE);

        func_802A3730(D_800DC5F8);

    } else {
        func_802A3730(D_800DC5F8);
        func_802A39E0(D_800DC5F8);

        if (D_800DC5B4 != 0) {
            render_skybox(sSkyboxP4, D_800DC5F8, SCREEN_WIDTH, SCREEN_HEIGHT, &gCameraZoom[3]);
            func_80057FC4(11);
            func_802A487C(sSkyboxP4, D_800DC5F8, SCREEN_WIDTH, SCREEN_HEIGHT, &gCameraZoom[3]);
            func_80093A30(11);
        }
    }
}

void render_player_one_1p_screen(void) {
    Camera* camera = &cameras[0];
    UNUSED s32 pad[4];
    u16 perspNorm;
    UNUSED s32 pad2[2];
#ifdef VERSION_EU
    f32 sp9C;
#endif
    UNUSED s32 pad3;
    Mat4 matrix;

#ifdef VERSION_EU
    sp9C = race_view_aspect() * 1.2f;
#endif
    func_802A53A4();
    init_rdp();
    func_802A3730(D_800DC5EC);
    gSPSetGeometryMode(gDisplayListHead++, G_ZBUFFER | G_SHADE | G_CULL_BACK | G_LIGHTING | G_SHADING_SMOOTH);
    gDPSetRenderMode(gDisplayListHead++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
#ifdef VERSION_EU
    r361_guPerspective(&gGfxPool->mtxPersp[0], &perspNorm, gCameraZoom[0], sp9C, gCourseNearPersp, gCourseFarPersp, 1.0f);
#else
    r361_guPerspective(&gGfxPool->mtxPersp[0], &perspNorm, gCameraZoom[0], race_view_aspect(), gCourseNearPersp, gCourseFarPersp,
                  1.0f);
#endif
    gSPPerspNormalize(gDisplayListHead++, perspNorm);
    gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxPersp[0]),
              G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);

    r37_guLookAt(&gGfxPool->mtxLookAt[0], camera->pos[0], camera->pos[1], camera->pos[2], camera->lookAt[0],
             camera->lookAt[1], camera->lookAt[2], camera->up[0], camera->up[1], camera->up[2]);
    if (D_800DC5C8 == 0) {
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[0]),
                  G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
        mtxf_identity(matrix);
        render_set_position(matrix, 0);
    } else {
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[0]),
                  G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    }
    render_course(D_800DC5EC);
    if (D_800DC5C8 == 1) {
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[0]),
                  G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
        mtxf_identity(matrix);
        render_set_position(matrix, 0);
    }
    render_course_actors(D_800DC5EC);
    render_object(RENDER_SCREEN_MODE_1P_PLAYER_ONE);
    render_players_on_screen_one();
    func_8029122C(D_800DC5EC, PLAYER_ONE);
    func_80021B0C();
    render_item_boxes(D_800DC5EC);
    render_player_snow_effect(RENDER_SCREEN_MODE_1P_PLAYER_ONE);
    /*
     * MK64_ONLINE_SINGLE_VIEW_HUD_RANK_V11
     * All online modes draw their corrected local-slot HUD once at the
     * end-of-frame via x360_race8_render_hud(). Do not also draw the stock
     * PLAYER_ONE HUD here in ordinary non-split 2-4P online.
     */
    if(x360_net_active()) return;
    func_80058BF4();
    if (D_800DC5B8 != 0) {
        func_80058C20(RENDER_SCREEN_MODE_1P_PLAYER_ONE);
    }
    func_80093A5C(RENDER_SCREEN_MODE_1P_PLAYER_ONE);
    if (D_800DC5B8 != 0) {
        render_hud(RENDER_SCREEN_MODE_1P_PLAYER_ONE);
    }
}

void render_player_one_2p_screen_vertical(void) {
    Camera* camera = &cameras[0];
    UNUSED s32 pad[2];
    u16 perspNorm;
    Mat4 matrix;
#ifdef VERSION_EU
    f32 sp9C;
#else
    UNUSED f32 sp9C;
#endif

    func_802A50EC();
#ifdef VERSION_EU
    sp9C = race_view_aspect() * 1.2f;
#endif
    init_rdp();
    func_802A3730(D_800DC5EC);
    gSPSetGeometryMode(gDisplayListHead++, G_ZBUFFER | G_SHADE | G_CULL_BACK | G_SHADING_SMOOTH);
#ifdef VERSION_EU
    r361_guPerspective(&gGfxPool->mtxPersp[0], &perspNorm, gCameraZoom[0], sp9C, gCourseNearPersp, gCourseFarPersp, 1.0f);
#else
    r361_guPerspective(&gGfxPool->mtxPersp[0], &perspNorm, gCameraZoom[0], race_view_aspect(), gCourseNearPersp, gCourseFarPersp,
                  1.0f);
#endif
    gSPPerspNormalize(gDisplayListHead++, perspNorm);
    gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxPersp[0]),
              G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    r37_guLookAt(&gGfxPool->mtxLookAt[0], camera->pos[0], camera->pos[1], camera->pos[2], camera->lookAt[0],
             camera->lookAt[1], camera->lookAt[2], camera->up[0], camera->up[1], camera->up[2]);

    if (D_800DC5C8 == 0) {

        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[0]),
                  G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
        mtxf_identity(matrix);
        render_set_position(matrix, 0);
    } else {
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[0]),
                  G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    }
    render_course(D_800DC5EC);
    if (D_800DC5C8 == 1) {

        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[0]),
                  G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);

        mtxf_identity(matrix);
        render_set_position(matrix, 0);
    }
    render_course_actors(D_800DC5EC);
    render_object(RENDER_SCREEN_MODE_2P_HORIZONTAL_PLAYER_ONE);
    render_players_on_screen_one();
    func_8029122C(D_800DC5EC, PLAYER_ONE);
    func_80021B0C();
    render_item_boxes(D_800DC5EC);
    render_player_snow_effect(RENDER_SCREEN_MODE_2P_HORIZONTAL_PLAYER_ONE);
    func_80058BF4();
    if (D_800DC5B8 != 0) {
        func_80058C20(RENDER_SCREEN_MODE_2P_HORIZONTAL_PLAYER_ONE);
    }
    func_80093A5C(RENDER_SCREEN_MODE_2P_HORIZONTAL_PLAYER_ONE);
    if (D_800DC5B8 != 0) {
        render_hud(RENDER_SCREEN_MODE_2P_HORIZONTAL_PLAYER_ONE);
    }
    D_8015F788 += 1;
}

void render_player_two_2p_screen_vertical(void) {
    Camera* camera = &cameras[1];
    UNUSED s32 pad[2];
    u16 perspNorm;
    Mat4 matrix;
#ifdef VERSION_EU
    f32 sp9C;
#else
    UNUSED f32 sp9C;
#endif

    func_802A5004();
    init_rdp();
    func_802A3730(D_800DC5F0);
#ifdef VERSION_EU
    sp9C = race_view_aspect() * 1.2f;
#endif
    gSPSetGeometryMode(gDisplayListHead++, G_ZBUFFER | G_SHADE | G_CULL_BACK | G_SHADING_SMOOTH);
#ifdef VERSION_EU
    r361_guPerspective(&gGfxPool->mtxPersp[1], &perspNorm, gCameraZoom[1], sp9C, gCourseNearPersp, gCourseFarPersp, 1.0f);
#else
    r361_guPerspective(&gGfxPool->mtxPersp[1], &perspNorm, gCameraZoom[1], race_view_aspect(), gCourseNearPersp, gCourseFarPersp,
                  1.0f);
#endif
    gSPPerspNormalize(gDisplayListHead++, perspNorm);
    gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxPersp[1]),
              G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    r37_guLookAt(&gGfxPool->mtxLookAt[1], camera->pos[0], camera->pos[1], camera->pos[2], camera->lookAt[0],
             camera->lookAt[1], camera->lookAt[2], camera->up[0], camera->up[1], camera->up[2]);

    if (D_800DC5C8 == 0) {
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[1]),
                  G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
        mtxf_identity(matrix);
        render_set_position(matrix, 0);
    } else {
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[1]),
                  G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    }
    render_course(D_800DC5F0);
    if (D_800DC5C8 == 1) {
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[1]),
                  G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
        mtxf_identity(matrix);
        render_set_position(matrix, 0);
    }
    render_course_actors(D_800DC5F0);
    render_object(RENDER_SCREEN_MODE_2P_HORIZONTAL_PLAYER_TWO);
    render_players_on_screen_two();
    func_8029122C(D_800DC5F0, PLAYER_TWO);
    func_80021C78();
    render_item_boxes(D_800DC5F0);
    func_80058BF4();
    render_player_snow_effect(RENDER_SCREEN_MODE_2P_HORIZONTAL_PLAYER_TWO);
    if (D_800DC5B8 != 0) {
        func_80058C20(RENDER_SCREEN_MODE_2P_HORIZONTAL_PLAYER_TWO);
    }
    func_80093A5C(RENDER_SCREEN_MODE_2P_HORIZONTAL_PLAYER_TWO);
    if (D_800DC5B8 != 0) {
        render_hud(RENDER_SCREEN_MODE_2P_HORIZONTAL_PLAYER_TWO);
    }
    D_8015F788 += 1;
}

void render_player_one_2p_screen_horizontal(void) {
    Camera* camera = &cameras[0];
    UNUSED s32 pad[2];
    u16 perspNorm;
    Mat4 matrix;
#ifdef VERSION_EU
    f32 sp9C;
#endif

    func_802A51D4();
    gSPSetGeometryMode(gDisplayListHead++, G_SHADE | G_CULL_BACK | G_LIGHTING | G_SHADING_SMOOTH);
    init_rdp();
    func_802A3730(D_800DC5EC);
#ifdef VERSION_EU
    sp9C = race_view_aspect() * 1.2f;
#endif
    gSPSetGeometryMode(gDisplayListHead++, G_ZBUFFER | G_SHADE | G_CULL_BACK | G_SHADING_SMOOTH);
#ifdef VERSION_EU
    r361_guPerspective(&gGfxPool->mtxPersp[0], &perspNorm, gCameraZoom[0], sp9C, gCourseNearPersp, gCourseFarPersp, 1.0f);
#else
    r361_guPerspective(&gGfxPool->mtxPersp[0], &perspNorm, gCameraZoom[0], race_view_aspect(), gCourseNearPersp, gCourseFarPersp,
                  1.0f);
#endif
    gSPPerspNormalize(gDisplayListHead++, perspNorm);
    gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxPersp[0]),
              G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    r37_guLookAt(&gGfxPool->mtxLookAt[0], camera->pos[0], camera->pos[1], camera->pos[2], camera->lookAt[0],
             camera->lookAt[1], camera->lookAt[2], camera->up[0], camera->up[1], camera->up[2]);

    if (D_800DC5C8 == 0) {

        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[0]),
                  G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
        mtxf_identity(matrix);
        render_set_position(matrix, 0);
    } else {
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[0]),
                  G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    }
    render_course(D_800DC5EC);
    if (D_800DC5C8 == 1) {
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[0]),
                  G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
        mtxf_identity(matrix);
        render_set_position(matrix, 0);
    }
    render_course_actors(D_800DC5EC);
    render_object(RENDER_SCREEN_MODE_2P_VERTICAL_PLAYER_ONE);
    render_players_on_screen_one();
    func_8029122C(D_800DC5EC, PLAYER_ONE);
    func_80021B0C();
    render_item_boxes(D_800DC5EC);
    render_player_snow_effect(RENDER_SCREEN_MODE_2P_VERTICAL_PLAYER_ONE);
    if(x360_net8_active()){D_8015F788+=1;return;}
    func_80058BF4();
    if (D_800DC5B8 != 0) {
        func_80058C20(RENDER_SCREEN_MODE_2P_VERTICAL_PLAYER_ONE);
    }
    func_80093A5C(RENDER_SCREEN_MODE_2P_VERTICAL_PLAYER_ONE);
    if (D_800DC5B8 != 0) {
        render_hud(RENDER_SCREEN_MODE_2P_VERTICAL_PLAYER_ONE);
    }
    D_8015F788 += 1;
}

void render_player_two_2p_screen_horizontal(void) {
    Camera* camera = &cameras[1];
    UNUSED s32 pad[2];
    u16 perspNorm;
    Mat4 matrix;
#ifdef VERSION_EU
    f32 sp9C;
#endif

    func_802A52BC();
    gSPSetGeometryMode(gDisplayListHead++, G_SHADE | G_CULL_BACK | G_LIGHTING | G_SHADING_SMOOTH);
    init_rdp();
    func_802A3730(D_800DC5F0);
#ifdef VERSION_EU
    sp9C = race_view_aspect() * 1.2f;
#endif
    gSPSetGeometryMode(gDisplayListHead++, G_ZBUFFER | G_SHADE | G_CULL_BACK | G_SHADING_SMOOTH);
#ifdef VERSION_EU
    r361_guPerspective(&gGfxPool->mtxPersp[1], &perspNorm, gCameraZoom[1], sp9C, gCourseNearPersp, gCourseFarPersp, 1.0f);
#else
    r361_guPerspective(&gGfxPool->mtxPersp[1], &perspNorm, gCameraZoom[1], race_view_aspect(), gCourseNearPersp, gCourseFarPersp,
                  1.0f);
#endif
    gSPPerspNormalize(gDisplayListHead++, perspNorm);
    gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxPersp[1]),
              G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    r37_guLookAt(&gGfxPool->mtxLookAt[1], camera->pos[0], camera->pos[1], camera->pos[2], camera->lookAt[0],
             camera->lookAt[1], camera->lookAt[2], camera->up[0], camera->up[1], camera->up[2]);

    if (D_800DC5C8 == 0) {
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[1]),
                  G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
        mtxf_identity(matrix);
        render_set_position(matrix, 0);
    } else {
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[1]),
                  G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    }
    render_course(D_800DC5F0);
    if (D_800DC5C8 == 1) {
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[1]),
                  G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
        mtxf_identity(matrix);
        render_set_position(matrix, 0);
    }
    render_course_actors(D_800DC5F0);
    render_object(RENDER_SCREEN_MODE_2P_VERTICAL_PLAYER_TWO);
    render_players_on_screen_two();
    func_8029122C(D_800DC5F0, PLAYER_TWO);
    func_80021C78();
    render_item_boxes(D_800DC5F0);
    render_player_snow_effect(RENDER_SCREEN_MODE_2P_VERTICAL_PLAYER_TWO);
    if(x360_net8_active()){D_8015F788+=1;return;}
    func_80058BF4();
    if (D_800DC5B8 != 0) {
        func_80058C20(RENDER_SCREEN_MODE_2P_VERTICAL_PLAYER_TWO);
    }
    func_80093A5C(RENDER_SCREEN_MODE_2P_VERTICAL_PLAYER_TWO);
    if (D_800DC5B8 != 0) {
        render_hud(RENDER_SCREEN_MODE_2P_VERTICAL_PLAYER_TWO);
    }
    D_8015F788 += 1;
}

void render_player_one_3p_4p_screen(void) {
    Camera* camera = camera1;
    UNUSED s32 pad[2];
    u16 perspNorm;
    Mat4 matrix;
#ifdef VERSION_EU
    f32 sp9C;
    sp9C = race_view_aspect() * 1.2f;
#endif

    func_802A54A8();
    init_rdp();
    func_802A3730(D_800DC5EC);
    gSPSetGeometryMode(gDisplayListHead++, G_ZBUFFER | G_SHADE | G_CULL_BACK | G_SHADING_SMOOTH);
#ifdef VERSION_EU
    r361_guPerspective(&gGfxPool->mtxPersp[0], &perspNorm, gCameraZoom[0], sp9C, gCourseNearPersp, gCourseFarPersp, 1.0f);
#else
    r361_guPerspective(&gGfxPool->mtxPersp[0], &perspNorm, gCameraZoom[0], race_view_aspect(), gCourseNearPersp, gCourseFarPersp,
                  1.0f);
#endif
    gSPPerspNormalize(gDisplayListHead++, perspNorm);
    gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxPersp[0]),
              G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    r37_guLookAt(&gGfxPool->mtxLookAt[0], camera->pos[0], camera->pos[1], camera->pos[2], camera->lookAt[0],
             camera->lookAt[1], camera->lookAt[2], camera->up[0], camera->up[1], camera->up[2]);

    if (D_800DC5C8 == 0) {
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[0]),
                  G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
        mtxf_identity(matrix);
        render_set_position(matrix, 0);
    } else {
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[0]),
                  G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    }
    render_course(D_800DC5EC);
    if (D_800DC5C8 == 1) {
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[0]),
                  G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
        mtxf_identity(matrix);
        render_set_position(matrix, 0);
    }
    render_course_actors(D_800DC5EC);
    render_object(RENDER_SCREEN_MODE_3P_4P_PLAYER_ONE);
    render_players_on_screen_one();
    func_8029122C(D_800DC5EC, PLAYER_ONE);
    func_80021B0C();
    render_item_boxes(D_800DC5EC);
    render_player_snow_effect(RENDER_SCREEN_MODE_3P_4P_PLAYER_ONE);
    func_80058BF4();
    if (D_800DC5B8 != 0) {
        func_80058C20(RENDER_SCREEN_MODE_3P_4P_PLAYER_ONE);
    }
    func_80093A5C(RENDER_SCREEN_MODE_3P_4P_PLAYER_ONE);
    if (D_800DC5B8 != 0) {
        render_hud(RENDER_SCREEN_MODE_3P_4P_PLAYER_ONE);
    }
    D_8015F788 += 1;
}

void render_player_two_3p_4p_screen(void) {
    Camera* camera = camera2;
    UNUSED s32 pad[2];
    u16 perspNorm;
    Mat4 matrix;
#ifdef VERSION_EU
    f32 sp9C;
    sp9C = race_view_aspect() * 1.2f;
#endif

    func_802A5590();
    init_rdp();
    func_802A3730(D_800DC5F0);
    gSPSetGeometryMode(gDisplayListHead++, G_ZBUFFER | G_SHADE | G_CULL_BACK | G_SHADING_SMOOTH);
#ifdef VERSION_EU
    r361_guPerspective(&gGfxPool->mtxPersp[1], &perspNorm, gCameraZoom[1], sp9C, gCourseNearPersp, gCourseFarPersp, 1.0f);
#else
    r361_guPerspective(&gGfxPool->mtxPersp[1], &perspNorm, gCameraZoom[1], race_view_aspect(), gCourseNearPersp, gCourseFarPersp,
                  1.0f);
#endif
    gSPPerspNormalize(gDisplayListHead++, perspNorm);
    gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxPersp[1]),
              G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);

    r37_guLookAt(&gGfxPool->mtxLookAt[1], camera->pos[0], camera->pos[1], camera->pos[2], camera->lookAt[0],
             camera->lookAt[1], camera->lookAt[2], camera->up[0], camera->up[1], camera->up[2]);
    if (D_800DC5C8 == 0) {
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[1]),
                  G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
        mtxf_identity(matrix);
        render_set_position(matrix, 0);
    } else {
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[1]),
                  G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    }
    render_course(D_800DC5F0);
    if (D_800DC5C8 == 1) {
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[1]),
                  G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
        mtxf_identity(matrix);
        render_set_position(matrix, 0);
    }
    render_course_actors(D_800DC5F0);
    render_object(RENDER_SCREEN_MODE_3P_4P_PLAYER_TWO);
    render_players_on_screen_two();
    func_8029122C(D_800DC5F0, PLAYER_TWO);
    func_80021C78();
    render_item_boxes(D_800DC5F0);
    render_player_snow_effect(RENDER_SCREEN_MODE_3P_4P_PLAYER_TWO);
    func_80058BF4();
    if (D_800DC5B8 != 0) {
        func_80058C20(RENDER_SCREEN_MODE_3P_4P_PLAYER_TWO);
    }
    func_80093A5C(RENDER_SCREEN_MODE_3P_4P_PLAYER_TWO);
    if (D_800DC5B8 != 0) {
        render_hud(RENDER_SCREEN_MODE_3P_4P_PLAYER_TWO);
    }
    D_8015F788 += 1;
}

void render_player_three_3p_4p_screen(void) {
    Camera* camera = camera3;
    UNUSED s32 pad[2];
    u16 perspNorm;
    Mat4 matrix;
#ifdef VERSION_EU
    f32 sp9C;
    sp9C = race_view_aspect() * 1.2f;
#endif

    func_802A5678();
    init_rdp();
    func_802A3730(D_800DC5F4);

    gSPSetGeometryMode(gDisplayListHead++, G_ZBUFFER | G_SHADE | G_CULL_BACK | G_SHADING_SMOOTH);
#ifdef VERSION_EU
    r361_guPerspective(&gGfxPool->mtxPersp[2], &perspNorm, gCameraZoom[2], sp9C, gCourseNearPersp, gCourseFarPersp, 1.0f);
#else
    r361_guPerspective(&gGfxPool->mtxPersp[2], &perspNorm, gCameraZoom[2], race_view_aspect(), gCourseNearPersp, gCourseFarPersp,
                  1.0f);
#endif
    gSPPerspNormalize(gDisplayListHead++, perspNorm);
    gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxPersp[2]),
              G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    r37_guLookAt(&gGfxPool->mtxLookAt[2], camera->pos[0], camera->pos[1], camera->pos[2], camera->lookAt[0],
             camera->lookAt[1], camera->lookAt[2], camera->up[0], camera->up[1], camera->up[2]);
    if (D_800DC5C8 == 0) {
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[2]),
                  G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);

        mtxf_identity(matrix);
        render_set_position(matrix, 0);
    } else {
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[2]),
                  G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    }
    render_course(D_800DC5F4);
    if (D_800DC5C8 == 1) {
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[2]),
                  G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
        mtxf_identity(matrix);
        render_set_position(matrix, 0);
    }
    render_course_actors(D_800DC5F4);
    render_object(RENDER_SCREEN_MODE_3P_4P_PLAYER_THREE);
    render_players_on_screen_three();
    func_8029122C(D_800DC5F4, PLAYER_THREE);
    func_80021D40();
    render_item_boxes(D_800DC5F4);
    render_player_snow_effect(RENDER_SCREEN_MODE_3P_4P_PLAYER_THREE);
    func_80058BF4();
    if (D_800DC5B8 != 0) {
        func_80058C20(RENDER_SCREEN_MODE_3P_4P_PLAYER_THREE);
    }
    func_80093A5C(RENDER_SCREEN_MODE_3P_4P_PLAYER_THREE);
    if (D_800DC5B8 != 0) {
        render_hud(RENDER_SCREEN_MODE_3P_4P_PLAYER_THREE);
    }
    D_8015F788 += 1;
}

void render_player_four_3p_4p_screen(void) {
    Camera* camera = camera4;
    UNUSED s32 pad[2];
    u16 perspNorm;
    Mat4 matrix;
#ifdef VERSION_EU
    f32 sp9C;
    sp9C = race_view_aspect() * 1.2f;
#endif

    func_802A5760();
    if (gPlayerCountSelection1 == 3) {
        func_80093A5C(RENDER_SCREEN_MODE_3P_4P_PLAYER_FOUR);
        if (D_800DC5B8 != 0) {
            render_hud(RENDER_SCREEN_MODE_3P_4P_PLAYER_FOUR);
        }
        D_8015F788 += 1;
        return;
    }

    init_rdp();
    func_802A3730(D_800DC5F8);

    gSPSetGeometryMode(gDisplayListHead++, G_ZBUFFER | G_SHADE | G_CULL_BACK | G_SHADING_SMOOTH);
#ifdef VERSION_EU
    r361_guPerspective(&gGfxPool->mtxPersp[3], &perspNorm, gCameraZoom[3], sp9C, gCourseNearPersp, gCourseFarPersp, 1.0f);
#else
    r361_guPerspective(&gGfxPool->mtxPersp[3], &perspNorm, gCameraZoom[3], race_view_aspect(), gCourseNearPersp, gCourseFarPersp,
                  1.0f);
#endif
    gSPPerspNormalize(gDisplayListHead++, perspNorm);
    gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxPersp[3]),
              G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    r37_guLookAt(&gGfxPool->mtxLookAt[3], camera->pos[0], camera->pos[1], camera->pos[2], camera->lookAt[0],
             camera->lookAt[1], camera->lookAt[2], camera->up[0], camera->up[1], camera->up[2]);
    if (D_800DC5C8 == 0) {
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[3]),
                  G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
        mtxf_identity(matrix);
        render_set_position(matrix, 0);
    } else {
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[3]),
                  G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    }
    render_course(D_800DC5F8);
    if (D_800DC5C8 == 1) {
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gGfxPool->mtxLookAt[3]),
                  G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
        mtxf_identity(matrix);
        render_set_position(matrix, 0);
    }
    render_course_actors(D_800DC5F8);
    render_object(RENDER_SCREEN_MODE_3P_4P_PLAYER_FOUR);
    render_players_on_screen_four();
    func_8029122C(D_800DC5F8, PLAYER_FOUR);
    func_80021DA8();
    render_item_boxes(D_800DC5F8);
    render_player_snow_effect(RENDER_SCREEN_MODE_3P_4P_PLAYER_FOUR);
    func_80058BF4();
    if (D_800DC5B8 != 0) {
        func_80058C20(RENDER_SCREEN_MODE_3P_4P_PLAYER_FOUR);
    }
    func_80093A5C(RENDER_SCREEN_MODE_3P_4P_PLAYER_FOUR);
    if (D_800DC5B8 != 0) {
        render_hud(RENDER_SCREEN_MODE_3P_4P_PLAYER_FOUR);
    }
    D_8015F788 += 1;
}

void func_802A74BC(void) {
    struct UnkStruct_800DC5EC* wrapper = &D_8015F480[0];
    Player* player = &gPlayers[0];
    Camera* camera = &cameras[0];
    struct Controller* controller = &gControllers[0];

    // struct? size = 0x10. unk++ doesn't work cause s32 too small.
    s32* unk = &D_8015F790[0];
    s32 i;

    for (i = 0; i < 4; i++) {
        wrapper->controllers = controller;
        wrapper->camera = camera;
        wrapper->player = player;
        wrapper->unkC = unk;
        wrapper->screenWidth = 4;
        wrapper->screenHeight = 4;
        wrapper->pathCounter = 1;

        switch (gActiveScreenMode) {
            case SCREEN_MODE_1P:
                if (i == 0) {
                    wrapper->screenStartX = 160;
                }
                wrapper->screenStartY = 120;
                break;
            case SCREEN_MODE_2P_SPLITSCREEN_VERTICAL:
                if (i == 0) {
                    wrapper->screenStartX = 80;
                    wrapper->screenStartY = 120;
                } else if (i == 1) {
                    wrapper->screenStartX = 240;
                    wrapper->screenStartY = 120;
                }
                break;
            case SCREEN_MODE_2P_SPLITSCREEN_HORIZONTAL:
                if (i == 0) {
                    wrapper->screenStartX = 160;
                    wrapper->screenStartY = 60;
                } else if (i == 1) {
                    wrapper->screenStartX = 160;
                    wrapper->screenStartY = 180;
                }
                break;
            case SCREEN_MODE_3P_4P_SPLITSCREEN:
                if (i == 0) {
                    wrapper->screenStartX = 80;
                    wrapper->screenStartY = 60;
                } else if (i == 1) {
                    wrapper->screenStartX = 240;
                    wrapper->screenStartY = 60;
                } else if (i == 2) {
                    wrapper->screenStartX = 80;
                    wrapper->screenStartY = 180;
                } else {
                    wrapper->screenStartX = 240;
                    wrapper->screenStartY = 180;
                }
                break;
        }
        player++;
        camera++;
        wrapper++;
        unk += 0x10;
    }
}

#ifdef XBOX360_PORT
/* Jumbotron (Luigi Raceway / Wario Stadium). On the N64 the game copies pieces of
   the RDRAM framebuffer into the jumbotron textures. On the Xbox 360 the image is
   drawn by the GPU and that framebuffer is never filled, so the jumbotron stayed
   white. The renderer supplies the piece from a recent screen capture, and gfx_pc.c
   keeps the written blocks for the jumbotron draw. */
#ifdef __cplusplus
extern "C" {
#endif
int x360_capture_n64_region(int x, int y, int w, int h, u16* target);
void x360_telao_set_ram_seg5(uintptr_t base, uintptr_t bloco);
void x360_telao_gravado(uintptr_t base, uintptr_t bloco, const void* dados, int bytes);
void x360_telao_contexto(int modo, uintptr_t base);
#ifdef __cplusplus
}
#endif
#endif

void copy_framebuffer(s32 arg0, s32 arg1, s32 width, s32 height, u16* source, u16* target) {
    s32 var_v1;
    s32 var_a1;
    s32 targetIndex;
    s32 sourceIndex;
#ifdef XBOX360_PORT
    x360_telao_set_ram_seg5((uintptr_t) PHYSICAL_TO_VIRTUAL(gSegmentTable[5]), (uintptr_t) target);
    x360_telao_contexto(gActiveScreenMode, (uintptr_t) PHYSICAL_TO_VIRTUAL(gSegmentTable[5]));
    if (x360_capture_n64_region(arg0, arg1, width, height, target)) {
        x360_telao_gravado((uintptr_t) PHYSICAL_TO_VIRTUAL(gSegmentTable[5]), (uintptr_t) target,
                           target, width * height * 2);
        return;
    }
#endif

    targetIndex = 0;
    for (var_v1 = 0; var_v1 < height; var_v1++) {
        sourceIndex = ((arg1 + var_v1) * 320) + arg0;
        for (var_a1 = 0; var_a1 < width; var_a1++, targetIndex++, sourceIndex++) {
            target[targetIndex] = source[sourceIndex];
        }
    }
}

void func_802A7728(void) {
    s16 temp_v0;

    if (gActiveScreenMode == SCREEN_MODE_3P_4P_SPLITSCREEN) {
        D_800DC5DC = 0;
    } else {
        D_800DC5DC = 128;
    }
    D_800DC5E0 = 0;
    temp_v0 = (s16) sRenderedFramebuffer - 1;
    if (temp_v0 < 0) {
        temp_v0 = 2;
    } else if (temp_v0 > 2) {
        temp_v0 = 0;
    }
    copy_framebuffer(D_800DC5DC, D_800DC5E0, 64, 32, (u16*) PHYSICAL_TO_VIRTUAL(gPhysicalFramebuffers[temp_v0]),
                     (u16*) PHYSICAL_TO_VIRTUAL(gSegmentTable[5] + 0x8800));
    copy_framebuffer(D_800DC5DC + 64, D_800DC5E0, 64, 32, (u16*) PHYSICAL_TO_VIRTUAL(gPhysicalFramebuffers[temp_v0]),
                     (u16*) PHYSICAL_TO_VIRTUAL(gSegmentTable[5] + 0x9800));
    copy_framebuffer(D_800DC5DC, D_800DC5E0 + 32, 64, 32, (u16*) PHYSICAL_TO_VIRTUAL(gPhysicalFramebuffers[temp_v0]),
                     (u16*) PHYSICAL_TO_VIRTUAL(gSegmentTable[5] + 0xA800));
    copy_framebuffer(D_800DC5DC + 64, D_800DC5E0 + 32, 64, 32,
                     (u16*) PHYSICAL_TO_VIRTUAL(gPhysicalFramebuffers[temp_v0]),
                     (u16*) PHYSICAL_TO_VIRTUAL(gSegmentTable[5] + 0xB800));
    copy_framebuffer(D_800DC5DC, D_800DC5E0 + 64, 64, 32, (u16*) PHYSICAL_TO_VIRTUAL(gPhysicalFramebuffers[temp_v0]),
                     (u16*) PHYSICAL_TO_VIRTUAL(gSegmentTable[5] + 0xC800));
    copy_framebuffer(D_800DC5DC + 64, D_800DC5E0 + 64, 64, 32,
                     (u16*) PHYSICAL_TO_VIRTUAL(gPhysicalFramebuffers[temp_v0]),
                     (u16*) PHYSICAL_TO_VIRTUAL(gSegmentTable[5] + 0xD800));
}

void func_802A7940(void) {
    s16 temp_v0;

    if (gActiveScreenMode == SCREEN_MODE_3P_4P_SPLITSCREEN) {
        D_800DC5DC = 0;
    } else {
        D_800DC5DC = 128;
    }
    D_800DC5E0 = 0;
    temp_v0 = (s16) sRenderedFramebuffer - 1;
    if (temp_v0 < 0) {
        temp_v0 = 2;
    } else if (temp_v0 > 2) {
        temp_v0 = 0;
    }
    copy_framebuffer(D_800DC5DC, D_800DC5E0, 0x40, 0x20, (u16*) PHYSICAL_TO_VIRTUAL(gPhysicalFramebuffers[temp_v0]),
                     (u16*) PHYSICAL_TO_VIRTUAL(gSegmentTable[5] + 0xF800));
    copy_framebuffer(D_800DC5DC + 0x40, D_800DC5E0, 0x40, 0x20,
                     (u16*) PHYSICAL_TO_VIRTUAL(gPhysicalFramebuffers[temp_v0]),
                     (u16*) PHYSICAL_TO_VIRTUAL(gSegmentTable[5] + 0x10800));
    copy_framebuffer(D_800DC5DC, D_800DC5E0 + 0x20, 0x40, 0x20,
                     (u16*) PHYSICAL_TO_VIRTUAL(gPhysicalFramebuffers[temp_v0]),
                     (u16*) PHYSICAL_TO_VIRTUAL(gSegmentTable[5] + 0x11800));
    copy_framebuffer(D_800DC5DC + 0x40, D_800DC5E0 + 0x20, 0x40, 0x20,
                     (u16*) PHYSICAL_TO_VIRTUAL(gPhysicalFramebuffers[temp_v0]),
                     (u16*) PHYSICAL_TO_VIRTUAL(gSegmentTable[5] + 0x12800));
    copy_framebuffer(D_800DC5DC, D_800DC5E0 + 0x40, 0x40, 0x20,
                     (u16*) PHYSICAL_TO_VIRTUAL(gPhysicalFramebuffers[temp_v0]),
                     (u16*) PHYSICAL_TO_VIRTUAL(gSegmentTable[5] + 0x13800));
    copy_framebuffer(D_800DC5DC + 0x40, D_800DC5E0 + 0x40, 0x40, 0x20,
                     (u16*) PHYSICAL_TO_VIRTUAL(gPhysicalFramebuffers[temp_v0]),
                     (u16*) PHYSICAL_TO_VIRTUAL(gSegmentTable[5] + 0x14800));
}
