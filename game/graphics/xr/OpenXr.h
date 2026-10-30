#pragma once

/*!
 * @file OpenXr.h
 * OpenXR session for the Jak 1 stereo path: instance, system, session bound to the window's GL
 * context, one colour swapchain per eye and the frame loop. The renderer only sees the per-eye
 * targets below; everything OpenXR-specific stays in OpenXr.cpp.
 */

#include <cmath>
#include <string>

#include "common/common_types.h"

namespace xr {

enum class State {
  OFF,         // toggle off, nothing created
  NO_RUNTIME,  // xrCreateInstance failed (no runtime, or no GL binding); retried
  NO_SYSTEM,   // instance up, no headset (xrGetSystem failed); retried
  IDLE,        // session created, waiting for the runtime to report READY
  RUNNING,     // xrBeginSession done, frames are submitted
  FAILED,      // unrecoverable error in session or swapchain setup; retried after a delay
};

const char* state_name(State s);

/*!
 * One eye's target for the current frame. The texture is the acquired swapchain image; the
 * renderer draws the eye, then copies it in with the projection-centre shift applied.
 */
struct EyeTarget {
  u32 gl_texture = 0;
  int width = 0;
  int height = 0;
  // eye position along the head's x axis, metres, OpenXR convention (+x right)
  float head_x_m = 0.f;
  // tangents of the runtime's field of view: left and down are negative
  float tan_left = -1.f, tan_right = 1.f, tan_up = 1.f, tan_down = -1.f;
  // head tracking, in the game camera's axes (+x left, +y up, +z forward): a game camera-space
  // point c is at rot * (c - pos_m * units per metre) in this eye's camera space
  float rot[3][3] = {{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}};
  float pos_m[3] = {0.f, 0.f, 0.f};
};

/*!
 * The 2D layer (HUD, menus, subtitles, debug text): one canvas image, composited by the runtime
 * as a head-locked quad kUiDistanceM in front of the view, kUiWidthDeg wide.
 */
constexpr float kUiDistanceM = 2.0f;
constexpr float kUiWidthDeg = 55.0f;
constexpr int kUiCanvasW = 1920;
constexpr int kUiCanvasH = 1080;

struct UiTarget {
  u32 gl_texture = 0;  // premultiplied alpha, display-encoded like the eyes
  int width = 0;
  int height = 0;
  float aspect = 16.f / 9.f;  // set by the renderer: the game's 2D canvas width / height
  bool drawn = false;         // set by the renderer once the canvas is in the image
};

/// Half extents (metres) of the UI quad at kUiDistanceM for a canvas of this aspect.
inline void ui_half_size(float aspect, float* half_w, float* half_h) {
  *half_w = kUiDistanceM * std::tan(0.5f * kUiWidthDeg * 0.01745329252f);
  *half_h = *half_w / (aspect > 0.1f ? aspect : 16.f / 9.f);
}

struct FrameTargets {
  EyeTarget eye[2];
  UiTarget ui;
  // true: the eyes follow the head (rot, pos_m) and the frame is drawn with a symmetric frustum
  // of half-tangents render_tan_x/y at render_w x render_h, each eye cropped from it.
  // false (no tracking): the fixed-IPD stereo path, head_x_m only, submitted head-locked
  bool tracked = false;
  float render_tan_x = 1.f, render_tan_y = 1.f;
  int render_w = 0, render_h = 0;
  bool rendered = false;  // set by the renderer once both eyes are in the images
  bool fake_head = false;  // the head pose is scripted (set_fake), not tracked
  // the head view the game drew the chain from (pc-get-xr-cull): its camera is then the head,
  // a point c of the game camera is at head_rot * (c - head_t) there (game units)
  bool chain_head = false;
  float head_rot[3][3] = {{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}};
  float head_t[3] = {0.f, 0.f, 0.f};
  // with chain_head: the chain's projection half-tangents (the head view widened by the margin)
  // and the game's own before the widening, for 2D consumers (0 when unknown, see howto/xr.md)
  float view_tan_x = 0.f, view_tan_y = 0.f;
  float game_tan_x = 0.f, game_tan_y = 0.f;
  // the runtime's predicted display period (s), 0 without a runtime (emulated session)
  double display_period_s = 0.;
};

/*!
 * Called once per display frame on the render thread with the GL context current. Creates or
 * tears down the OpenXR objects to follow the toggle and polls runtime events.
 */
void update(bool want);

/*!
 * Waits for the runtime's frame slot and begins the frame. Returns the eye targets when the
 * runtime wants this frame rendered, or nullptr (end_frame must still be called if a frame
 * was begun; it is a no-op otherwise). pos_scale multiplies the head's offset from the seated
 * reference (clamped to 0.5 m) before it moves the game camera; 0 keeps rotation only.
 */
FrameTargets* begin_frame(float pos_scale);

/*!
 * Releases the swapchain images and submits the projection layer if the eyes were rendered.
 */
void end_frame();

/*!
 * Scripted head pose for tests (synthetic eye images): replaces the head xrLocateViews reports
 * with (yaw, pitch, roll in degrees, then x, y, z in metres, OpenXR axes). The eyes keep the
 * runtime's head-relative view poses and fov; frames are drawn and submitted as usual, also
 * while the runtime does not ask for them. Turning it on recentres at the identity pose.
 * Called once per display frame before begin_frame.
 */
void set_fake(bool on, const float pose[6], bool quiet = false);

State state();
bool frame_in_flight();

/*!
 * Takes the head pose of the next located frame as the seated reference: its yaw and position
 * become the game camera's. Also happens on its own when a session starts.
 */
void request_recentre();

/*!
 * One-line status for the debug window, e.g. runtime name, recommended size and frame timing.
 */
std::string status_line();

void shutdown();

}  // namespace xr
