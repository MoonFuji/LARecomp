// Gameplay camera: FOV scale, the chase camera smoothing that holds above
// 30 FPS, and the free-fly debug camera.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/ui/window.h>
#include <cmath>
#include <cstdint>
#include <string>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif
#include "imgui.h"

#include "hooks.h"
#include "hooks_internal.h"
#include "../menu_camera.h"

REXCVAR_DEFINE_DOUBLE(fov_1p_scale, 1.0, "MCLA/Camera", "FOV scale — 1st person / cockpit (0.5 = narrower, 2.0 = wider)")
    .range(0.5, 5.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(fov_3p_scale, 1.0, "MCLA/Camera", "FOV scale — 3rd person / chase cam (0.5 = narrower, 2.0 = wider)")
    .range(0.5, 5.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// BadassBaboon's Recomp Adjustments: Continuous exponential camera boom smoothing at 60 FPS
REXCVAR_DEFINE_BOOL(smooth_chase_cam, true, "MCLA/Camera",
    "Fix: Smooth chase camera boom interpolation at 60 FPS using continuous-time exponential decay.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(chase_cam_smoothing_factor, 1.0, "MCLA/Camera",
    "Chase camera boom smoothing factor multiplier (0.1 - 3.0).")
    .range(0.1, 3.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// BadassBaboon's Recomp Adjustments: Camera shake scaling for high framerates and intensity tuning
REXCVAR_DEFINE_BOOL(scale_camera_shake, true, "MCLA/Camera",
    "Fix: Scale camera shake amplitude with framerate (60+ FPS) to prevent violent noise jitter during drifts, burnouts, and crashes.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(camera_shake_scale, 1.0, "MCLA/Camera",
    "Camera shake intensity multiplier (0.0 = disabled, 1.0 = normal, 2.0 = strong).")
    .range(0.0, 3.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// BadassBaboon's Recomp Adjustments: Continuous-time camera boom gimbal smoothing
REXCVAR_DEFINE_BOOL(smooth_camera_gimbal, true, "MCLA/Camera",
    "Fix: Smooth camera boom gimbal / orbit rotation using continuous-time exponential decay across high framerates.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// BadassBaboon's Recomp Adjustments: Camera transition / intro orbit Quaternion SLERP stabilization
REXCVAR_DEFINE_BOOL(smooth_camera_transition, true, "MCLA/Camera",
    "Fix: Stabilize camera transition and race intro orbit interpolation using Quaternion SLERP instead of Euler angles.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(debug_cam, "off", "MCLA/Camera",
    "Free-fly camera during live gameplay: left stick moves, right stick looks, triggers change "
    "speed. Gameplay keeps running underneath (drive, traffic, physics).")
    .allowed({"off", "free"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(debug_cam_speed, 40.0, "MCLA/Camera",
    "Free-fly camera move speed (world units/second).")
    .range(1.0, 500.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(debug_cam_sens, 2.5, "MCLA/Camera",
    "Free-fly camera look sensitivity (radians/second at full stick).")
    .range(0.2, 10.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(debug_cam_mouse, true, "MCLA/Camera",
    "Free-fly camera: use the mouse to look (captures the cursor while active).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(debug_cam_mouse_sens, 0.0025, "MCLA/Camera",
    "Free-fly mouse look sensitivity (radians per mouse pixel).")
    .range(0.0002, 0.02)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Free-fly camera host state (see Patch_DebugCam). Angles are seeded from the
// camera on activation, then integrated from the right stick each frame. The
// world position lives in guest memory at +0x50 and is read/written in place.
static bool g_freecam_seeded = false;
static float g_freecam_yaw = 0.0f;
static float g_freecam_pitch = 0.0f;

// Mouse-look for the free-fly camera. Self-contained OS mouse capture (Win32):
// while active we hide + confine the cursor, recenter it every frame, and feed
// the raw pixel delta into yaw/pitch as a displacement (no dt scaling — mouse
// deltas are already per-frame). Independent of mnk_mode, so it does not hijack
// the pad. Sign matches the right-stick path: mouse right = look right (yaw
// down), mouse up = look up (pitch down). Caller clamps pitch afterward.
#if defined(_WIN32)
static bool g_freecam_mouse_captured = false;
static void FreecamMouseUpdate(bool active, float& yaw, float& pitch) {
    auto* win = rex::Runtime::instance()->display_window();
    if (!win) return;
    HWND hwnd = static_cast<HWND>(win->GetNativeWindowHandle());
    if (!hwnd) return;

    // Release the cursor whenever an ImGui overlay (F4 RexGlue Settings, console,
    // etc.) wants the mouse, so the menu is usable; recapture once it is closed.
    // Deterministic from the live overlay state — no key-toggle to drift out of
    // sync with the menu. debug_cam_mouse is the master on/off.
    bool menu_open = ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse;
    bool want = active && REXCVAR_GET(debug_cam_mouse) && !menu_open;

    // Client-area center in screen coordinates (recenter target).
    RECT rc;
    GetClientRect(hwnd, &rc);
    POINT center = {(rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2};
    ClientToScreen(hwnd, &center);

    if (want && !g_freecam_mouse_captured) {
        g_freecam_mouse_captured = true;
        win->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
        win->CaptureMouse();
        SetCursorPos(center.x, center.y);  // seed center, skip first-frame spike
        return;
    }
    if (!want) {
        if (g_freecam_mouse_captured) {
            g_freecam_mouse_captured = false;
            win->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
            win->ReleaseMouse();
        }
        return;
    }

    POINT cur;
    GetCursorPos(&cur);
    int dx = cur.x - center.x;
    int dy = cur.y - center.y;
    float s = static_cast<float>(REXCVAR_GET(debug_cam_mouse_sens));
    yaw -= dx * s;
    pitch += dy * s;
    SetCursorPos(center.x, center.y);
}
#else
static void FreecamMouseUpdate(bool, float&, float&) {}
#endif

// Normalize an XInput thumbstick axis (int16, -32768..32767, centered at 0) to
// [-1, 1] with a small radial deadzone. XInput convention: up/right = positive.
static float NormalizeStick(int16_t raw) {
    float v = float(raw) / 32767.0f;
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    if (v < 0.15f && v > -0.15f) return 0.0f;
    return v;
}

// Gate bypass so the free-fly camera runs during live gameplay. In sub_822C0320
// the dcam manager block only executes when there is no gameplay camera source
// (r29 == 0, menus) or the photo-mode-active byte is set; in normal gameplay it
// is skipped, so Patch_DebugCam never fires. Hook at the gate compare
// (0x822C0644); returning true jumps straight to the manager block
// (0x822C065C), the same target as the gate-passed path (skipped instructions
// are loads/compares only). Only bypasses when the free camera is requested, so
// photo mode is untouched when debug_cam = off.
bool Patch_DebugCamGate() {
    return REXCVAR_GET(debug_cam) == "free";
}

// Free-fly camera driver. Hook at 0x822C0668: r3 = dcam manager (from
// camsys+0x33C, already null-checked), right before the manager's per-frame
// update (sub_82502E18) which in turn updates the active camera.
//
// The manager's active camera index (mgr+0, big-endian u32) selects slot
// mgr+8+index*4; index 0 (mgr+8) is the free-fly camera — its update
// (sub_82536288 -> sub_82537450) integrates position from velocity and rebuilds
// its orientation matrix every frame from scalar angles, so driving those
// scalars + position is enough to fly it. Camera object layout (offsets from
// the object pointer):
//   +0x3C float  yaw   (added to +0x38, which we keep 0)
//   +0x40 float  pitch
//   +0x50 vec4   velocity (added into position each frame; we zero it and set
//                 position directly)
//   +0x100 vec4  position [x, y, z, w]
// The game rebuilds orientation as a yaw about +Y (sub_82202E38), i.e. Y is up.
// We keep yaw/pitch/position in host state and drive them from the sticks.
void Patch_DebugCam(PPCRegister& r3) {
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    uint32_t mgr = static_cast<uint32_t>(r3.u64);

    if (REXCVAR_GET(debug_cam) != "free") {
        // Release: cut the blend back to the gameplay camera immediately.
        if (ReadGuestF32(base, mgr + 860) > 0.0f) {
            WriteGuestF32(base, mgr + 860, 0.0f);
        }
        g_freecam_seeded = false;
        FreecamMouseUpdate(false, g_freecam_yaw, g_freecam_pitch);  // release cursor
        return;
    }

    // Force the free-fly camera (index 0) active and fully blended in.
    base[mgr + 0] = 0;
    base[mgr + 1] = 0;
    base[mgr + 2] = 0;
    base[mgr + 3] = 0;
    WriteGuestF32(base, mgr + 860, 1.0f);
    // mgr+856 gates the per-frame camera update in sub_82502E18 (the call that
    // rebuilds the orientation matrix at +0xD0 from our angles). It is 1 in the
    // ctor but cleared outside photo mode, freezing the orientation. Force it on.
    base[mgr + 856] = 1;

    uint32_t cam = ReadGuestU32(base, mgr + 8);
    if (!cam) return;

    // Kill the camera's internal look input so the update (sub_82536288) does
    // not overwrite our yaw at +0x3C from the (unfed) photo-mode input source.
    WriteGuestU32(base, cam + 0x68, 0);

    // Seed our angle state from the camera's current orientation on activation
    // so the view does not jump. Position lives at +0x50 and is read fresh each
    // frame (below), so it needs no host-side seed.
    if (!g_freecam_seeded) {
        g_freecam_yaw = ReadGuestF32(base, cam + 0x3C);
        g_freecam_pitch = ReadGuestF32(base, cam + 0x40);
        g_freecam_seeded = true;
    }

    float dt = ReadGuestF32(base, 0x827D755C);  // clock+0x5C: raw frame dt
    if (dt <= 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;

    // Read the pad straight from the host input system (the guest ioPad globals
    // are ambiguous). XInput axes: up/right positive, int16 range.
    float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
    auto* isys = static_cast<rex::input::InputSystem*>(
        rex::Runtime::instance()->input_system());
    if (isys) {
        rex::input::X_INPUT_STATE state{};
        if (isys->GetState(0, &state) == 0) {
            lx = NormalizeStick(state.gamepad.thumb_lx);
            ly = -NormalizeStick(state.gamepad.thumb_ly);  // stick up = forward
            rx = -NormalizeStick(state.gamepad.thumb_rx);  // stick right = look right
            ry = -NormalizeStick(state.gamepad.thumb_ry);  // stick up = look up
        }
    }

    float sens = static_cast<float>(REXCVAR_GET(debug_cam_sens));
    float speed = static_cast<float>(REXCVAR_GET(debug_cam_speed));

    // Look: integrate the right stick into yaw/pitch and write them back; the
    // game rebuilds the orientation matrix (+0xD0) from these each frame.
    g_freecam_yaw += rx * sens * dt;
    g_freecam_pitch += ry * sens * dt;
    // Add mouse look on top of the stick (displacement, no dt).
    FreecamMouseUpdate(true, g_freecam_yaw, g_freecam_pitch);
    if (g_freecam_pitch > 1.5f) g_freecam_pitch = 1.5f;
    if (g_freecam_pitch < -1.5f) g_freecam_pitch = -1.5f;
    WriteGuestF32(base, cam + 0x38, 0.0f);
    WriteGuestF32(base, cam + 0x3C, g_freecam_yaw);
    WriteGuestF32(base, cam + 0x40, g_freecam_pitch);

    // Move: the world position is the vec3 at +0x50 (x, y=height, z). Read it
    // fresh (so the native button controls LB/RB/LT/RT still add in), advance it
    // along the view direction from the left stick, and write it back. Y is up
    // (orientation is a yaw about Y).
    float cy = std::cos(g_freecam_yaw), sy = std::sin(g_freecam_yaw);
    float cp = std::cos(g_freecam_pitch), sp = std::sin(g_freecam_pitch);
    float fwd_x = cp * sy, fwd_y = sp, fwd_z = cp * cy;  // forward (yaw+pitch)
    float right_x = cy, right_z = -sy;                   // horizontal strafe
    float step = speed * dt;

    float px = ReadGuestF32(base, cam + 0x50);
    float py = ReadGuestF32(base, cam + 0x54);
    float pz = ReadGuestF32(base, cam + 0x58);
    px += (fwd_x * ly + right_x * lx) * step;
    py += (fwd_y * ly) * step;
    pz += (fwd_z * ly + right_z * lx) * step;
    WriteGuestF32(base, cam + 0x50, px);
    WriteGuestF32(base, cam + 0x54, py);
    WriteGuestF32(base, cam + 0x58, pz);

    // Hand the pose to the menu-camera module so menu_cam_dump can emit the spot
    // you are flying at (position + look direction) as a front-end camera line.
    MenuCam_NoteFreecam(px, py, pz, g_freecam_yaw, g_freecam_pitch);
}

void Patch_FOVScale(PPCRegister& f1, PPCRegister& r24) {
    int cam_idx = static_cast<int>(r24.u64);
    double scale = (cam_idx == 1) ? REXCVAR_GET(fov_1p_scale) : REXCVAR_GET(fov_3p_scale);
    if (scale != 1.0) {
        f1.f64 = f1.f64 * scale;
    }
}

// BadassBaboon's Recomp Adjustments:
// Continuous-time exponential decay for chase camera smoothing factors.
// In sub_82320298 (mcPlayerCamera::Update):
//   0x82320468 - f13 is the camera position chase/lag factor S1
//   0x823204F4 - f0  is the camera look-at / orientation factor S2
//
// On 30 FPS console the engine multiplied the raw profile factor by 0.5 and stepped once per update:
//   S(dt) = 1 - (1 - 0.5 * S_raw) ^ (30 * dt * scale)
// The engine ALREADY halves this factor itself when the frame rate is under
// 60, at 0x82320460:
//
//   82320454  cmpwi cr6, r11, 0x3C     ; r11 = round(1/dt), the frame rate
//   8232045c  lfs   f13, 0xE0(r11)     ; f13 = the raw tune value
//   82320460  bge   cr6, loc_82320468  ; >= 60 fps? leave it alone
//   82320464  fmuls f13, f13, f30      ; else f13 = tune * 0.5
//   82320468  <-- both camera hooks land here
//
// So the value arriving in the hook is the raw tune above 60 fps and half the
// tune below it. Multiplying by 0.5 unconditionally therefore quartered the
// factor whenever the measured rate dipped under 60, and doubled it back the
// moment it recovered. Around the 60 fps boundary that flips every frame,
// which is the camera jitter seen while drifting, sliding and doing donuts:
// exactly the moments where the frame rate wobbles across the threshold.
//
// Halving only when the engine did not reproduces the console reference curve
// continuously across the boundary.
static bool EngineAlreadyHalvedCameraFactor(const uint8_t* base) {
    const float fps = ReadGuestF32(base, kGuestFrameRate);
    // fctiwz after the +/- 0.5 bias is round-half-away-from-zero.
    const int fps_i = static_cast<int>(fps >= 0.0f ? fps + 0.5f : fps - 0.5f);
    return fps_i < 60;
}

static void ApplyCameraSmoothing(PPCRegister& reg) {
    if (!REXCVAR_GET(smooth_chase_cam)) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    const float dt = ReadGuestF32(base, kGuestFrameDelta);
    const double raw_k = reg.f64;
    if (raw_k <= 0.0 || raw_k >= 1.0 || dt <= 0.0f) return;

    const double k30 = EngineAlreadyHalvedCameraFactor(base) ? raw_k : 0.5 * raw_k;
    const double scale = REXCVAR_GET(chase_cam_smoothing_factor);
    reg.f64 = 1.0 - std::pow(1.0 - k30, static_cast<double>(dt) * 30.0 * scale);
}

void MCLACameraPosSmoothing(PPCRegister& f13) {
    ApplyCameraSmoothing(f13);
}

void MCLACameraLookAtSmoothing(PPCRegister& f0) {
    ApplyCameraSmoothing(f0);
}

// BadassBaboon's Recomp Adjustments: Camera shake amplitude scaling for high and uncapped framerates
// In sub_82315000:
// The engine evaluates channel amplitude (f31 = amp * intensity * envelope), then steps
// the LCG PRNG (qword_82860880) each frame and injects discrete white noise angle/position kicks.
// Without time scaling, noise frequency and RMS angular velocity scale directly with FPS,
// transforming low-frequency rumble into violent buzzing/vibrating at 100+ FPS.
// Scaling amplitude by min(1.0, 60.0 * dt) keeps the perceived angular velocity invariant.
void MCLACameraShakeScaling(PPCRegister& f31) {
    if (!REXCVAR_GET(scale_camera_shake)) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    const float dt = ReadGuestF32(base, kGuestFrameDelta);
    if (dt <= 0.0f) return;

    const double fps_scale = (dt < (1.0f / 60.0f)) ? (static_cast<double>(dt) * 60.0) : 1.0;
    const double user_scale = REXCVAR_GET(camera_shake_scale);
    f31.f64 *= fps_scale * user_scale;
}

// BadassBaboon's Recomp Adjustments: Lookaround & camera boom gimbal continuous-time exponential smoothing
// In sub_822B2AB0 (mcPlayerCamera update at 0x822B30A4):
// f31 is the raw per-frame LookaroundLerpFactor k0 (tune+48).
// Converting to continuous-time exponential retention:
//   k(dt) = 1.0 - (1.0 - k0)^(60 * dt)
// keeps gimbal response and orbit rotation speed perfectly uniform across framerates.
void MCLACameraGimbalSmoothing(PPCRegister& f31) {
    if (!REXCVAR_GET(smooth_camera_gimbal)) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    const float dt = ReadGuestF32(base, kGuestFrameDelta);
    const double raw_k = f31.f64;
    if (raw_k <= 0.0 || raw_k >= 1.0 || dt <= 0.0f) return;

    f31.f64 = 1.0 - std::pow(1.0 - raw_k, static_cast<double>(dt) * 60.0);
}

// BadassBaboon's Recomp Adjustments: Lookaround yaw shortest-path angular delta
// In sub_822B2AB0 at 0x822B30AC: fsubs f0, f27, f1 computes delta_yaw = target - current.
// Normalizes delta into [-pi, +pi] to prevent 360-degree snap spins across the +/-pi seam.
void MCLACameraYawShortestPath(PPCRegister& f0) {
    double d = f0.f64;
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kTwoPi = 6.28318530717958647692;
    while (d > kPi) d -= kTwoPi;
    while (d < -kPi) d += kTwoPi;
    f0.f64 = d;
}

// -----------------------------------------------------------------------------------------
// Quaternion Math for Camera Transition SLERP Stabilization
// -----------------------------------------------------------------------------------------
namespace {

struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

// Shepperd's algorithm for converting 3x3 orthonormal row vectors to a unit quaternion
static Quat MatrixToQuat(const float r0[3], const float r1[3], const float r2[3]) {
    const float trace = r0[0] + r1[1] + r2[2];
    Quat q;
    if (trace > 0.0f) {
        float s = 0.5f / std::sqrt(trace + 1.0f);
        q.w = 0.25f / s;
        q.x = (r1[2] - r2[1]) * s;
        q.y = (r2[0] - r0[2]) * s;
        q.z = (r0[1] - r1[0]) * s;
    } else {
        if (r0[0] > r1[1] && r0[0] > r2[2]) {
            float s = 2.0f * std::sqrt(std::max(0.0f, 1.0f + r0[0] - r1[1] - r2[2]));
            if (s < 1e-6f) s = 1e-6f;
            q.w = (r1[2] - r2[1]) / s;
            q.x = 0.25f * s;
            q.y = (r0[1] + r1[0]) / s;
            q.z = (r0[2] + r2[0]) / s;
        } else if (r1[1] > r2[2]) {
            float s = 2.0f * std::sqrt(std::max(0.0f, 1.0f + r1[1] - r0[0] - r2[2]));
            if (s < 1e-6f) s = 1e-6f;
            q.w = (r2[0] - r0[2]) / s;
            q.x = (r0[1] + r1[0]) / s;
            q.y = 0.25f * s;
            q.z = (r1[2] + r2[1]) / s;
        } else {
            float s = 2.0f * std::sqrt(std::max(0.0f, 1.0f + r2[2] - r0[0] - r1[1]));
            if (s < 1e-6f) s = 1e-6f;
            q.w = (r0[1] - r1[0]) / s;
            q.x = (r0[2] + r2[0]) / s;
            q.y = (r1[2] + r2[1]) / s;
            q.z = 0.25f * s;
        }
    }
    const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len > 1e-6f) {
        q.x /= len; q.y /= len; q.z /= len; q.w /= len;
    } else {
        q = {0.0f, 0.0f, 0.0f, 1.0f};
    }
    return q;
}

// Spherical Linear Interpolation (SLERP) along geodesic shortest path
static Quat QuatSlerp(Quat q1, Quat q2, float t) {
    float dot = q1.x * q2.x + q1.y * q2.y + q1.z * q2.z + q1.w * q2.w;
    if (dot < 0.0f) {
        q2.x = -q2.x; q2.y = -q2.y; q2.z = -q2.z; q2.w = -q2.w;
        dot = -dot;
    }
    if (dot > 0.9995f) {
        Quat res = {
            q1.x + t * (q2.x - q1.x),
            q1.y + t * (q2.y - q1.y),
            q1.z + t * (q2.z - q1.z),
            q1.w + t * (q2.w - q1.w)
        };
        const float len = std::sqrt(res.x * res.x + res.y * res.y + res.z * res.z + res.w * res.w);
        if (len > 1e-6f) {
            res.x /= len; res.y /= len; res.z /= len; res.w /= len;
        }
        return res;
    }
    const float theta_0 = std::acos(std::min(1.0f, std::max(-1.0f, dot)));
    const float theta = theta_0 * t;
    const float sin_theta = std::sin(theta);
    const float sin_theta_0 = std::sin(theta_0);
    const float s1 = std::cos(theta) - dot * sin_theta / sin_theta_0;
    const float s2 = sin_theta / sin_theta_0;
    return {
        s1 * q1.x + s2 * q2.x,
        s1 * q1.y + s2 * q2.y,
        s1 * q1.z + s2 * q2.z,
        s1 * q1.w + s2 * q2.w
    };
}

static void QuatToMatrix(const Quat& q, float r0[3], float r1[3], float r2[3]) {
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

    r0[0] = 1.0f - 2.0f * (yy + zz);
    r0[1] = 2.0f * (xy + wz);
    r0[2] = 2.0f * (xz - wy);

    r1[0] = 2.0f * (xy - wz);
    r1[1] = 1.0f - 2.0f * (xx + zz);
    r1[2] = 2.0f * (yz + wx);

    r2[0] = 2.0f * (xz + wy);
    r2[1] = 2.0f * (yz - wx);
    r2[2] = 1.0f - 2.0f * (xx + yy);
}

} // anonymous namespace

// BadassBaboon's Recomp Adjustments: Camera transition & intro orbit Quaternion SLERP stabilization
// In sub_8271F438 (camTransition::Update at 0x8271F920):
// Replaces the native Euler ZXY angle decomposition and linear angle blend with Quaternion SLERP.
// Eliminates gimbal lock singularities, polar angle sign flips, and camera position wobbles.
void MCLACameraTransitionSlerp(PPCRegister& r31, PPCRegister& r1, PPCRegister& r26) {
    if (!REXCVAR_GET(smooth_camera_transition)) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    const uint32_t out_addr = static_cast<uint32_t>(r31.u64);
    const uint32_t sp = static_cast<uint32_t>(r1.u64);
    const uint32_t t_ptr = static_cast<uint32_t>(r26.u64);
    if (!out_addr || !sp || !t_ptr) return;

    float t = ReadGuestF32(base, t_ptr);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    // Source camera 3x3 orientation from stack (r1 + 0x80, 0x90, 0xA0)
    const float src_r0[3] = {
        ReadGuestF32(base, sp + 0x80),
        ReadGuestF32(base, sp + 0x84),
        ReadGuestF32(base, sp + 0x88)
    };
    const float src_r1[3] = {
        ReadGuestF32(base, sp + 0x90),
        ReadGuestF32(base, sp + 0x94),
        ReadGuestF32(base, sp + 0x98)
    };
    const float src_r2[3] = {
        ReadGuestF32(base, sp + 0xA0),
        ReadGuestF32(base, sp + 0xA4),
        ReadGuestF32(base, sp + 0xA8)
    };

    // Target camera 3x3 orientation from stack (r1 + 0xC0, 0xD0, 0xE0)
    const float tgt_r0[3] = {
        ReadGuestF32(base, sp + 0xC0),
        ReadGuestF32(base, sp + 0xC4),
        ReadGuestF32(base, sp + 0xC8)
    };
    const float tgt_r1[3] = {
        ReadGuestF32(base, sp + 0xD0),
        ReadGuestF32(base, sp + 0xD4),
        ReadGuestF32(base, sp + 0xD8)
    };
    const float tgt_r2[3] = {
        ReadGuestF32(base, sp + 0xE0),
        ReadGuestF32(base, sp + 0xE4),
        ReadGuestF32(base, sp + 0xE8)
    };

    const float src_len2 = src_r0[0] * src_r0[0] + src_r0[1] * src_r0[1] + src_r0[2] * src_r0[2];
    const float tgt_len2 = tgt_r0[0] * tgt_r0[0] + tgt_r0[1] * tgt_r0[1] + tgt_r0[2] * tgt_r0[2];
    if (src_len2 < 0.1f || tgt_len2 < 0.1f) return;

    const Quat q_src = MatrixToQuat(src_r0, src_r1, src_r2);
    const Quat q_tgt = MatrixToQuat(tgt_r0, tgt_r1, tgt_r2);
    const Quat q_interp = QuatSlerp(q_src, q_tgt, t);

    float out_r0[3], out_r1[3], out_r2[3];
    QuatToMatrix(q_interp, out_r0, out_r1, out_r2);

    WriteGuestF32(base, out_addr + 0x00, out_r0[0]);
    WriteGuestF32(base, out_addr + 0x04, out_r0[1]);
    WriteGuestF32(base, out_addr + 0x08, out_r0[2]);

    WriteGuestF32(base, out_addr + 0x10, out_r1[0]);
    WriteGuestF32(base, out_addr + 0x14, out_r1[1]);
    WriteGuestF32(base, out_addr + 0x18, out_r1[2]);

    WriteGuestF32(base, out_addr + 0x20, out_r2[0]);
    WriteGuestF32(base, out_addr + 0x24, out_r2[1]);
    WriteGuestF32(base, out_addr + 0x28, out_r2[2]);
}
#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

bool Patch_DebugCamGate() { return false; }
void Patch_DebugCam(PPCRegister& r3) {}
void Patch_FOVScale(PPCRegister& f1, PPCRegister& r24) {}
void MCLACameraPosSmoothing(PPCRegister& f13) {}
void MCLACameraLookAtSmoothing(PPCRegister& f0) {}
void MCLACameraShakeScaling(PPCRegister& f31) {}
void MCLACameraGimbalSmoothing(PPCRegister& f31) {}
void MCLACameraYawShortestPath(PPCRegister& f0) {}
void MCLACameraTransitionSlerp(PPCRegister& r31, PPCRegister& r1, PPCRegister& r26) {}
#endif // REXGLUE_HAS_XEO3_TARGET
