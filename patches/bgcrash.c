#include "patches.h"

/**
 * @recomp Start-Game crash fix (2026-07-11).
 *
 * bgLoadRoomModelData runs a fog CC/RM LUT pass over each freshly loaded
 * room's display lists (bg.c:3968). When a room reaches that pass without its
 * primary GDL actually loaded (csize 0 or bgLoadRoomPrimaryGdl bailing), the
 * walk starts from a garbage ptr_expanded_mapping_info — observed in crash
 * dumps as exactly 0x80000000, i.e. K0(NULL), with the sentinel-terminated
 * byte scan then reading from it (host fault at rdram+0x80000003: the +3 is
 * the recomp's byte-swap XOR, which is how we know it was the s8 G_ENDDL
 * sentinel read in the end==NULL branch).
 *
 * On the N64 that walk read (and could even patch!) low RDRAM junk and got
 * away with it; the recompiled build takes an access violation instead,
 * because the raw struct-field value bypasses the address translation the
 * recompiler applies to well-formed pointers. It surfaced as the "flaky
 * Start-Game crash" — flaky because whether the junk read faults depends on
 * the patch binary layout, so it came and went across builds.
 *
 * Fix: re-implement the walker (vanilla logic, bgapply.c:77
 * bgApplyDynamicCCRMLUT — dump.toml names it bgLoadFromDynamicCCRMLUT) with
 * two guards that cannot change behaviour for any valid input:
 *   - refuse a start pointer at/below K0(NULL) or beyond RDRAM;
 *   - hard-stop the walk at the end of RDRAM (wild size / missing sentinel).
 * The vanilla debug replacement counter (a static, telemetry only) is
 * dropped.
 */

extern Gfx* ptrDynamic_CC_RM_LUT[];

#define BG_K0_RAM_START 0x80000000u
#define BG_K0_RAM_END 0x80800000u

// Mirror of the decomp's s_room_info (lib/ge/src/game/bg.h:44) — offsets are
// load-bearing, keep in sync.
typedef struct s_room_info {
    u8 room_rendered;                           // 0x00
    u8 room_neighbor_to_rendered;               // 0x01
    u8 model_bin_loaded;                        // 0x02 (0 = unloaded)
    u8 portal_visit_count;                      // 0x03
    Vtx* vertices;                              // 0x04
    void* ptr_expanded_mapping_info;            // 0x08
    void* ptr_secondary_expanded_mapping_info;  // 0x0c
    s32 csize_point_index_binary;               // 0x10
    s32 csize_primary_DL_binary;                // 0x14
    s32 csize_secondary_DL_binary;              // 0x18
    s32 usize_point_index_binary;               // 0x1c
    s32 usize_primary_DL_binary;                // 0x20
    s32 usize_secondary_DL_binary;              // 0x24
    s32 cur_room_totalsize;                     // 0x28
    void* vtx_batch_bounds;                     // 0x2c
    s16 num_vtx_batch_bounds;                   // 0x30
    s16 field_32;                               // 0x32
    u8 room_loaded_mask;                        // 0x34
    u8 field_35;                                // 0x35
    s16 field_36;                               // 0x36
    coord3d minbounds;                          // 0x38
    coord3d maxbounds;                          // 0x44
} s_room_info;

extern s_room_info g_BgRoomInfo[];
extern s32 g_MaxNumRooms;
extern s32 g_FogSkyIsEnabled;

void* memaAlloc(u32 amount);
void memaFree(void* addr, s32 size);
s32 memaRealloc(s32 addr, u32 oldsize, u32 newsize);
s32 memaGetLongestFree(void);
s32 get_debug_joy2detailedit_flag(void);
void redarken_lights_in_room(s32 room);
void roomsHandleStateDebugging(void);
s32 sub_GAME_7F0B5FAC(s32 roomnum, u8* dst, s32 len);       // bgLoadRoomVtxData
s32 sub_GAME_7F0B609C(s32 roomnum, u8* dst, s32 allocsize); // bgLoadRoomPrimaryGdl
s32 sub_GAME_7F0B61DC(s32 roomnum, u8* dst, s32 allocsize); // bgLoadRoomSecondaryGdl
void sub_GAME_7F0B6994(s32 roomID);                         // bgBuildRoomVtxBounds

void bgLoadFromDynamicCCRMLUT(Gfx* start, Gfx* end, s32 lutIndex);

RECOMP_PATCH void bgLoadFromDynamicCCRMLUT(Gfx* start, Gfx* end, s32 lutIndex) {
    Gfx* curGfx;
    Gfx* lutPair;

    // @recomp: a room that never loaded its primary GDL hands us K0(NULL)
    // (or worse) here. Nothing valid lives at the bottom of RDRAM; walking
    // it is at best a junk scan, at worst a host AV. Bail.
    if ((u32) start <= BG_K0_RAM_START || (u32) start >= BG_K0_RAM_END) {
        // @recomp TEMP diagnostic (2026-07-11 flicker hunt): a fire here
        // means a room reached the fog-LUT pass without its model data
        // loaded — the condition that used to crash, now surviving silently.
        recomp_printf("BGLUT guard fired: start=%08x end=%08x lut=%d (room GDL never loaded?)\n",
                      (u32) start, (u32) end, lutIndex);
        return;
    }

    curGfx = start;

    while (((end != NULL) && (curGfx < end)) || ((end == NULL) && (((s8*) curGfx)[0] != (s8) G_ENDDL))) {
        for (lutPair = ptrDynamic_CC_RM_LUT[lutIndex]; lutPair->words.w0 != 0; lutPair += 2) {
            if ((lutPair->words.w0 == curGfx->words.w0) && (lutPair->words.w1 == curGfx->words.w1)) {
                *curGfx = *(lutPair + 1);
            }
        }

        curGfx++;

        // @recomp: garbage end pointer or missing G_ENDDL sentinel — stop at
        // the end of RDRAM instead of walking into unmapped space.
        if ((u32) curGfx >= BG_K0_RAM_END) {
            // @recomp TEMP diagnostic (2026-07-11 flicker hunt).
            recomp_printf("BGLUT guard: walk ran off RDRAM end (start=%08x lut=%d)\n",
                          (u32) start, lutIndex);
            return;
        }
    }
}

/**
 * @recomp Room-load robustness (2026-07-11): the REAL fix for the crash
 * family the LUT guard above only deflected.
 *
 * Vanilla bgLoadRoomModelData (bg.c:3868) tolerates its sub-loads failing:
 * bgLoadRoomVtxData / bgLoadRoomPrimaryGdl return -1 when the memaAlloc'd
 * block is too small (memory pressure / fragmentation — the first load of a
 * room sizes the block from memaGetLongestFree), and vanilla just carries on:
 * it marks the room LOADED anyway and runs the fog CC/RM LUT pass and
 * bgBuildRoomVtxBounds over whatever stale ptr_expanded_mapping_info /
 * vertices the room struct held (K0(NULL) on a first load). On the N64 those
 * junk walks usually got away with it; the recomp takes a host AV — first in
 * the LUT walk (guarded above since ebc770e), then, one call later, in
 * bgBuildRoomVtxBounds' identical G_ENDDL scan (crash observed 2026-07-11 at
 * sub_GAME_7F0B6994+0x18b, deterministic). Rooms that "survived" this way
 * also render without their fog LUT replacements — bright, unshaded, and
 * popping in and out as loads are retried after eviction: the Depot/Frigate
 * flicker report.
 *
 * Fix: when any sub-load fails, free the block and put the room back to
 * "unloaded" so the visibility pass retries when memory frees up — never mark
 * garbage as loaded, never walk it. Tail passes additionally refuse a
 * ptr_expanded_mapping_info outside RDRAM (the csize==0 edge case vanilla
 * would also have junk-walked).
 */
RECOMP_PATCH void sub_GAME_7F0B6368(s32 roomID) { // bgLoadRoomModelData
    s32 allocsize;
    s32 used;
    s32 result;
    u8* data;
    s_room_info* room;

    used = 0;

    if (roomID >= g_MaxNumRooms) {
        return;
    }
    room = &g_BgRoomInfo[roomID];
    if (room->model_bin_loaded) {
        return;
    }

    allocsize = room->cur_room_totalsize;
    if (allocsize > 0) {
        if (get_debug_joy2detailedit_flag()) {
            allocsize += 0x400;
        }
    } else {
        // First load: vanilla grabs the largest free block and shrinks later.
        allocsize = memaGetLongestFree();
    }

    data = memaAlloc(allocsize);
    if (data == NULL) {
        return;
    }

    if (room->csize_point_index_binary) {
        result = sub_GAME_7F0B5FAC(roomID, data, allocsize);
        if (result < 0) {
            goto fail;
        }
        used = result;
        redarken_lights_in_room(roomID);
    } else {
        room->vertices = NULL;
        room->usize_point_index_binary = 0;
    }

    if (room->csize_primary_DL_binary) {
        result = sub_GAME_7F0B609C(roomID, data + used, allocsize - used);
        if (result < 0) {
            goto fail;
        }
        used += result;
    }

    if (room->csize_secondary_DL_binary) {
        result = sub_GAME_7F0B61DC(roomID, data + used, allocsize - used);
        if (result < 0) {
            goto fail;
        }
        if (result > 0) {
            used += result;
        }
    } else {
        room->ptr_secondary_expanded_mapping_info = NULL;
    }

    room->cur_room_totalsize = ((used + 0x20) & ~0xf);
    room->model_bin_loaded = 1;

    if (allocsize != ((used + 0x20) & ~0xf)) {
        memaRealloc((s32) data, allocsize, ((used + 0x20) & ~0xf));
    }

    // Tail passes only over a sane primary GDL pointer (see header comment).
    if ((u32) room->ptr_expanded_mapping_info > BG_K0_RAM_START &&
        (u32) room->ptr_expanded_mapping_info < BG_K0_RAM_END) {
        if (g_FogSkyIsEnabled) {
            bgLoadFromDynamicCCRMLUT(room->ptr_expanded_mapping_info,
                                     (Gfx*) ((u8*) room->ptr_expanded_mapping_info + room->usize_primary_DL_binary),
                                     1);
            if (room->ptr_secondary_expanded_mapping_info) {
                bgLoadFromDynamicCCRMLUT(
                    room->ptr_secondary_expanded_mapping_info,
                    (Gfx*) ((u8*) room->ptr_secondary_expanded_mapping_info + room->usize_secondary_DL_binary), 5);
            }
        } else {
            bgLoadFromDynamicCCRMLUT(room->ptr_expanded_mapping_info,
                                     (Gfx*) ((u8*) room->ptr_expanded_mapping_info + room->usize_primary_DL_binary),
                                     6);
            if (room->ptr_secondary_expanded_mapping_info) {
                bgLoadFromDynamicCCRMLUT(
                    room->ptr_secondary_expanded_mapping_info,
                    (Gfx*) ((u8*) room->ptr_secondary_expanded_mapping_info + room->usize_secondary_DL_binary), 7);
            }
        }

        sub_GAME_7F0B6994(roomID); // bgBuildRoomVtxBounds
    } else {
        recomp_printf("ROOMLOAD: room %d loaded but primary GDL ptr %08x out of range - tail passes skipped\n",
                      roomID, (u32) room->ptr_expanded_mapping_info);
    }

    roomsHandleStateDebugging();
    return;

fail:
    // @recomp TEMP diagnostic (2026-07-11 flicker hunt): every line here is a
    // room-load failure the vanilla game would have papered over.
    recomp_printf("ROOMLOAD fail: room %d alloc=%d used=%d csize v/p/s=%d/%d/%d longestFree=%d\n",
                  roomID, allocsize, used, room->csize_point_index_binary, room->csize_primary_DL_binary,
                  room->csize_secondary_DL_binary, memaGetLongestFree());
    memaFree(data, allocsize);
    room->vertices = NULL;
    room->ptr_expanded_mapping_info = NULL;
    room->ptr_secondary_expanded_mapping_info = NULL;
    room->model_bin_loaded = 0;
    room->cur_room_totalsize = 0;
}
