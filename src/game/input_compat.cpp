// Compatibility adapter: implements the old vendored recomp::/zelda64::
// input and option API on top of RecompFrontend's recompinput. This is the
// strangler-fig seam of the frontend migration — the game glue
// (controls.cpp, config.cpp, recomp_api.cpp, main.cpp) keeps its old call
// surface while the implementation is the new library. Screens/menus get
// ported to recompui incrementally; option values live here as statics and
// are persisted through the existing zelda64::load_config/save_config JSON
// layer, which calls the setters below at startup.

#include <array>
#include <atomic>
#include <vector>

#include "SDL.h"

#include "recompinput/recompinput.h"
#include "recompinput/input_events.h"

#include "recomp_input.h"
#include "recomp_ui.h"
#include "zelda_config.h"
#include "zelda_sound.h"

// ---------------------------------------------------------------------------
// Input field conversion. The old InputField layout {uint32_t input_type,
// int32_t input_id} and enum numbering (None=0, Keyboard, Mouse,
// ControllerDigital, ControllerAnalog) share their lineage with recompinput's
// — values pass through numerically so existing saved bindings keep working.
// ---------------------------------------------------------------------------

static recompinput::InputField to_new(const recomp::InputField& old) {
    return recompinput::InputField{ static_cast<recompinput::InputType>(old.input_type), old.input_id };
}

static std::vector<recompinput::InputField> to_new(std::span<const recomp::InputField> fields) {
    std::vector<recompinput::InputField> out;
    out.reserve(fields.size());
    for (const auto& f : fields) {
        out.push_back(to_new(f));
    }
    return out;
}

// GE is a single-input-source game: everything reads player 0.
constexpr int compat_player = 0;

// ---------------------------------------------------------------------------
// Input functions.
// ---------------------------------------------------------------------------

// Latched debug F-key presses (bit n = F(n+1); F10/F11 reserved by the UI).
// Edge-detected in poll_inputs from the SDL keyboard state so it works
// without hooking recompinput's event pump.
static std::atomic<uint32_t> pending_debug_keys = 0;
static uint16_t prev_fkey_state = 0;

static void latch_debug_fkeys() {
    int numkeys = 0;
    const Uint8* keys = SDL_GetKeyboardState(&numkeys);
    if (keys == nullptr || numkeys <= SDL_SCANCODE_F12) {
        return;
    }
    uint16_t cur = 0;
    for (int i = 0; i < 12; i++) {
        SDL_Scancode sc = static_cast<SDL_Scancode>(SDL_SCANCODE_F1 + i);
        if (sc == SDL_SCANCODE_F10 || sc == SDL_SCANCODE_F11) {
            continue;
        }
        if (keys[sc]) {
            cur |= (1u << i);
        }
    }
    uint16_t newly_pressed = cur & ~prev_fkey_state;
    prev_fkey_state = cur;
    if (newly_pressed != 0) {
        pending_debug_keys.fetch_or(newly_pressed);
    }
}

uint32_t recomp::get_and_clear_debug_keys() {
    return pending_debug_keys.exchange(0);
}

void recomp::poll_inputs() {
    recompinput::poll_inputs();
    latch_debug_fkeys();
}

void recomp::handle_events() {
    recompinput::handle_events();
}

bool recomp::game_input_disabled() {
    return recompinput::game_input_disabled();
}

bool recomp::get_input_digital(const recomp::InputField& field) {
    return recompinput::get_input_digital(compat_player, to_new(field));
}

bool recomp::get_input_digital(std::span<const recomp::InputField> fields) {
    auto converted = to_new(fields);
    return recompinput::get_input_digital(compat_player, std::span<const recompinput::InputField>{ converted });
}

float recomp::get_input_analog(const recomp::InputField& field) {
    return recompinput::get_input_analog(compat_player, to_new(field));
}

float recomp::get_input_analog(std::span<const recomp::InputField> fields) {
    auto converted = to_new(fields);
    return recompinput::get_input_analog(compat_player, std::span<const recompinput::InputField>{ converted });
}

void recomp::get_gyro_deltas(float* x, float* y) {
    recompinput::get_gyro_deltas(compat_player, x, y);
}

void recomp::get_mouse_deltas(float* x, float* y) {
    recompinput::get_mouse_deltas(x, y);
}

void recomp::get_right_analog(float* x, float* y) {
    recompinput::get_right_analog(compat_player, x, y);
}

void recomp::apply_joystick_deadzone(float x_in, float y_in, float* x_out, float* y_out) {
    recompinput::apply_joystick_deadzone(x_in, y_in, x_out, y_out);
}

void recomp::set_right_analog_suppressed(bool suppressed) {
    recompinput::set_right_analog_suppressed(suppressed);
}

void recomp::set_rumble(int controller_num, bool on) {
    recompinput::set_rumble(controller_num, on);
}

void recomp::update_rumble() {
    recompinput::update_rumble();
}

ultramodern::input::connected_device_info_t recomp::get_connected_device_info(int controller_num) {
    // TODO route through recompinput's player/device tracking; the default
    // (controller + rumble pak, matching the old behaviour of forcing the
    // rumble pak on) keeps the game's device queries satisfied.
    (void)controller_num;
    return ultramodern::input::connected_device_info_t{
        ultramodern::input::Device::Controller,
        ultramodern::input::Pak::RumblePak,
    };
}

// ---------------------------------------------------------------------------
// Default mappings: converted from recompinput's own defaults would be ideal,
// but the old UI only reads these when resetting bindings — keep the previous
// vendored keyboard defaults and empty controller defaults (controller
// binding goes through recompinput's own profile system).
// ---------------------------------------------------------------------------

static recomp::InputField kb(int32_t scancode) {
    return recomp::InputField{ 1 /* InputType::Keyboard lineage value */, scancode };
}

const recomp::DefaultN64Mappings recomp::default_n64_keyboard_mappings = {
    .a = { kb(SDL_SCANCODE_SPACE) },
    .b = { kb(SDL_SCANCODE_LSHIFT) },
    .l = { kb(SDL_SCANCODE_E) },
    .r = { kb(SDL_SCANCODE_R) },
    .z = { kb(SDL_SCANCODE_Q) },
    .start = { kb(SDL_SCANCODE_RETURN) },
    .c_left = { kb(SDL_SCANCODE_LEFT) },
    .c_right = { kb(SDL_SCANCODE_RIGHT) },
    .c_up = { kb(SDL_SCANCODE_UP) },
    .c_down = { kb(SDL_SCANCODE_DOWN) },
    .dpad_left = { kb(SDL_SCANCODE_J) },
    .dpad_right = { kb(SDL_SCANCODE_L) },
    .dpad_up = { kb(SDL_SCANCODE_I) },
    .dpad_down = { kb(SDL_SCANCODE_K) },
    .analog_left = { kb(SDL_SCANCODE_A) },
    .analog_right = { kb(SDL_SCANCODE_D) },
    .analog_up = { kb(SDL_SCANCODE_W) },
    .analog_down = { kb(SDL_SCANCODE_S) },
};

const recomp::DefaultN64Mappings recomp::default_n64_controller_mappings = {};

// ---------------------------------------------------------------------------
// Option storage. Defaults mirror the old ui_config context; config.cpp's
// load path overwrites them from config.json via these setters.
// ---------------------------------------------------------------------------

static int opt_mouse_sensitivity = 0;
static int opt_joystick_deadzone = 5;
static int opt_gyro_sensitivity = 50;
static int opt_rumble_strength = 25;
static recomp::BackgroundInputMode opt_background_input = recomp::BackgroundInputMode::On;

int recomp::get_mouse_sensitivity() { return opt_mouse_sensitivity; }
void recomp::set_mouse_sensitivity(int sensitivity) { opt_mouse_sensitivity = sensitivity; }
int recomp::get_joystick_deadzone() { return opt_joystick_deadzone; }
void recomp::set_joystick_deadzone(int deadzone) { opt_joystick_deadzone = deadzone; }
int recomp::get_gyro_sensitivity() { return opt_gyro_sensitivity; }
void recomp::set_gyro_sensitivity(int sensitivity) { opt_gyro_sensitivity = sensitivity; }
int recomp::get_rumble_strength() { return opt_rumble_strength; }
void recomp::set_rumble_strength(int strength) { opt_rumble_strength = strength; }
recomp::BackgroundInputMode recomp::get_background_input_mode() { return opt_background_input; }
void recomp::set_background_input_mode(recomp::BackgroundInputMode mode) { opt_background_input = mode; }

static zelda64::TargetingMode opt_targeting = zelda64::TargetingMode::Switch;
static zelda64::AutosaveMode opt_autosave = zelda64::AutosaveMode::On;
static zelda64::AnalogCamMode opt_analog_cam = zelda64::AnalogCamMode::Off;
static zelda64::CameraInvertMode opt_cam_invert = zelda64::CameraInvertMode::InvertY;
static zelda64::CameraInvertMode opt_analog_cam_invert = zelda64::CameraInvertMode::InvertNone;
static bool opt_debug_mode = false;
static bool opt_low_health_beeps = true;
static int opt_main_volume = 100;
static int opt_bgm_volume = 100;
static int opt_sfx_volume = 100;
static int opt_voice_volume = 100;

zelda64::TargetingMode zelda64::get_targeting_mode() { return opt_targeting; }
void zelda64::set_targeting_mode(zelda64::TargetingMode mode) { opt_targeting = mode; }
zelda64::AutosaveMode zelda64::get_autosave_mode() { return opt_autosave; }
void zelda64::set_autosave_mode(zelda64::AutosaveMode mode) { opt_autosave = mode; }
zelda64::AnalogCamMode zelda64::get_analog_cam_mode() { return opt_analog_cam; }
void zelda64::set_analog_cam_mode(zelda64::AnalogCamMode mode) { opt_analog_cam = mode; }
zelda64::CameraInvertMode zelda64::get_camera_invert_mode() { return opt_cam_invert; }
void zelda64::set_camera_invert_mode(zelda64::CameraInvertMode mode) { opt_cam_invert = mode; }
zelda64::CameraInvertMode zelda64::get_analog_camera_invert_mode() { return opt_analog_cam_invert; }
void zelda64::set_analog_camera_invert_mode(zelda64::CameraInvertMode mode) { opt_analog_cam_invert = mode; }
bool zelda64::get_debug_mode_enabled() { return opt_debug_mode; }
void zelda64::set_debug_mode_enabled(bool enabled) { opt_debug_mode = enabled; }
bool zelda64::get_low_health_beeps_enabled() { return opt_low_health_beeps; }
void zelda64::set_low_health_beeps_enabled(bool enabled) { opt_low_health_beeps = enabled; }
int zelda64::get_main_volume() { return opt_main_volume; }
void zelda64::set_main_volume(int volume) { opt_main_volume = volume; }
int zelda64::get_bgm_volume() { return opt_bgm_volume; }
void zelda64::set_bgm_volume(int volume) { opt_bgm_volume = volume; }
int zelda64::get_sfx_volume() { return opt_sfx_volume; }
void zelda64::set_sfx_volume(int volume) { opt_sfx_volume = volume; }
int zelda64::get_voice_volume() { return opt_voice_volume; }
void zelda64::set_voice_volume(int volume) { opt_voice_volume = volume; }

void zelda64::reset_sound_settings() {
    opt_main_volume = 100;
    opt_bgm_volume = 100;
    opt_sfx_volume = 100;
    opt_voice_volume = 100;
}

// ---------------------------------------------------------------------------
// Old recompui hooks still referenced by the glue. The real config menus are
// recompui's now; these become no-ops until the GE screens are ported.
// ---------------------------------------------------------------------------

void recompui::update_supported_options() {
    // TODO port to the new recompui config system.
}
