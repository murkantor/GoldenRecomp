#include "patches.h"

/**
 * @recomp Weapon / guard-attack fixes (2026-07-12).
 *
 * Three fixes ported from theboy181's 60FPS mod set:
 *
 * 1. bondwalkItemGetAutomaticFiringRate: double the firing interval when the
 *    game runs at 60 (speedgraphframes == 1) so automatic weapons fire at the
 *    same real-time rate as on console, matching the 60FPS patch's behaviour
 *    in multiplayer. Also avoids handle_weapon_id_values_possibly_1st_person_animation
 *    hitting a break instruction (division by 0) when the rate is allowed to
 *    drop too low.
 *
 * 2. chrlvTickAttack: don't re-set TARGET_AIM_ONLY when a firing animation
 *    completes (the non-US versions already don't). Fixes guards behind
 *    crates locking into aim-only and never firing.
 *
 * 3. D_80030078[14] (standing weapon-fire anim table, anim 0x8698
 *    fire_kneel_dual_wield — Baron Samedi's freeze animation): vanilla data
 *    typo end_frame 11.0, which is before the shoot window (34..87), so the
 *    animation ends instantly and the character freezes. Set to 111.0 to
 *    match the same anim's entry in the crouched table (chr.c:915). Data
 *    can't be replaced through RECOMP_PATCH, so gunfixesApplyDataFixes()
 *    pokes the resident table once from the patched bossMainloop.
 */

// Mirror of the decomp's WeaponStats (lib/ge/src/game/gun.h:13), truncated
// after the field we need — offsets are load-bearing, keep in sync.
typedef struct WeaponStatsView {
    f32 MuzzleFlashExtension;      // 0x00
    f32 PosX;                      // 0x04
    f32 PosY;                      // 0x08
    f32 PosZ;                      // 0x0c
    f32 PlayX;                     // 0x10
    f32 PlayY;                     // 0x14
    f32 PlayZ;                     // 0x18
    s32 AmmoType;                  // 0x1c
    s16 MagSize;                   // 0x20
    u8 AutomaticFiringRate;        // 0x22
} WeaponStatsView;

// Mirror of the decomp's struct weapon_firing_animation_table
// (lib/ge/src/game/chr.h:59), sizeof 0x48 — offsets are load-bearing.
struct weapon_firing_animation_table {
    s32 anim;                      // 0x00 (anim id / ModelAnimation*)
    f32 unk04;                     // 0x04
    f32 turn_angle_per_frame;      // 0x08
    f32 angle_offset;              // 0x0c
    f32 start_frame;               // 0x10
    f32 end_frame;                 // 0x14
    f32 shoot_start_frame;         // 0x18
    f32 shoot_end_frame;           // 0x1c
    f32 recoil_start_frame;        // 0x20
    f32 recoil_end_frame;          // 0x24
    f32 aim_start_frame;           // 0x28
    f32 aim_end_frame;             // 0x2c
    f32 max_up;                    // 0x30
    f32 max_down;                  // 0x34
    f32 max_left;                  // 0x38
    f32 max_right;                 // 0x3c
    f32 free_arm_frac_up;          // 0x40
    f32 free_arm_frac_down;        // 0x44
};

// Partial mirror of the decomp's ChrRecord (lib/ge/src/bondtypes.h:2330) with
// only the act_attack fields chrlvTickAttack touches — offsets are
// load-bearing, keep in sync.
typedef struct ChrRecordView {
    u8 pad_00[0x1C];                                      // 0x00
    Model* model;                                         // 0x1c
    u8 pad_20[0x0C];                                      // 0x20
    struct {
        struct weapon_firing_animation_table* animfloats; // 0x2c
        s8 unk30;                                         // 0x30
        u8 pad_31[5];                                     // 0x31
        s8 unk36;                                         // 0x36
        s8 unk37;                                         // 0x37
        s8 unk38[2];                                      // 0x38
        u8 pad_3a[0x12];                                  // 0x3a
        u32 attacktype;                                   // 0x4c
        u32 entityid;                                     // 0x50
        u32 unk54;                                        // 0x54
        s32 type_of_motion;                               // 0x58
    } act_attack;
} ChrRecordView;

// lib/ge/src/bondaicommands.h:467
#define TARGET_AIM_ONLY 0x0020
#define TARGET_DONTTURN 0x0040

extern WeaponStatsView* get_ptr_item_statistics(ITEM_IDS item);
extern struct weapon_firing_animation_table D_80030078[];

extern f32 objecthandlerGetModelField28(Model* model);
extern f32 sub_GAME_7F06F5C4(Model* model);
extern ModelAnimation* objecthandlerGetModelAnim(Model* model);
extern f32 chrlvGetGuard007SpeedRating(ChrRecordView* self, f32 min, f32 max);
extern s32 chrlvUpdateAimendsideback(ChrRecordView* self, struct weapon_firing_animation_table* arg1, s32 arg2, s32 arg3, f32 arg4);
extern void chrlvResetAimend(ChrRecordView* self);
extern void chrlvTickAttackCommon(ChrRecordView* self);
extern void sub_GAME_7F025560(ChrRecordView* self, s32 attack_type, s32 arg2);
extern void sub_GAME_7F0256F0(ChrRecordView* self, s32 attack_type, s32 arg2);

extern void modelSetAnimation(Model* model, ModelAnimation* modelAnimation, s32 flip, f32 startframe, f32 speed, f32 merge);
extern void modelSetAnimEndFrame(Model* model, f32 endframe);

/**
 * Called once from the patched bossMainloop (workbench_theboy.c) after the
 * game segment is resident.
 */
void gunfixesApplyDataFixes(void) {
    // Baron Samedi freeze fix: vanilla end_frame 11.0 is a typo (before the
    // 34..87 shoot window); 111.0 matches the crouched table's 0x8698 entry.
    D_80030078[14].end_frame = 111.0f;
}

/**
 * Address 0x7F05DFCC
 */
RECOMP_PATCH s8 bondwalkItemGetAutomaticFiringRate(ITEM_IDS item) {
    // Double the returned value so weapons fire slower on 60 FPS emulator.
    // This is so this mod behaves like the 60FPS patch in multiplayer.

    // Allowing the weapons to shoot any faster than the normal rate (e.g. when the framerate is low)
    // will cause the function `handle_weapon_id_values_possibly_1st_person_animation` to likely
    // hit a break instruction and freeze the game. Likely because of a division by 0 because res will be 0.

    if (speedgraphframes == 1)
    {
        return (get_ptr_item_statistics(item)->AutomaticFiringRate * 2);
    }

    return (get_ptr_item_statistics(item)->AutomaticFiringRate);
}

/**
 * Address 0x7F02EBFC (VERSION_US).
 *
 * @recomp Identical to vanilla except the type_of_motion == 2 branch no
 * longer sets TARGET_AIM_ONLY (matches the non-US versions) — fixes crate
 * guards locking into aim-only.
 */
RECOMP_PATCH void chrlvTickAttack(ChrRecordView *self)
{
    Model *self_model;
    f32 temp_f0;
    f32 phi_f2;

    self_model = self->model;
    temp_f0 = objecthandlerGetModelField28(self_model);

    if (self->act_attack.type_of_motion)
    {
        if (self->act_attack.type_of_motion == 1)
        {
            if (self->act_attack.animfloats->recoil_end_frame >= 0.0f)
            {
                phi_f2 = self->act_attack.animfloats->recoil_end_frame;
            }
            else
            {
                phi_f2 = self->act_attack.animfloats->shoot_end_frame;
            }

            modelSetAnimation(
                self_model,
                objecthandlerGetModelAnim(self_model),
                (s32) self_model->gunhand,
                phi_f2,
                chrlvGetGuard007SpeedRating(self, 0.5f, 0.8f),
                16.0f);

            if (self->act_attack.animfloats->end_frame >= 0.0f)
            {
                modelSetAnimEndFrame(self_model, self->act_attack.animfloats->end_frame);
            }

            self->act_attack.type_of_motion = 2;
            chrlvResetAimend(self);

            return;
        }

        if (self->act_attack.type_of_motion == 2)
        {
            if (sub_GAME_7F06F5C4(self_model) <= temp_f0)
            {
                // don't set TARGET_AIM_ONLY, fixes crate guards
                self->act_attack.attacktype &= ~TARGET_DONTTURN;

                if (self->act_attack.unk54 != 0)
                {
                    sub_GAME_7F025560(self, (s32) self->act_attack.attacktype, self->act_attack.entityid);

                    return;
                }

                sub_GAME_7F0256F0(self, (s32) self->act_attack.attacktype, self->act_attack.entityid);

                return;
            }

            return;
        }
    }

    if ((self->act_attack.attacktype & TARGET_AIM_ONLY) != 0)
    {
        if ((self->act_attack.attacktype & TARGET_DONTTURN) != 0)
        {
            if (chrlvUpdateAimendsideback(self, self->act_attack.animfloats, (s32) self->act_attack.unk38[1], (s32) self->act_attack.unk38[0], 0.2f) == 0)
            {
                self->act_attack.type_of_motion = 1;
            }

            return;
        }

        if (sub_GAME_7F06F5C4(self_model) <= temp_f0)
        {
            self->act_attack.attacktype |= TARGET_DONTTURN;
            self->act_attack.unk30 = 2;

            return;
        }
    }

    if (self->act_attack.unk36 == 0)
    {
        if ((self->act_attack.animfloats->recoil_end_frame > 0.0f) && (temp_f0 <= self->act_attack.animfloats->recoil_end_frame))
        {
            if (sub_GAME_7F06F5C4(self_model) <= temp_f0)
            {
                modelSetAnimation(
                    self_model,
                    objecthandlerGetModelAnim(self_model),
                    (s32) self_model->gunhand,
                    self->act_attack.animfloats->recoil_end_frame,
                    chrlvGetGuard007SpeedRating(self, 0.5f, 0.8f),
                    16.0f);

                if (self->act_attack.unk37 != 0)
                {
                    if (self->act_attack.animfloats->end_frame >= 0.0f)
                    {
                        modelSetAnimEndFrame(self_model, self->act_attack.animfloats->end_frame);
                    }
                }
                else
                {
                    modelSetAnimEndFrame(self_model, self->act_attack.animfloats->shoot_end_frame);
                }
            }
        }
    }

    chrlvTickAttackCommon(self);
}
