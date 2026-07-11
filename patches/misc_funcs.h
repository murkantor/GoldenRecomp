#ifndef __RECOMP_FUNCS_H__
#define __RECOMP_FUNCS_H__

#include "patch_helpers.h"

DECLARE_FUNC(void, yield_self_1ms, void);
DECLARE_FUNC(void, recomp_load_overlays, u32 rom, void* ram, u32 size);
DECLARE_FUNC(void, recomp_puts, const char* data, u32 size);
DECLARE_FUNC(void, recomp_exit);
DECLARE_FUNC(void, recomp_handle_quicksave_actions, OSMesgQueue* enter_mq, OSMesgQueue* exit_mq);
DECLARE_FUNC(void, recomp_handle_quicksave_actions_main, OSMesgQueue* enter_mq, OSMesgQueue* exit_mq);
DECLARE_FUNC(u16, recomp_get_pending_warp);
DECLARE_FUNC(u32, recomp_get_pending_set_time);
DECLARE_FUNC(s32, recomp_autosave_enabled);
DECLARE_FUNC(void, recomp_load_overlays, u32 rom, void* ram, u32 size);
DECLARE_FUNC(s32, osPiStartDma_recomp, OSIoMesg* mb, s32 priority, s32 direction, uintptr_t devAddr, void* vAddr,
             size_t nbytes, OSMesgQueue* mq);
DECLARE_FUNC(s32, osPfsInit_recomp, OSMesgQueue*, OSPfs*, int);
DECLARE_FUNC(s32, osMotorInit_recomp, OSMesgQueue*, OSPfs*, int);
// Latched debug F-key presses from the host keyboard: bit n = F(n+1)
// (F1 = bit 0). F10/F11 never latch (UI reload / fullscreen). Reading
// clears the latch, so poll from ONE place per frame.
DECLARE_FUNC(u32, recomp_get_debug_keys);
// Host mouse deltas since the last read (pixels, pre-scaled by the Mouse
// Sensitivity option; both zero when mouse look is disabled). Clears on
// read - poll from ONE place per tick.
DECLARE_FUNC(void, recomp_get_mouse_deltas, f32* x, f32* y);
// Tells the host what the mouse currently controls: 0 = off, 1 = look
// (deltas consumed by the mouselook patch), 2 = stick (host converts the
// deltas into N64 stick input - aim-mode crosshair and menu navigation).
// Call once per tick; the host decays to stick mode if the game stops
// reporting (e.g. front menus where the bond pipeline is not ticking).
DECLARE_FUNC(void, recomp_set_mouse_mode, u32 mode);
#endif
