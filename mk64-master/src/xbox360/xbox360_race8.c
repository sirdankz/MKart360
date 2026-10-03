#include <ultra64.h>
#include <string.h>
#include <stdio.h>
#include <common_structs.h>
#include <defines.h>
#include <course.h>
#include <objects.h>
#include <actor_types.h>
#include <bomb_kart.h>
#include "xbox360/race8.h"
#include "xbox360/online_hud.h"
#include "xbox360/race8_state.h"
#include "main.h"
#include "menus.h"
#include "camera.h"
#include "spawn_players.h"
#include "code_800029B0.h"
#include "code_80057C60.h"
#include "update_objects.h"
#include "render_objects.h"
#include "math_util_2.h"
#include "cpu_vehicles_camera_path.h"
#include "racing/race_logic.h"
#include "racing/skybox_and_splitscreen.h"
#include "audio/external.h"
#include "math_util.h"
#include "memory.h"

static Race8Results results;
static unsigned int resultsFrame;
static Player simulationPlayers[8];
static Object simulationObjects[OBJECT_LIST_SIZE];
static struct Actor simulationActors[ACTOR_LIST_SIZE];
static BombKart simulationBombs[NUM_BOMB_KARTS_MAX];
extern s16 gPlayerBalloonCount[];
extern u16 gRandomSeed16;
extern s32 gLapCountByPlayerId[];
extern s32 D_80164A28;
extern void x360_render_online_place(s32 playerId, s32 rank);

void x360_race8_prepare(void) {
    /* Native online setup bypasses the legacy menus that normally unblank VI. */
    osViBlack(0);
    gPlayerCount = x360_net_player_count();
    gPlayerCountSelection1 = gPlayerCount;
    gScreenModeSelection = SCREEN_MODE_1P;
    gActiveScreenMode = SCREEN_MODE_1P;
    gCurrentCourseId = x360_net8_course();
    gModeSelection = (gCurrentCourseId==COURSE_BLOCK_FORT || gCurrentCourseId==COURSE_SKYSCRAPER ||
        gCurrentCourseId==COURSE_DOUBLE_DECK || gCurrentCourseId==COURSE_BIG_DONUT) ? BATTLE : VERSUS;
    gCCSelection = x360_net8_cc();
    {int i;for(i=0;i<4;++i)gCharacterSelections[i]=(s8)x360_net8_character(i);}
    gDemoMode=0; gDemoUseController=0; gEnableDebugMode=0;
    gIsMirrorMode=0; gIsGamePaused=0;
    gGamestateNext=RACING;
    race8_results_init(&results,gPlayerCount);
    resultsFrame=0;
}

void x360_race8_service(void) {
    /* Race changes are requested by synchronized controls after the preceding frame. */
}

/* MK64_ONLINE_CAMERA_MAP_CONTROLS_V8 */
void x360_race8_controls(void) {
    int i;
    if(results.done) {
        if(x360_net_frame()-resultsFrame < 60) return;
        if(gControllers[0].buttonPressed & (A_BUTTON|B_BUTTON)) {
            if(gControllers[0].buttonPressed&B_BUTTON) x360_net8_configure();
            x360_race8_prepare();
            setup_race();
        }
        return;
    }
    if(gRaceState!=RACE_IN_PROGRESS) return;

    /* V8 per-player minimap visibility.
     * Native HUD input keys off the number of native screens, but race8's
     * network player count is not the number of local views. Keep one R-C
     * minimap bit for each logical online player instead. */
    for(i=0;i<x360_net_player_count();++i) {
        if(gControllers[i].buttonPressed & R_CBUTTONS) {
            D_80165800[i] = (D_80165800[i] + 1) & 1;
        }
    }
    /* MK64_R58_7_HOST_ONLY_PAUSE
     * P1 is the host in every netplay topology. Because controllers are
     * lockstep-synchronized, every console observes the same host START edge.
     * Guest START presses are intentionally ignored for pause/resume. */
    if(gControllers[0].buttonPressed&START_BUTTON) {
        gIsGamePaused=gIsGamePaused?0:1;
        func_800C9F90(gIsGamePaused?1:0);
    }
}

void x360_race8_spawn(void) {
    int i,n=x360_net_player_count();
    float x=0,y=0,z=0;
    if(gModeSelection==BATTLE) func_8000EEDC();
    else {
        init_course_path_point();
        x=(float)gTrackPaths[0][0].posX; y=(float)gTrackPaths[0][0].posY; z=(float)gTrackPaths[0][0].posZ;
        if(gCurrentCourseId==COURSE_TOADS_TURNPIKE) x=0;
    }
    for(i=0;i<8;++i) {
        float px=x+((i&1)?-20.0f:20.0f), pz=z+30.0f+20.0f*i, py=y, yaw=32768.0f;
        if(gModeSelection==BATTLE) {
            float radius=200.0f,offset=(i>=4)?45.0f:-45.0f;
            py=5;
            if(gCurrentCourseId==COURSE_SKYSCRAPER) {radius=400; py=480;}
            if(gCurrentCourseId==COURSE_DOUBLE_DECK) {radius=160; py=55;}
            if(gCurrentCourseId==COURSE_BIG_DONUT) {radius=575; py=200;}
            switch(i&3) {
                case 0:px=offset;pz=radius;yaw=32768;break;
                case 1:px=offset;pz=-radius;yaw=0;break;
                case 2:px=-radius;pz=offset;yaw=-16384;break;
                default:px=radius;pz=offset;yaw=16384;break;
            }
            /* Face along Big Donut's ring, as the original battle grid does. */
            if(gCurrentCourseId==COURSE_BIG_DONUT) {
                static const float ringYaw[4]={-16384,16384,0,32768};
                yaw=ringYaw[i&3];
            }
        }
        spawn_player(&gPlayers[i],(s8)i,px,pz,py,yaw,(s16)x360_net8_character(i),
            (u16)(i<n?(PLAYER_EXISTS|PLAYER_START_SEQUENCE|PLAYER_HUMAN):0));
    }
    D_80164A28=0;
    func_80039AE4();
    /* The extended spawn path bypasses func_8003C0F0's native epilogue.
     * Initialize every racer's path/lap/rank state on each race/restart, then
     * publish the matching rank-to-player lookup used by items and the HUD. */
    if (gModeSelection != BATTLE) {
        init_players();
        update_race_position_data();
    }
    race8_results_init(&results,n);
}

void x360_race8_cameras(void) {
    int i;

    /*
     * MK64_R28_CROSSPLAY_CAMERA_DETERMINISM
     *
     * OG MK64 updates camera 1, runs the first player/CPU batch, then updates
     * camera 2 (and camera 3/4 in the multiplayer branch).  R27 race8 updated
     * every network camera here before that CPU batch, so camera2+ were one
     * simulation step newer on Xbox 360.  CPU visibility consumes those camera
     * states, making the ordering gameplay-relevant.
     *
     * During active OG/360 crossplay multiplayer, update only camera 1 here.
     * main.c already performs the later camera2/3/4 updates in the original OG
     * order.  Xbox-360-only netplay keeps the existing all-camera update.
     */
    if (x360_net_crossplay() && gActiveScreenMode != SCREEN_MODE_1P) {
        func_8001EE98(&gPlayers[0], &cameras[0], 0);
        gCameraZoom[0] = cameras[0].unk_B4;
        return;
    }

    for(i=0;i<x360_net_player_count();++i) {
        func_8001EE98(&gPlayers[i],&cameras[i],(s8)i);
        gCameraZoom[i]=cameras[i].unk_B4;
    }
}

/* Project the existing sky animation from the selected camera without ticking
 * animation or RNG. Object changes are discarded with the render snapshot. */
static void race8_project_sky(int view) {
    int i;float field=gCameraZoom[view]+40.0f;
    int limit=(int)((field/2.0f)*DEGREES(1))+DEGREES(10);
    for(i=0;i<D_8018D1F0;++i){
        int index=D_8018CC80[view*D_8018D1F0+i];
        Object *o=&gObjectList[index];
        s16 angle=(s16)(cameras[view].rot[1]+o->direction_angle[1]);
        if(gCurrentCourseId==COURSE_FRAPPE_SNOWLAND){
            angle=(s16)(cameras[view].rot[1]-o->direction_angle[1]);
            o->offset[0]=160.0f+(1.7578125f/field)*angle;
            o->pos[0]=o->origin_pos[0]+o->offset[0];
        } else o->unk_09C=(s16)(160.0f+(1.7578125f/field)*angle);
        if(angle>=-limit && angle<=limit)o->status|=0x10;else o->status&=~0x10;
    }
}

/* Simulation always uses the same eight canonical cameras on every console.
 * Only display-list generation selects one full view or two horizontal views.
 * Separate persistent wrappers/matrix slots keep both views valid until the
 * graphics thread consumes the list, after the simulation pointers are restored. */

/*
 * MK64_RACE8_NATIVE_HUD_LAYOUT_V7
 *
 * race8 simulation deliberately stays SCREEN_MODE_1P even when a console
 * presents two local views. The stock end-of-frame HUD therefore uses P1's
 * layout/state and cannot represent a fullscreen P2/P3/P4 guest correctly.
 *
 * Draw the original MK64 HUD primitives from the actual local network slots.
 * Only position fields are replaced temporarily; timer/lap/item/rank state
 * remains the real player's state and is restored immediately after command
 * generation.
 */

static void x360_race8_layout_hud_1p(hud_player *hud) {
    /* MK64_ONLINE_GUEST_EFFECTS_RANK_HUD_V10: fullscreen online rank placement. */
    /* Stock settled 1P coordinates. */
    hud->itemBoxX = 160;
    hud->itemBoxY = 32;
    hud->slideItemBoxX = 0;
    hud->slideItemBoxY = 0;

    hud->rankX = 0x2E;
    hud->rankY = 0xC8;
    hud->slideRankX = 0;
    hud->slideRankY = 0;
    hud->rankScaling = 0.45f;

    hud->timerX = 0xE4;
    hud->timerY = 0x11;
    hud->lapCompletionTimeXs[0] = 0xE4;
    hud->lapCompletionTimeXs[1] = 0xE4;
    hud->lapCompletionTimeXs[2] = 0xE4;
    hud->totalTimeX = 0xE4;

    hud->lapX = 0x53;
    hud->lapAfterImage1X = 0x53;
    hud->lapAfterImage2X = 0x53;
    hud->lapY = 0x19;
}

static void x360_race8_layout_hud_2p_horizontal(hud_player *hud, int bottom) {
    /* Settled coordinates from MK64's native horizontal 2P HUD. */
    hud->itemBoxX = 45;
    hud->itemBoxY = bottom ? 143 : 34;
    hud->slideItemBoxX = 0;
    hud->slideItemBoxY = 0;

    hud->rankX = 0x34;
    hud->rankY = bottom ? 0xD2 : 0x62;
    hud->slideRankX = 0;
    hud->slideRankY = 0;
    hud->rankScaling = 0.5f;

    hud->timerX = 0xEA;
    hud->timerY = bottom ? 0x7F : 0x10;
    hud->lapCompletionTimeXs[0] = 0xEA;
    hud->lapCompletionTimeXs[1] = 0xEA;
    hud->lapCompletionTimeXs[2] = 0xEA;
    hud->totalTimeX = 0xEA;

    hud->lapX = 0x101;
    hud->lapAfterImage1X = 0x101;
    hud->lapAfterImage2X = 0x101;
    hud->lapY = bottom ? 0xDA : 0x6A;
}

static void x360_race8_draw_native_hud_slot(int slot, int bottom, int split) {
    hud_player savedHud;
    s32 savedMode;

    if (slot < 0 || slot >= x360_net_player_count()) return;

    savedHud = playerHUD[slot];
    savedMode = gModeSelection;

    if (split) {
        x360_race8_layout_hud_2p_horizontal(&playerHUD[slot], bottom);
    } else {
        x360_race8_layout_hud_1p(&playerHUD[slot]);
    }

    /* Native animated item window / roulette for this actual network slot. */
    draw_item_window(slot);

    if (savedMode != BATTLE) {
        /* Online uses VS rules, but the requested presentation includes TIME. */
        if (savedMode == VERSUS) gModeSelection = GRAND_PRIX;
        render_hud_timer(slot);
        gModeSelection = savedMode;

        draw_simplified_lap_count(slot);
        /*
         * MK64_ONLINE_SINGLE_VIEW_HUD_RANK_V11
         * gPlayers[].currentRank is synchronized gameplay state and is already
         * what the online debug HUD/audio paths use. Avoid stale native-screen
         * rank bookkeeping on fullscreen guests.
         */
        if (gCurrentCourseId != COURSE_YOSHI_VALLEY) {
            s32 rank = gPlayers[slot].currentRank;
            /* Network slots count humans, not the race field. GP ranks already
             * include CPUs, even when only two humans are connected. Preserve
             * the engine's rank in every online mode/local layout; clamp only
             * to the supported place-texture range (1st through 8th). */
            if (rank < 0) rank = 0;
            if (rank >= NUM_PLAYERS) rank = NUM_PLAYERS - 1;
            x360_render_online_place(slot, rank);
        }
    }

    playerHUD[slot] = savedHud;
    gModeSelection = savedMode;
}

void x360_race8_render_hud(void) {
    static struct UnkStruct_800DC5EC hudView;
    static Mtx hudProjection;
    int local;
    int locals;

    /*
     * MK64_ALL_ONLINE_HUD_AUDIO_PHASE_V9
     * Presentation is based on LOCAL view count, not total network players.
     * Plain 2-4P online with one local player must therefore use this same
     * corrected fullscreen HUD instead of MK64's native 2P/3P/4P HUD.
     */
    if (!x360_net_active() || gGamestate != RACING) return;
    if (gHUDDisable != 0 || gIsHUDVisible == 0) return;

    local = x360_net_local_slot();
    locals = x360_net_local_count();

    /*
     * Restore a full 320x240 viewport/scissor. The split HUD layouts below
     * already contain their absolute top/bottom Y positions.
     */
    if (local < 0 || local >= x360_net_player_count()) return;
    gDPNoOpTag(gDisplayListHead++, X360_ONLINE_HUD_BEGIN);
    /* Never reuse a world viewport or projection: queued commands retain
     * pointers to them, and legacy 2-4P modes do not use 320x240 here. */
    hudView.screenWidth = SCREEN_WIDTH;
    hudView.screenHeight = SCREEN_HEIGHT;
    hudView.screenStartX = SCREEN_WIDTH / 2;
    hudView.screenStartY = SCREEN_HEIGHT / 2;
    func_802A3730(&hudView);
    guOrtho(&hudProjection, 0.0f, 320.0f, 240.0f, 0.0f, -1.0f, 1.0f, 1.0f);
    func_80058BF4();
    gDPSetTexturePersp(gDisplayListHead++, G_TP_PERSP);
    gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&hudProjection),
              G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);

    if (locals > 1 && local + 1 < x360_net_player_count()) {
        x360_race8_draw_native_hud_slot(local, 0, 1);
        x360_race8_draw_native_hud_slot(local + 1, 1, 1);
    } else {
        x360_race8_draw_native_hud_slot(local, 0, 0);

        /* Fullscreen keeps the normal track minimap with every online racer. */
        if (gModeSelection != BATTLE && D_80165800[local] != 0) {
            func_8004EE54(0);
            render_mini_map_finish_line(0);
            func_8004F3E4(0);
        }
    }
    gDPNoOpTag(gDisplayListHead++, X360_ONLINE_HUD_END);
}

void x360_race8_render(void) {
    /* MK64_RACE8_LOCAL_SPLIT_RENDER_V3
     *
     * The network race keeps one shared 3-8 racer simulation, but presentation
     * is local to each Xbox. A console with two local racers needs two native
     * horizontal viewports. Do not turn the simulation itself into native 2P:
     * temporarily remap only the renderer's camera/view wrappers.
     *
     * Persistent wrappers are required because G_SPViewport commands keep a
     * pointer to the Vp data until the display list is executed later.
     */
    static struct UnkStruct_800DC5EC splitView[2];

    int local = x360_net_local_slot();
    int locals = x360_net_local_count();
    int players = x360_net_player_count();

    Camera savedCamera0 = cameras[0];
    Camera savedCamera1 = cameras[1];
    Camera localCamera0 = cameras[local];
    Camera localCamera1 = (locals > 1 && local + 1 < players) ? cameras[local + 1] : cameras[local];

    float savedZoom0 = gCameraZoom[0];
    float savedZoom1 = gCameraZoom[1];
    float localZoom0 = gCameraZoom[local];
    float localZoom1 = (locals > 1 && local + 1 < players) ? gCameraZoom[local + 1] : gCameraZoom[local];

    int savedLakitu0 = gIndexLakituList[0];
    int savedLakitu1 = gIndexLakituList[1];
    int localLakitu0 = gIndexLakituList[local];
    int localLakitu1 = (locals > 1 && local + 1 < players) ? gIndexLakituList[local + 1] : gIndexLakituList[local];

    struct UnkStruct_800DC5EC *savedView0 = D_800DC5EC;
    struct UnkStruct_800DC5EC *savedView1 = D_800DC5F0;
    Camera *savedHudCamera = D_8018CF14;
    s32 savedActiveScreenMode = gActiveScreenMode;
    s32 savedScreenModeSelection = gScreenModeSelection;
    Player *savedViewPlayer = D_800DC5EC->player;
    u16 savedNativeHud = D_800DC5B8;
    u16 seed = gRandomSeed16;

    memcpy(simulationPlayers, gPlayers, sizeof(simulationPlayers));
    memcpy(simulationObjects, gObjectList, sizeof(simulationObjects));
    memcpy(simulationActors, gActorList, sizeof(simulationActors));
    memcpy(simulationBombs, gBombKarts, sizeof(simulationBombs));

    /*
     * One local player: retain the proven fullscreen race8 path.
     * This also handles a defensive invalid two-local span without touching
     * networking or race state.
     */
    if (locals < 2 || local + 1 >= players) {
        cameras[0] = localCamera0;
        D_8018CF14 = &cameras[0];
        gIndexLakituList[0] = localLakitu0;
        gCameraZoom[0] = localZoom0;
        D_800DC5EC->player = &gPlayers[local];

        /* The stock HUD is bound to PLAYER_ONE. The frame-end online HUD
         * draws this console's actual local slot instead. */
        D_800DC5B8 = 0;
        /*
         * MK64_RACE8_LOCAL_SKY_CAMERA_V6
         * Native race update normally calls course_update_clouds() once per
         * presented camera. race8 deliberately skips that simulation-side
         * camera-dependent update, so do it here after camera[0] has been
         * canonicalized to this console's actual local network player.
         *
         * simulationObjects was snapshotted before this point and is restored
         * below, so cloud/star visibility and screen X positions stay purely
         * presentation-local and cannot affect lockstep state.
         */
        course_update_clouds(0);
        render_player_one_1p_screen();

        memcpy(gPlayers, simulationPlayers, sizeof(simulationPlayers));
        memcpy(gObjectList, simulationObjects, sizeof(simulationObjects));
        memcpy(gActorList, simulationActors, sizeof(simulationActors));
        memcpy(gBombKarts, simulationBombs, sizeof(simulationBombs));
        gRandomSeed16 = seed;

        cameras[0] = savedCamera0;
        cameras[1] = savedCamera1;
        gCameraZoom[0] = savedZoom0;
        gCameraZoom[1] = savedZoom1;
        gIndexLakituList[0] = savedLakitu0;
        gIndexLakituList[1] = savedLakitu1;
        D_8018CF14 = savedHudCamera;
        D_800DC5B8 = savedNativeHud;
        D_800DC5EC->player = savedViewPlayer;

        return;
    }

    /*
     * Build two persistent presentation wrappers. They are intentionally not
     * restored after command generation: display-list viewport commands point
     * at splitView[].viewport and consume those values later.
     */
    /* The horizontal render helpers do not clear the depth buffer. */
    move_segment_table_to_dmem();
    init_rdp();
    init_z_buffer();
    select_framebuffer();
    splitView[0] = *savedView0;
    splitView[1] = *savedView1;

    splitView[0].controllers = &gControllers[local];
    splitView[1].controllers = &gControllers[local + 1];
    splitView[0].camera = &cameras[0];
    splitView[1].camera = &cameras[1];
    splitView[0].player = &gPlayers[local];
    splitView[1].player = &gPlayers[local + 1];

    splitView[0].screenWidth = SCREEN_WIDTH;
    splitView[1].screenWidth = SCREEN_WIDTH;
    splitView[0].screenHeight = SCREEN_HEIGHT / 2;
    splitView[1].screenHeight = SCREEN_HEIGHT / 2;
    splitView[0].screenStartX = SCREEN_WIDTH / 2;
    splitView[1].screenStartX = SCREEN_WIDTH / 2;
    splitView[0].screenStartY = SCREEN_HEIGHT / 4;
    splitView[1].screenStartY = (SCREEN_HEIGHT * 3) / 4;

    D_800DC5EC = &splitView[0];
    D_800DC5F0 = &splitView[1];

    cameras[0] = localCamera0;
    cameras[1] = localCamera1;
    gCameraZoom[0] = localZoom0;
    gCameraZoom[1] = localZoom1;
    gIndexLakituList[0] = localLakitu0;
    gIndexLakituList[1] = localLakitu1;

    /*
     * The native render helpers must see horizontal 2P presentation so course
     * display-list selection, viewport setup and per-screen effects agree.
     * The real network player count remains untouched.
     */
    gActiveScreenMode = SCREEN_MODE_2P_SPLITSCREEN_HORIZONTAL;
    gScreenModeSelection = SCREEN_MODE_2P_SPLITSCREEN_HORIZONTAL;

    /*
     * The frame-end online HUD uses the actual local slots. Suppress the
     * legacy native HUD here so it does not duplicate or mislabel views.
     */
    D_800DC5B8 = 0;

    D_8018CF14 = &cameras[0];
    /*
     * MK64_RACE8_LOCAL_SKY_CAMERA_V6
     * Horizontal presentation view 0 uses camera1/cameras[0] and cloud set 0.
     */
    course_update_clouds(3);
    render_player_one_2p_screen_horizontal();

    /*
     * Legacy rendering changes player/object visibility and other shared
     * presentation bits. Bottom view must start from the exact same simulation
     * snapshot as top view.
     */
    memcpy(gPlayers, simulationPlayers, sizeof(simulationPlayers));
    memcpy(gObjectList, simulationObjects, sizeof(simulationObjects));
    memcpy(gActorList, simulationActors, sizeof(simulationActors));
    memcpy(gBombKarts, simulationBombs, sizeof(simulationBombs));
    gRandomSeed16 = seed;

    splitView[0].player = &gPlayers[local];
    splitView[1].player = &gPlayers[local + 1];
    D_8018CF14 = &cameras[1];
    /*
     * The shared object snapshot was restored after the top view. Recompute
     * only cloud/star presentation set 1 from camera2/cameras[1] for the
     * bottom local player before generating its display list.
     */
    course_update_clouds(4);
    render_player_two_2p_screen_horizontal();

    /* No render pass is allowed to leak into lockstep gameplay state. */
    memcpy(gPlayers, simulationPlayers, sizeof(simulationPlayers));
    memcpy(gObjectList, simulationObjects, sizeof(simulationObjects));
    memcpy(gActorList, simulationActors, sizeof(simulationActors));
    memcpy(gBombKarts, simulationBombs, sizeof(simulationBombs));
    gRandomSeed16 = seed;

    /* Restore simulation/presentation globals. splitView[] itself stays alive. */
    D_800DC5EC = savedView0;
    D_800DC5F0 = savedView1;
    cameras[0] = savedCamera0;
    cameras[1] = savedCamera1;
    gCameraZoom[0] = savedZoom0;
    gCameraZoom[1] = savedZoom1;
    gIndexLakituList[0] = savedLakitu0;
    gIndexLakituList[1] = savedLakitu1;
    D_8018CF14 = savedHudCamera;
    gActiveScreenMode = savedActiveScreenMode;
    gScreenModeSelection = savedScreenModeSelection;
    D_800DC5B8 = savedNativeHud;

}

int x360_race8_rules(void) {
    int i,j,n=results.players,battle=gModeSelection==BATTLE;
    int finished[8],number=0;
    if(gRaceState<RACE_IN_PROGRESS) return 0; /* Original staging and Lakitu countdown. */
    if(results.done) return 1;
    if(!battle) update_race_position_data();
    for(i=0;i<n;++i) {
        if(results.place[i]>=0) continue;
        if(battle) {
            if(gPlayerBalloonCount[i]<0) finished[number++]=i;
        } else {
            gPlayers[i].lapCount=gLapCountByPlayerId[i];
            if(gPlayers[i].lapCount>=2 && !results.finalLap[i]) {results.finalLap[i]=1;func_800CA49C((u8)i);}
            if(gPlayers[i].lapCount>=3) finished[number++]=i;
        }
    }
    /* Resolve same-tick finishes by interpolated crossing time, then slot for exact ties. */
    if(!battle) for(i=1;i<number;++i) {
        int slot=finished[i]; float t=func_8028EE8C(slot);
        for(j=i;j>0 && func_8028EE8C(finished[j-1])>t;--j) finished[j]=finished[j-1];
        finished[j]=slot;
    }
    for(i=0;i<number;++i) if(race8_finish(&results,finished[i],battle)) {
        int slot=finished[i];
        func_800CA118((u8)slot);
        /* Use the original automatic driving after a VS finish. */
        gPlayers[slot].type |= PLAYER_CPU | PLAYER_CINEMATIC_MODE;
        gPlayers[slot].currentItemCopy=0;
        if(battle) gPlayers[slot].type &= ~PLAYER_EXISTS;
    }
    if(results.count==n-1) {
        for(i=0;i<n;++i) if(results.place[i]<0) {
            func_800CA118((u8)i);
            gPlayers[i].type |= PLAYER_CPU | PLAYER_CINEMATIC_MODE;
            gPlayers[i].currentItemCopy=0;
        }
    }
    race8_complete_last(&results,battle);
    for(i=0;i<n;++i) if(results.place[i]>=0) gPlayers[i].currentRank=(s16)results.place[i];
    if(results.done) {
        gRaceState=RACE_DONE; gIsGamePaused=1; resultsFrame=x360_net_frame();
        gPlayerWinningIndex=results.order[0];
        for(i=0;i<n;++i) gPlayers[i].type |= PLAYER_START_SEQUENCE;
    }
    return 1;
}

int x360_race8_hud_line_for_view(int view,int row,char *out,int size) {
    static const char *names[]={"MARIO","LUIGI","YOSHI","TOAD","DK","WARIO","PEACH","BOWSER"};
    int local=x360_net_local_slot()+view;
    if (!out || size < 1) return 0;
    out[0]=0;
    if(!x360_net8_active() || gGamestate!=RACING || view<0 ||
       view>=x360_net_local_count() || local<0 || local>=results.players) return 0;

    /* Native graphics own item/lap/time/place. Text is only for status/results. */
    if(row==0) {
        if(results.done)
            _snprintf(out,size,"RESULTADOS - HOST A: JOGAR NOVAMENTE   B: TROCAR PISTA");
        else if(gIsGamePaused)
            _snprintf(out,size,"PAUSADO PELO HOST - START DO HOST PARA CONTINUAR");
        else if(results.place[local]>=0)
            _snprintf(out,size,"FINALIZOU - AGUARDANDO OS OUTROS CORREDORES");
        else if(gModeSelection==BATTLE)
            _snprintf(out,size,"BALOES %d",gPlayerBalloonCount[local]+1);
    } else if(results.done && row>=1 && row<1+results.players) {
        int slot=results.order[row-1];
        _snprintf(out,size,"%d. P%d  %s",row,slot+1,names[gPlayers[slot].characterId&7]);
    }
    out[size-1]=0;
    return out[0]!=0;
}

int x360_race8_hud_line(int row,char *out,int size) {
    return x360_race8_hud_line_for_view(0,row,out,size);
}

unsigned int x360_race8_hash(unsigned int h) {
    int i;
    h=(h^(unsigned int)gIsGamePaused)*16777619U;
    h=(h^(unsigned int)gRaceState)*16777619U;
    h=(h^(unsigned int)results.count)*16777619U;
    for(i=0;i<results.players;++i) {
        h=(h^(unsigned int)results.place[i])*16777619U;
        h=(h^(unsigned int)gPlayers[i].currentItemCopy)*16777619U;
        h=(h^(unsigned int)gLapCountByPlayerId[i])*16777619U;
        h=(h^(unsigned int)gPlayerBalloonCount[i])*16777619U;
    }
    return h;
}

