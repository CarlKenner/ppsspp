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

#ifdef __INTELLISENSE__
#define HAVE_OCULUSSDK
#endif

#ifdef _WIN32
#include <windows.h>
#undef max
#endif

#ifdef HAVE_OCULUSSDK
#include "OVR_Version.h"
#if OVR_MAJOR_VERSION <= 4
#include "Kernel/OVR_Types.h"
#else
#define OCULUSSDK044ORABOVE
#define OVR_DLL_BUILD
#endif
#include "OVR_CAPI.h"
#if OVR_MAJOR_VERSION >= 5
#include "Extras/OVR_Math.h"
#else
#include "Kernel/OVR_Math.h"

// Detect which version of the Oculus SDK we are using
#if OVR_MINOR_VERSION >= 4
#if OVR_BUILD_VERSION >= 4
#define OCULUSSDK044ORABOVE
#elif OVR_BUILD_VERSION >= 3
#define OCULUSSDK043
#else
#define OCULUSSDK042
#endif
#else
Error, Oculus SDK 0.3.x is no longer supported
#endif

extern "C"
{
	void ovrhmd_EnableHSWDisplaySDKRender(ovrHmd hmd, ovrBool enabled);
}
#endif

#else
#ifdef _WIN32
#include "OculusSystemLibraryHeader.h"
#define OCULUSSDK044ORABOVE
#endif
#endif

#ifdef OVR_MAJOR_VERSION
extern ovrHmd hmd;
extern ovrHmdDesc hmdDesc;
extern ovrFovPort g_eye_fov[2];
extern ovrEyeRenderDesc g_eye_render_desc[2];
#if OVR_MAJOR_VERSION <= 7
extern ovrFrameTiming g_rift_frame_timing;
#endif
#endif

#if defined(OVR_MAJOR_VERSION) && OVR_MAJOR_VERSION >= 7
#define ovrHmd_GetFrameTiming ovr_GetFrameTiming
#define ovrHmd_SubmitFrame ovr_SubmitFrame
#define ovrHmd_GetRenderDesc ovr_GetRenderDesc
#define ovrHmd_DestroySwapTextureSet ovr_DestroySwapTextureSet
#define ovrHmd_DestroyMirrorTexture ovr_DestroyMirrorTexture
#define ovrHmd_SetEnabledCaps ovr_SetEnabledCaps
#define ovrHmd_GetEnabledCaps ovr_GetEnabledCaps
#define ovrHmd_ConfigureTracking ovr_ConfigureTracking
#define ovrHmd_RecenterPose ovr_RecenterPose
#define ovrHmd_Destroy ovr_Destroy
#define ovrHmd_GetFovTextureSize ovr_GetFovTextureSize
#define ovrHmd_GetFloat ovr_GetFloat
#define ovrHmd_SetBool ovr_SetBool
#define ovrHmd_GetTrackingState ovr_GetTrackingState
#endif

