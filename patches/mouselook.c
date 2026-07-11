#include "patches.h"

/**
 * @recomp Mouse look (2026-07-11).
 *
 * 1964-mouse-injector style: host mouse deltas are applied directly to the
 * view angles, bypassing the stick acceleration/smoothing pipeline entirely.
 * The hook is bondviewApplyVertaTheta — the small leaf the game calls
 * whenever the view angles are finalized — patched to inject the deltas
 * FIRST and then run the vanilla wrap (+-180), pitch clamp (+-90), and
 * sin/cos + transform derivations, so the mouse inherits every limit the
 * stick has.
 *
 * The deltas arrive pre-scaled by the Mouse Sensitivity option (General tab
 * in the config menu; 50 = 1.0x) via recomp_get_mouse_deltas, and are zero
 * when the option is 0 — the slider is both the tuning and the on/off
 * switch, and it's the same option recompinput uses to decide cursor
 * capture. Injection is gated to normal first-person gameplay
 * (CAMERAMODE_NONE — no cutscenes/intro cams/fades) and to player 0, who
 * owns the mouse. recomp_get_mouse_deltas clears on read; this is its only
 * game-side caller.
 */

// View degrees per (sensitivity-scaled) mouse count — the baseline feel
// under the sensitivity slider. Flip the signs in the code below if a look
// direction feels inverted.
#define MOUSELOOK_DEG_PER_COUNT 0.25f

#define MOUSELOOK_DEG_TO_RAD 0.017453292519943295f

// libultra's internal implementations — used instead of cosf/sinf because
// clang under -ffast-math fuses adjacent sinf+cosf pairs into sincosf,
// which the game does not export.
f32 __cosf(f32);
f32 __sinf(f32);

RECOMP_PATCH void bondviewApplyVertaTheta(void) {
    // @recomp: mouse injection (see header comment). Everything below it is
    // the vanilla body (bondview.c:8077).
    // TEMP diagnostic (mouse-look bring-up): once a second, show that this
    // patch runs and what the gate/deltas look like. Remove when confirmed.
    {
        static u32 diag_ticks = 0;
        if ((diag_ticks++ & 63) == 0) {
            recomp_printf("MOUSEDIAG patch: cam=%d player=%d\n", (s32) g_CameraMode, get_cur_playernum());
        }
    }
    // CAMERAMODE_FP is normal first-person play (diagnosed empirically:
    // cam=4 during gameplay, 1/3 during intro cams). FP_NOINPUT stays
    // excluded — the game is explicit about input being off there.
    // Aim mode (R held, insightaimmode) hands the mouse to the STICK path
    // instead: the host converts deltas to stick input, which is exactly
    // what drives the aim crosshair. Everywhere else (menus, cutscenes) the
    // stick mode default lets the mouse navigate.
    if (((g_CameraMode == CAMERAMODE_FP) || (g_CameraMode == CAMERAMODE_MP)) && (get_cur_playernum() == 0) &&
        !g_CurrentPlayer->insightaimmode) {
        f32 mouse_dx;
        f32 mouse_dy;
        recomp_set_mouse_mode(1); // look
        recomp_get_mouse_deltas(&mouse_dx, &mouse_dy);
        if (mouse_dx != 0.0f || mouse_dy != 0.0f) {
            g_CurrentPlayer->vv_theta += mouse_dx * MOUSELOOK_DEG_PER_COUNT;
            while (g_CurrentPlayer->vv_theta >= 360.0f) {
                g_CurrentPlayer->vv_theta -= 360.0f;
            }
            while (g_CurrentPlayer->vv_theta < 0.0f) {
                g_CurrentPlayer->vv_theta += 360.0f;
            }
            // Mouse forward (negative SDL y) looks up.
            g_CurrentPlayer->vv_verta -= mouse_dy * MOUSELOOK_DEG_PER_COUNT;
        }
    }
    else if (get_cur_playernum() == 0) {
        // Aim mode or non-FP camera: the mouse drives the stick.
        recomp_set_mouse_mode(2);
    }

    while (g_CurrentPlayer->vv_verta < -180.0f) {
        g_CurrentPlayer->vv_verta += 360.0f;
    }

    while (g_CurrentPlayer->vv_verta >= 180.0f) {
        g_CurrentPlayer->vv_verta -= 360.0f;
    }

    if (g_CurrentPlayer->vv_verta > 90.0f) {
        g_CurrentPlayer->vv_verta = 90.0f;
    }
    else if (g_CurrentPlayer->vv_verta < -90.0f) {
        g_CurrentPlayer->vv_verta = -90.0f;
    }

    g_CurrentPlayer->vv_costheta = __cosf(g_CurrentPlayer->vv_theta * MOUSELOOK_DEG_TO_RAD);
    g_CurrentPlayer->vv_sintheta = __sinf(g_CurrentPlayer->vv_theta * MOUSELOOK_DEG_TO_RAD);

    g_CurrentPlayer->vv_verta360 = g_CurrentPlayer->vv_verta;
    if (g_CurrentPlayer->vv_verta360 < 0.0f) {
        g_CurrentPlayer->vv_verta360 += 360.0f;
    }

    g_CurrentPlayer->vv_cosverta = __cosf(g_CurrentPlayer->vv_verta360 * MOUSELOOK_DEG_TO_RAD);
    g_CurrentPlayer->vv_sinverta = __sinf(g_CurrentPlayer->vv_verta360 * MOUSELOOK_DEG_TO_RAD);

    g_CurrentPlayer->field_488.theta_transform.f[0] = -g_CurrentPlayer->vv_sintheta;
    g_CurrentPlayer->field_488.theta_transform.f[1] = 0;
    g_CurrentPlayer->field_488.theta_transform.f[2] = g_CurrentPlayer->vv_costheta;
}
