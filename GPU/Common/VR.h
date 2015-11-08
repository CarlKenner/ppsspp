// Copyright (c) 2014- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#pragma once

// Distances are in metres, angles are in degrees.
const float DEFAULT_VR_UNITS_PER_METRE = 1.0f, DEFAULT_VR_FREE_LOOK_SENSITIVITY = 1.0f, DEFAULT_VR_HUD_DISTANCE = 1.5f,
DEFAULT_VR_HUD_THICKNESS = 0.5f, DEFAULT_VR_HUD_3D_CLOSER = 0.5f,
DEFAULT_VR_CAMERA_FORWARD = 0.0f, DEFAULT_VR_CAMERA_PITCH = 0.0f, DEFAULT_VR_AIM_DISTANCE = 7.0f,
DEFAULT_VR_SCREEN_HEIGHT = 2.0f, DEFAULT_VR_SCREEN_DISTANCE = 1.5f, DEFAULT_VR_SCREEN_THICKNESS = 0.5f,
DEFAULT_VR_SCREEN_UP = 0.0f, DEFAULT_VR_SCREEN_RIGHT = 0.0f, DEFAULT_VR_SCREEN_PITCH = 0.0f,
DEFAULT_VR_TIMEWARP_TWEAK = 0.001f, DEFAULT_VR_MIN_FOV = 10.0f, DEFAULT_VR_MOTION_SICKNESS_FOV = 45.0f;
const int DEFAULT_VR_EXTRA_FRAMES = 0;
const int DEFAULT_VR_EXTRA_VIDEO_LOOPS = 0;
const int DEFAULT_VR_EXTRA_VIDEO_LOOPS_DIVIDER = 0;

#ifdef __INTELLISENSE__
//#define HAVE_OPENVR
//#define HAVE_OSVR
#endif

#ifdef _WIN32
#include <windows.h>
#undef max
#endif

#include <atomic>

// Maths
#include "math/lin/matrix4x4.h"
#include "math/math_util.h"
#include "GPU/Math3D.h"
typedef Matrix4x4 Matrix44;
typedef Matrix3x3 Matrix33;
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define RADIANS_TO_DEGREES(rad) ((float) rad * (float) (180.0 / M_PI))
#define DEGREES_TO_RADIANS(deg) ((float) deg * (float) (M_PI / 180.0))

// Main emulator interface
void VR_Init();
void VR_StopRendering();
void VR_Shutdown();
void VR_RecenterHMD();
void VR_NewVRFrame();
void VR_SetGame(std::string id);

// Used for VR rendering
void VR_GetEyePoses();
bool VR_UpdateHeadTrackingIfNeeded();
void VR_GetProjectionHalfTan(float &hmd_halftan);
void VR_GetProjectionMatrices(Matrix4x4 &left_eye, Matrix4x4 &right_eye, float znear, float zfar, bool isOpenGL);
void VR_GetEyePos(float *posLeft, float *posRight);
void VR_GetFovTextureSize(int *width, int *height);

// HMD description and capabilities
bool VR_ShouldUnthrottle();
extern bool g_has_hmd, g_has_rift, g_has_vr920, g_has_steamvr, g_is_direct_mode;
extern bool g_vr_cant_motion_blur, g_vr_must_motion_blur;
extern bool g_vr_has_dynamic_predict, g_vr_has_configure_rendering, g_vr_has_hq_distortion;
extern bool g_vr_should_swap_buffers, g_vr_dont_vsync;
extern bool g_vr_can_async_timewarp;
extern volatile bool g_vr_asyc_timewarp_active;
extern int g_hmd_window_width, g_hmd_window_height, g_hmd_window_x, g_hmd_window_y, g_hmd_refresh_rate;
extern const char *g_hmd_device_name;

// Command line
extern bool g_force_vr, g_prefer_steamvr;

// Tracking
extern Matrix44 g_head_tracking_matrix;
extern float g_head_tracking_position[3];
extern float g_left_hand_tracking_position[3], g_right_hand_tracking_position[3];

// The state of the game scene
extern bool g_fov_changed, g_vr_black_screen;
extern bool g_vr_had_3D_already;
extern float vr_widest_3d_HFOV, vr_widest_3d_VFOV, vr_widest_3d_zNear, vr_widest_3d_zFar;
extern float this_frame_widest_HFOV, this_frame_widest_VFOV, this_frame_widest_zNear, this_frame_widest_zFar;
extern float g_game_camera_pos[3];
extern Matrix44 g_game_camera_rotmat;

// Freelook
void TranslateView(float left_metres, float forward_metres, float down_metres = 0.0f);
void RotateView(float x, float y);
void ScaleView(float scale);
void ResetView();
extern float vr_freelook_speed;
extern float s_fViewTranslationVector[3];

// Debugging info
extern std::atomic<unsigned> g_drawn_vr;
extern std::string g_vr_sdk_version_string;
extern bool g_dumpThisFrame;
extern bool debug_nextScene;

// Pose and frame index are important for headtracking and timewarp
typedef struct {
	float qx, qy, qz, qw, x, y, z;
} VRPose;
extern VRPose g_eye_poses[2];
extern long long g_vr_frame_index;

// Used internally by VROGL.cpp
void VR_ConfigureHMDTracking();
void VR_ConfigureHMDPrediction();
extern bool g_new_tracking_frame;
extern bool g_first_vr_frame;
#ifdef _WIN32
extern LUID *g_hmd_luid;
#endif
