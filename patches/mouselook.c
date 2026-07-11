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
        if (((g_CameraMode == CAMERAMODE_FP) || (g_CameraMode == CAMERAMODE_MP)) && g_CurrentPlayer->insightaimmode) {
            // Aim mode: the crosshair patch (below) consumes the deltas
            // directly — keep the host's stick feed out of it.
            recomp_set_mouse_mode(0);
        }
        else {
            // Non-FP camera (menus etc.): the mouse drives the stick.
            recomp_set_mouse_mode(2);
        }
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


/**
 * @recomp Mouse aim (aim mode / R-held crosshair).
 *
 * Vanilla treats the crosshair and the gun azimuth as DAMPED ACCUMULATORS
 * of the per-tick turn inputs: pos = pos * damp + turn. With a mouse that
 * means the crosshair springs back to center the moment the mouse stops,
 * and the gun azimuth lags the inputs - the observed sway and inaccuracy.
 *
 * In mouse aim this patch instead integrates the deltas into the crosshair
 * as a STABLE position (no decay), and slaves the gun azimuth exactly to
 * the crosshair (matching the two accumulators' output domains), so the gun
 * points precisely where the crosshair is: no recentering, no sway, no lag
 * inaccuracy. Everything downstream - screen clamps, angle derivation, the
 * 3D transform into the gun hands - is the vanilla body (gun.c:14327).
 */

// Crosshair screen-fraction per (sensitivity-scaled) mouse count in aim
// mode: 0.002 = ~500 counts across the half-screen.
#define MOUSEAIM_POS_PER_COUNT 0.002f

void transformAndNormalizeByLength2Dto3D(coord2d* screen, coord3d* out, f32 length);
void sub_GAME_7F067AB4(coord3d* param_1);

RECOMP_PATCH void caclulate_gun_crosshair_position_rotation(f32 turn_x, f32 turn_y, f32 guncrossdamp, f32 gunaimdamp) {
    s32 i;
    f32 screen_width;
    f32 screen_height;
    coord3d coords;
    s32 mouse_aim;

    screen_width = getPlayer_c_screenwidth();
    screen_height = getPlayer_c_screenheight();

    mouse_aim = ((g_CameraMode == CAMERAMODE_FP) || (g_CameraMode == CAMERAMODE_MP)) &&
                (get_cur_playernum() == 0) && g_CurrentPlayer->insightaimmode;

    if (guncrossdamp != g_CurrentPlayer->guncrossdamp) {
        g_CurrentPlayer->crosshair_x_pos = (g_CurrentPlayer->crosshair_x_pos * (1.0f - g_CurrentPlayer->guncrossdamp)) / (1.0f - guncrossdamp);
        g_CurrentPlayer->crosshair_y_pos = (g_CurrentPlayer->crosshair_y_pos * (1.0f - g_CurrentPlayer->guncrossdamp)) / (1.0f - guncrossdamp);
        g_CurrentPlayer->guncrossdamp = guncrossdamp;
    }

    if (gunaimdamp != g_CurrentPlayer->gunaimdamp) {
        g_CurrentPlayer->gun_azimuth_angle = (g_CurrentPlayer->gun_azimuth_angle * (1.0f - g_CurrentPlayer->gunaimdamp)) / (1.0f - gunaimdamp);
        g_CurrentPlayer->gun_azimuth_turning = (g_CurrentPlayer->gun_azimuth_turning * (1.0f - g_CurrentPlayer->gunaimdamp)) / (1.0f - gunaimdamp);
        g_CurrentPlayer->gunaimdamp = gunaimdamp;
    }

    if (mouse_aim) {
        // Stable position: integrate deltas in the accumulator's own domain
        // (pos * (1 - damp) is the screen fraction), clamped to the screen.
        f32 mouse_dx;
        f32 mouse_dy;
        f32 pos_limit = 1.0f / (1.0f - guncrossdamp);
        recomp_get_mouse_deltas(&mouse_dx, &mouse_dy);
        g_CurrentPlayer->crosshair_x_pos += (mouse_dx * MOUSEAIM_POS_PER_COUNT) / (1.0f - guncrossdamp);
        g_CurrentPlayer->crosshair_y_pos += (mouse_dy * MOUSEAIM_POS_PER_COUNT) / (1.0f - guncrossdamp);
        if (g_CurrentPlayer->crosshair_x_pos > pos_limit) g_CurrentPlayer->crosshair_x_pos = pos_limit;
        if (g_CurrentPlayer->crosshair_x_pos < -pos_limit) g_CurrentPlayer->crosshair_x_pos = -pos_limit;
        if (g_CurrentPlayer->crosshair_y_pos > pos_limit) g_CurrentPlayer->crosshair_y_pos = pos_limit;
        if (g_CurrentPlayer->crosshair_y_pos < -pos_limit) g_CurrentPlayer->crosshair_y_pos = -pos_limit;
    }
    else {
        for (i = 0; i < g_ClockTimer; i++) {
            g_CurrentPlayer->crosshair_x_pos = (g_CurrentPlayer->crosshair_x_pos * guncrossdamp) + turn_x;
            g_CurrentPlayer->crosshair_y_pos = (g_CurrentPlayer->crosshair_y_pos * guncrossdamp) + turn_y;
        }
    }

    g_CurrentPlayer->crosshair_angle.f[0] = (g_CurrentPlayer->crosshair_x_pos * (1.0f - guncrossdamp) * screen_width * 0.5f) + (screen_width * 0.5f);
    g_CurrentPlayer->crosshair_angle.f[1] = (g_CurrentPlayer->crosshair_y_pos * (1.0f - guncrossdamp) * screen_height * 0.5f) + (screen_height * 0.5f);

    if (g_CurrentPlayer->crosshair_angle.f[0] < 3.0f) {
        g_CurrentPlayer->crosshair_angle.f[0] = 3.0f;
    }
    else if ((screen_width - 4.0f) < g_CurrentPlayer->crosshair_angle.f[0]) {
        g_CurrentPlayer->crosshair_angle.f[0] = screen_width - 4.0f;
    }

    if (g_CurrentPlayer->crosshair_angle.f[1] < 3.0f) {
        g_CurrentPlayer->crosshair_angle.f[1] = 3.0f;
    }
    else if ((screen_height - 4.0f) < g_CurrentPlayer->crosshair_angle.f[1]) {
        g_CurrentPlayer->crosshair_angle.f[1] = (screen_height - 4.0f);
    }

    g_CurrentPlayer->crosshair_angle.f[0] += getPlayer_c_screenleft();
    g_CurrentPlayer->crosshair_angle.f[1] += getPlayer_c_screentop();

    if (mouse_aim) {
        // The gun tracks the crosshair exactly: match the two accumulators'
        // output domains (each contributes pos * (1 - damp) of the screen
        // fraction). No input-driven lag, no sway.
        g_CurrentPlayer->gun_azimuth_angle = (g_CurrentPlayer->crosshair_x_pos * (1.0f - guncrossdamp)) / (1.0f - gunaimdamp);
        g_CurrentPlayer->gun_azimuth_turning = (g_CurrentPlayer->crosshair_y_pos * (1.0f - guncrossdamp)) / (1.0f - gunaimdamp);
    }
    else {
        for (i = 0; i < g_ClockTimer; i++) {
            g_CurrentPlayer->gun_azimuth_angle = (g_CurrentPlayer->gun_azimuth_angle * gunaimdamp) + turn_x;
            g_CurrentPlayer->gun_azimuth_turning = (g_CurrentPlayer->gun_azimuth_turning * gunaimdamp) + turn_y;
        }
    }

    g_CurrentPlayer->field_FFC.x = (g_CurrentPlayer->gun_azimuth_angle * (1.0f - gunaimdamp) * screen_width * 0.5f) + (screen_width * 0.5f);
    g_CurrentPlayer->field_FFC.y = (g_CurrentPlayer->gun_azimuth_turning * (1.0f - gunaimdamp) * screen_height * 0.5f) + (screen_height * 0.5f);

    g_CurrentPlayer->field_FFC.x += getPlayer_c_screenleft();
    g_CurrentPlayer->field_FFC.y += getPlayer_c_screentop();

    transformAndNormalizeByLength2Dto3D(&g_CurrentPlayer->field_FFC, &coords, 1000.0f);
    sub_GAME_7F067AB4(&coords);
}
