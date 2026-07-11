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

RECOMP_PATCH void bgLoadFromDynamicCCRMLUT(Gfx* start, Gfx* end, s32 lutIndex) {
    Gfx* curGfx;
    Gfx* lutPair;

    // @recomp: a room that never loaded its primary GDL hands us K0(NULL)
    // (or worse) here. Nothing valid lives at the bottom of RDRAM; walking
    // it is at best a junk scan, at worst a host AV. Bail.
    if ((u32) start <= BG_K0_RAM_START || (u32) start >= BG_K0_RAM_END) {
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
            return;
        }
    }
}
