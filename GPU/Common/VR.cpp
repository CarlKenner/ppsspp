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

#ifdef _WIN32
#include <windows.h>
#include <Shobjidl.h>
#include <string>
#include "Common/CommonWindows.h"
#include "GPU/Common/VR920.h"
#include "Windows/W32Util/Misc.h"
#endif

#include "base/logging.h"
#include "Common/Common.h"
#include "Common/StringUtils.h"
#include "Common/StdMutex.h"
#include "Core/Config.h"
#include "Core/CoreParameter.h"
#include "Core/Debugger/SymbolMap.h"
#include "Core/MemMap.h"
#include "Core/MIPS/JitCommon/NativeJit.h"
#include "Core/SaveState.h"
#include "Core/System.h"
#include "GPU/Common/VR.h"
#include "GPU/GPU.h"

#ifdef HAVE_OPENVR
#include <openvr.h>

vr::IVRSystem *m_pHMD;
vr::IVRRenderModels *m_pRenderModels;
vr::IVRCompositor *m_pCompositor;
std::string m_strDriver;
std::string m_strDisplay;
vr::TrackedDevicePose_t m_rTrackedDevicePose[vr::k_unMaxTrackedDeviceCount];
bool m_bUseCompositor = true;
bool m_rbShowTrackedDevice[vr::k_unMaxTrackedDeviceCount];
int m_iValidPoseCount;
#endif

void ClearDebugProj();

#ifdef OVR_MAJOR_VERSION
ovrHmd hmd = nullptr;
ovrHmdDesc hmdDesc;
ovrFovPort g_best_eye_fov[2], g_eye_fov[2], g_last_eye_fov[2];
ovrEyeRenderDesc g_eye_render_desc[2];
#if OVR_MAJOR_VERSION <= 7
ovrFrameTiming g_rift_frame_timing;
#endif
ovrPosef g_eye_poses[2], g_front_eye_poses[2];
long long g_ovr_frameindex;
#if OVR_MAJOR_VERSION >= 7
ovrGraphicsLuid luid;
#endif
#endif

#ifdef _WIN32
LUID *g_hmd_luid = nullptr;
#endif

std::mutex g_vr_lock;

bool g_force_vr = false, g_prefer_steamvr = false;
bool g_has_hmd = false, g_has_rift = false, g_has_vr920 = false, g_has_steamvr = false;
bool g_is_direct_mode = false;
bool g_new_tracking_frame = true;
bool g_new_frame_tracker_for_efb_skip = true;
u32 skip_objects_count = 0;
Matrix44 g_head_tracking_matrix;
float g_head_tracking_position[3];
float g_left_hand_tracking_position[3], g_right_hand_tracking_position[3];
int g_hmd_window_width = 0, g_hmd_window_height = 0, g_hmd_window_x = 0, g_hmd_window_y = 0;
int g_hmd_refresh_rate = 75;
const char *g_hmd_device_name = nullptr;
float g_vr_speed = 0;
float vr_freelook_speed = 0;
bool g_fov_changed = false, g_vr_black_screen = false;
bool g_vr_had_3D_already = false;
float vr_widest_3d_HFOV=0, vr_widest_3d_VFOV=0, vr_widest_3d_zNear=0, vr_widest_3d_zFar=0;
float this_frame_widest_HFOV=0, this_frame_widest_VFOV=0, this_frame_widest_zNear=0, this_frame_widest_zFar=0;
float g_game_camera_pos[3];
Matrix44 g_game_camera_rotmat;
bool debug_newScene = true, debug_nextScene = false;

// freelook
Matrix3x3 s_viewRotationMatrix;
Matrix3x3 s_viewInvRotationMatrix;
float s_fViewTranslationVector[3] = { 0, 0, 0 };
float s_fViewRotation[2] = { 0, 0 };
bool bProjectionChanged = false;
bool bFreeLookChanged = false;
bool g_can_async_timewarp = false;
volatile bool g_asyc_timewarp_active = false;

ControllerStyle vr_left_controller = CS_HYDRA_LEFT, vr_right_controller = CS_HYDRA_RIGHT;

bool g_opcode_replay_enabled = false;
bool g_new_frame_just_rendered = false;
bool g_first_pass = true;
bool g_first_pass_vs_constants = true;
bool g_opcode_replay_frame = false;
bool g_opcode_replay_log_frame = false;
int skipped_opcode_replay_count = 0;

#ifdef _WIN32
static char hmd_device_name[MAX_PATH] = "";
#endif

void VR_NewVRFrame()
{
	//INFO_LOG(VR, "-- NewVRFrame --");
	//g_new_tracking_frame = true;
	if (!g_vr_had_3D_already)
	{
		Matrix44::LoadIdentity(g_game_camera_rotmat);
	}
	g_vr_had_3D_already = false;
	ClearDebugProj();
}

#ifdef HAVE_OPENVR
//-----------------------------------------------------------------------------
// Purpose: Helper to get a string from a tracked device property and turn it
//			into a std::string
//-----------------------------------------------------------------------------
std::string GetTrackedDeviceString(vr::IVRSystem *pHmd, vr::TrackedDeviceIndex_t unDevice, vr::TrackedDeviceProperty prop, vr::TrackedPropertyError *peError = nullptr)
{
	uint32_t unRequiredBufferLen = pHmd->GetStringTrackedDeviceProperty(unDevice, prop, nullptr, 0, peError);
	if (unRequiredBufferLen == 0)
		return "";

	char *pchBuffer = new char[unRequiredBufferLen];
	unRequiredBufferLen = pHmd->GetStringTrackedDeviceProperty(unDevice, prop, pchBuffer, unRequiredBufferLen, peError);
	std::string sResult = pchBuffer;
	delete[] pchBuffer;
	return sResult;
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
bool BInitCompositor()
{
	vr::HmdError peError = vr::HmdError_None;

	m_pCompositor = (vr::IVRCompositor*)vr::VR_GetGenericInterface(vr::IVRCompositor_Version, &peError);

	if (peError != vr::HmdError_None)
	{
		m_pCompositor = nullptr;

		NOTICE_LOG(VR, "Compositor initialization failed with error: %s\n", vr::VR_GetStringForHmdError(peError));
		return false;
	}

	uint32_t unSize = m_pCompositor->GetLastError(NULL, 0);
	if (unSize > 1)
	{
		char* buffer = new char[unSize];
		m_pCompositor->GetLastError(buffer, unSize);
		NOTICE_LOG(VR, "Compositor - %s\n", buffer);
		delete[] buffer;
		return false;
	}

	// change grid room colour
	m_pCompositor->FadeToColor(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, true);

	return true;
}
#endif

bool InitSteamVR()
{
#ifdef HAVE_OPENVR
	// Loading the SteamVR Runtime
	vr::HmdError eError = vr::HmdError_None;
	m_pHMD = vr::VR_Init(&eError, vr::VRApplication_Scene);

	if (eError != vr::HmdError_None)
	{
		m_pHMD = nullptr;
		ERROR_LOG(VR, "Unable to init SteamVR: %s", vr::VR_GetStringForHmdError(eError));
		g_has_steamvr = false;
	}
	else
	{
		m_pRenderModels = (vr::IVRRenderModels *)vr::VR_GetGenericInterface(vr::IVRRenderModels_Version, &eError);
		if (!m_pRenderModels)
		{
			m_pHMD = nullptr;
			vr::VR_Shutdown();

			ERROR_LOG(VR, "Unable to get render model interface: %s", vr::VR_GetStringForHmdError(eError));
			g_has_steamvr = false;
		}
		else
		{
			NOTICE_LOG(VR, "VR_Init Succeeded");
			g_has_steamvr = true;
			g_has_hmd = true;
		}

		u32 m_nWindowWidth = 0;
		u32 m_nWindowHeight = 0;
		m_pHMD->GetWindowBounds(&g_hmd_window_x, &g_hmd_window_y, &m_nWindowWidth, &m_nWindowHeight);
		g_hmd_window_width = m_nWindowWidth;
		g_hmd_window_height = m_nWindowHeight;
		NOTICE_LOG(VR, "SteamVR WindowBounds (%d,%d) %dx%d", g_hmd_window_x, g_hmd_window_y, g_hmd_window_width, g_hmd_window_height);

		std::string m_strDriver = "No Driver";
		std::string m_strDisplay = "No Display";
		m_strDriver = GetTrackedDeviceString(m_pHMD, vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_TrackingSystemName_String);
		m_strDisplay = GetTrackedDeviceString(m_pHMD, vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_SerialNumber_String);
		vr::TrackedPropertyError error;
		g_hmd_refresh_rate = (int)(0.5f + m_pHMD->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float, &error));
		NOTICE_LOG(VR, "SteamVR strDriver = '%s'", m_strDriver.c_str());
		NOTICE_LOG(VR, "SteamVR strDisplay = '%s'", m_strDisplay.c_str());
		NOTICE_LOG(VR, "SteamVR refresh rate = %d Hz", g_hmd_refresh_rate);

		if (m_bUseCompositor)
		{
			if (!BInitCompositor())
			{
				ERROR_LOG(VR, "%s - Failed to initialize SteamVR Compositor!\n", __FUNCTION__);
				g_has_steamvr = false;
			}
		}
		if (g_has_steamvr)
			g_can_async_timewarp = false;
		return g_has_steamvr;
	}
#endif
	return false;
}

bool InitOculusDebugVR()
{
#if defined(OVR_MAJOR_VERSION) && OVR_MAJOR_VERSION <= 6
	if (g_force_vr)
	{
		NOTICE_LOG(VR, "Forcing VR mode, simulating Oculus Rift DK2.");
#if OVR_MAJOR_VERSION >= 6
		if (ovrHmd_CreateDebug(ovrHmd_DK2, &hmd) != ovrSuccess)
			hmd = nullptr;
#else
		hmd = ovrHmd_CreateDebug(ovrHmd_DK2);
#endif
		if (hmd != nullptr)
			g_can_async_timewarp = false;

		return (hmd != nullptr);
	}
#endif
	return false;
}

bool InitOculusHMD()
{
#ifdef OVR_MAJOR_VERSION
	if (hmd)
	{
		// Get more details about the HMD
		//ovrHmd_GetDesc(hmd, &hmdDesc);
#if OVR_MAJOR_VERSION >= 7
		hmdDesc = ovr_GetHmdDesc(hmd);
		ovr_SetEnabledCaps(hmd, ovrHmd_GetEnabledCaps(hmd) | 0);
#else
		hmdDesc = *hmd;
		ovrHmd_SetEnabledCaps(hmd, ovrHmd_GetEnabledCaps(hmd) | ovrHmdCap_DynamicPrediction | ovrHmdCap_LowPersistence);
#endif

#if OVR_MAJOR_VERSION >= 6
		if (OVR_SUCCESS(ovrHmd_ConfigureTracking(hmd, ovrTrackingCap_Orientation | ovrTrackingCap_Position | ovrTrackingCap_MagYawCorrection, 0)))
#else
		if (ovrHmd_ConfigureTracking(hmd, ovrTrackingCap_Orientation | ovrTrackingCap_Position | ovrTrackingCap_MagYawCorrection, 0))
#endif
		{
			g_has_rift = true;
			g_has_hmd = true;
			g_hmd_window_width = hmdDesc.Resolution.w;
			g_hmd_window_height = hmdDesc.Resolution.h;
			g_best_eye_fov[0] = hmdDesc.DefaultEyeFov[0];
			g_best_eye_fov[1] = hmdDesc.DefaultEyeFov[1];
			g_eye_fov[0] = g_best_eye_fov[0];
			g_eye_fov[1] = g_best_eye_fov[1];
			g_last_eye_fov[0] = g_eye_fov[0];
			g_last_eye_fov[1] = g_eye_fov[1];			
#if OVR_MAJOR_VERSION < 6
			// Before Oculus SDK 0.6 we had to size and position the mirror window (or actual window) correctly, at least for OpenGL.
			g_hmd_window_x = hmdDesc.WindowsPos.x;
			g_hmd_window_y = hmdDesc.WindowsPos.y;
			g_Config.iWindowX = g_hmd_window_x;
			g_Config.iWindowY = g_hmd_window_y;
			g_Config.iWindowWidth = g_hmd_window_width;
			g_Config.iWindowHeight = g_hmd_window_height;
			g_is_direct_mode = !(hmdDesc.HmdCaps & ovrHmdCap_ExtendDesktop);
			if (hmdDesc.Type < 6)
				g_hmd_refresh_rate = 60;
			else if (hmdDesc.Type > 6)
				g_hmd_refresh_rate = 90;
			else
				g_hmd_refresh_rate = 75;
#else
#if OVR_MAJOR_VERSION == 6
			g_hmd_refresh_rate = (int)(1.0f / ovrHmd_GetFloat(hmd, "VsyncToNextVsync", 0.f) + 0.5f);
#else
			g_hmd_refresh_rate = (int)(hmdDesc.DisplayRefreshRate + 0.5f);
#endif
			g_hmd_window_x = 0;
			g_hmd_window_y = 0;
			g_is_direct_mode = true;
#endif
#ifdef _WIN32
#if OVR_MAJOR_VERSION < 6
			g_hmd_device_name = hmdDesc.DisplayDeviceName;
#else
			g_hmd_device_name = nullptr;
#endif
			const char *p;
			if (g_hmd_device_name && (p = strstr(g_hmd_device_name, "\\Monitor")))
			{
				size_t n = p - g_hmd_device_name;
				if (n >= MAX_PATH)
					n = MAX_PATH - 1;
				g_hmd_device_name = strncpy(hmd_device_name, g_hmd_device_name, n);
				hmd_device_name[n] = '\0';
			}
#endif
			NOTICE_LOG(VR, "Oculus Rift head tracker started.");
		}
		return g_has_rift;
	}
#endif
	return false;
}

bool InitOculusVR()
{
#ifdef OVR_MAJOR_VERSION
#if OVR_MAJOR_VERSION <= 7
	memset(&g_rift_frame_timing, 0, sizeof(g_rift_frame_timing));
#endif

#if OVR_MAJOR_VERSION >= 7
	ovr_Initialize(nullptr);
	ovrGraphicsLuid luid;
	if (ovr_Create(&hmd, &luid) != ovrSuccess)
		hmd = nullptr;
#ifdef _WIN32
	else
		g_hmd_luid = reinterpret_cast<LUID*>(&luid);
#endif
	if (hmd != nullptr)
		g_can_async_timewarp = !g_Config.bNoAsyncTimewarp;
#elif OVR_MAJOR_VERSION >= 6
	ovr_Initialize(nullptr);
	if (ovrHmd_Create(0, &hmd) != ovrSuccess)
		hmd = nullptr;
	if (hmd != nullptr)
		g_can_async_timewarp = !g_Config.bNoAsyncTimewarp;
#else
	ovr_Initialize();
	hmd = ovrHmd_Create(0);
	g_can_async_timewarp = false;
#endif

	if (!hmd)
		WARN_LOG(VR, "Oculus Rift not detected. Oculus Rift support will not be available.");
	return (hmd != nullptr);
#else
	return false;
#endif
}

bool InitVR920VR()
{
#ifdef _WIN32
	LoadVR920();
	if (g_has_vr920)
	{
		g_has_hmd = true;
		g_hmd_window_width = 800;
		g_hmd_window_height = 600;
		// Todo: find vr920
		g_hmd_window_x = 0;
		g_hmd_window_y = 0;
		g_can_async_timewarp = false;
		g_hmd_refresh_rate = 60; // or 30, depending on how we implement it
		return true;
	}
#endif
	return false;
}

void InitVR()
{
	NOTICE_LOG(VR, "InitVR()");
	g_has_hmd = false;
	g_is_direct_mode = false;
	g_hmd_device_name = nullptr;
	g_has_steamvr = false;
	g_can_async_timewarp = false;
	g_asyc_timewarp_active = false;
#ifdef _WIN32
	g_hmd_luid = nullptr;
#endif

	if (g_prefer_steamvr)
	{
		if (!InitSteamVR() && !InitOculusVR() && !InitVR920VR() && !InitOculusDebugVR())
			g_has_hmd = g_force_vr;
	}
	else
	{
		if (!InitOculusVR() && !InitSteamVR() && !InitVR920VR() && !InitOculusDebugVR())
			g_has_hmd = g_force_vr;
	}
	InitOculusHMD();

	if (g_has_hmd)
	{
		NOTICE_LOG(VR, "HMD detected and initialised");
		//SConfig::GetInstance().strFullscreenResolution =
		//	StringFromFormat("%dx%d", g_hmd_window_width, g_hmd_window_height);
		//SConfig::GetInstance().iRenderWindowXPos = g_hmd_window_x;
		//SConfig::GetInstance().iRenderWindowYPos = g_hmd_window_y;
		//SConfig::GetInstance().iRenderWindowWidth = g_hmd_window_width;
		//SConfig::GetInstance().iRenderWindowHeight = g_hmd_window_height;
		//SConfig::GetInstance().m_special_case = true;
	}
	else
	{
		ERROR_LOG(VR, "No HMD detected!");
		//SConfig::GetInstance().m_special_case = false;
	}
}

void VR_StopRendering()
{
#ifdef _WIN32
	if (g_has_vr920)
	{
		VR920_StopStereo3D();
	}
#endif
#ifdef OVR_MAJOR_VERSION
	// Shut down rendering and release resources (by passing NULL)
	if (g_has_rift)
	{
#if OVR_MAJOR_VERSION >= 6
		for (int i = 0; i < ovrEye_Count; ++i)
			g_eye_render_desc[i] = ovrHmd_GetRenderDesc(hmd, (ovrEyeType)i, g_eye_fov[i]);
#else
		ovrHmd_ConfigureRendering(hmd, nullptr, 0, g_eye_fov, g_eye_render_desc);
#endif
	}
#endif
}

void ShutdownVR()
{
	g_can_async_timewarp = false;
#ifdef HAVE_OPENVR
	if (g_has_steamvr && m_pHMD)
	{
		g_has_steamvr = false;
		m_pHMD = nullptr;
		// crashes if OpenGL
		vr::VR_Shutdown();
	}
#endif
#ifdef OVR_MAJOR_VERSION
	if (hmd)
	{
#if OVR_MAJOR_VERSION < 6
		// on my computer, on runtime 0.4.2, the Rift won't switch itself off without this:
		if (g_is_direct_mode)
			ovrHmd_SetEnabledCaps(hmd, ovrHmdCap_DisplayOff);
#endif
		ovrHmd_Destroy(hmd);
		g_has_rift = false;
		g_has_hmd = false;
		g_is_direct_mode = false;
		NOTICE_LOG(VR, "Oculus Rift shut down.");
	}
	ovr_Shutdown();
#endif
}

void VR_RecenterHMD()
{
#ifdef HAVE_OPENVR
	if (g_has_steamvr && m_pHMD)
	{
		m_pHMD->ResetSeatedZeroPose();
	}
#endif
#ifdef OVR_MAJOR_VERSION
	if (g_has_rift)
	{
		ovrHmd_RecenterPose(hmd);
	}
#endif
}

void VR_ConfigureHMDTracking()
{
#ifdef OVR_MAJOR_VERSION
	if (g_has_rift)
	{
		int cap = 0;
		if (g_Config.bOrientationTracking)
			cap |= ovrTrackingCap_Orientation;
		if (g_Config.bMagYawCorrection)
			cap |= ovrTrackingCap_MagYawCorrection;
		if (g_Config.bPositionTracking)
			cap |= ovrTrackingCap_Position;
		ovrHmd_ConfigureTracking(hmd, cap, 0);
}
#endif
}

void VR_ConfigureHMDPrediction()
{
#ifdef OVR_MAJOR_VERSION
	if (g_has_rift)
	{
#if OVR_MAJOR_VERSION <= 5
		int caps = ovrHmd_GetEnabledCaps(hmd) & ~(ovrHmdCap_DynamicPrediction | ovrHmdCap_LowPersistence | ovrHmdCap_NoMirrorToWindow);
#else
#if OVR_MAJOR_VERSION >= 7
		int caps = ovrHmd_GetEnabledCaps(hmd) & ~(0);
#else
		int caps = ovrHmd_GetEnabledCaps(hmd) & ~(ovrHmdCap_DynamicPrediction | ovrHmdCap_LowPersistence);
#endif
#endif
#if OVR_MAJOR_VERSION <= 6
		if (g_Config.bLowPersistence)
			caps |= ovrHmdCap_LowPersistence;
		if (g_Config.bDynamicPrediction)
			caps |= ovrHmdCap_DynamicPrediction;
#if OVR_MAJOR_VERSION <= 5
		if (g_Config.bNoMirrorToWindow)
			caps |= ovrHmdCap_NoMirrorToWindow;
#endif
#endif
		ovrHmd_SetEnabledCaps(hmd, caps);
	}
#endif
}

void VR_GetEyePoses()
{
#ifdef OVR_MAJOR_VERSION
	if (g_has_rift)
	{
#ifdef OCULUSSDK042
		g_eye_poses[ovrEye_Left] = ovrHmd_GetEyePose(hmd, ovrEye_Left);
		g_eye_poses[ovrEye_Right] = ovrHmd_GetEyePose(hmd, ovrEye_Right);
#else
		ovrVector3f useHmdToEyeViewOffset[2] = { g_eye_render_desc[0].HmdToEyeViewOffset, g_eye_render_desc[1].HmdToEyeViewOffset };
#if OVR_MAJOR_VERSION >= 7
#if OVR_MAJOR_VERSION <= 7
		ovr_GetEyePoses(hmd, g_ovr_frameindex, useHmdToEyeViewOffset, g_eye_poses, nullptr);
#endif
#else
		ovrHmd_GetEyePoses(hmd, g_ovr_frameindex, useHmdToEyeViewOffset, g_eye_poses, nullptr);
#endif
#endif
	}
#endif
#if HAVE_OPENVR
	if (g_has_steamvr)
	{
		if (m_pCompositor)
		{
			m_pCompositor->WaitGetPoses(m_rTrackedDevicePose, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
		}
	}
#endif
}

#ifdef HAVE_OPENVR
//-----------------------------------------------------------------------------
// Purpose: Processes a single VR event
//-----------------------------------------------------------------------------
void ProcessVREvent(const vr::VREvent_t & event)
{
	switch (event.eventType)
	{
	case vr::VREvent_TrackedDeviceActivated:
	{
		//SetupRenderModelForTrackedDevice(event.trackedDeviceIndex);
		NOTICE_LOG(VR, "Device %u attached. Setting up render model.\n", event.trackedDeviceIndex);
		break;
	}
	case vr::VREvent_TrackedDeviceDeactivated:
	{
		NOTICE_LOG(VR, "Device %u detached.\n", event.trackedDeviceIndex);
		break;
	}
	case vr::VREvent_TrackedDeviceUpdated:
	{
		NOTICE_LOG(VR, "Device %u updated.\n", event.trackedDeviceIndex);
		break;
	}
	}
}
#endif

#ifdef OVR_MAJOR_VERSION
void UpdateOculusHeadTracking()
{
	// On Oculus SDK 0.6 and above, we start the next frame the first time we read the head tracking.
	// On SDK 0.5 and below, this is done in BeginFrame instead.
#if OVR_MAJOR_VERSION >= 6
	++g_ovr_frameindex;
#endif
	// we can only call GetEyePose between BeginFrame and EndFrame
#ifdef OCULUSSDK042
	g_vr_lock.lock();
	g_eye_poses[ovrEye_Left] = ovrHmd_GetEyePose(hmd, ovrEye_Left);
	g_eye_poses[ovrEye_Right] = ovrHmd_GetEyePose(hmd, ovrEye_Right);
	g_vr_lock.unlock();
	OVR::Posef pose = g_eye_poses[ovrEye_Left];
#else
	ovrVector3f useHmdToEyeViewOffset[2] = { g_eye_render_desc[0].HmdToEyeViewOffset, g_eye_render_desc[1].HmdToEyeViewOffset };
#if OVR_MAJOR_VERSION >= 8
	double display_time = ovr_GetPredictedDisplayTime(hmd, g_ovr_frameindex);
	ovrTrackingState state = ovr_GetTrackingState(hmd, display_time, false);
	ovr_CalcEyePoses(state.HeadPose.ThePose, useHmdToEyeViewOffset, g_eye_poses);
	OVR::Posef pose = state.HeadPose.ThePose;
#elif OVR_MAJOR_VERSION >= 6
	ovrFrameTiming timing = ovrHmd_GetFrameTiming(hmd, g_ovr_frameindex);
	ovrTrackingState state = ovrHmd_GetTrackingState(hmd, timing.DisplayMidpointSeconds);
	ovr_CalcEyePoses(state.HeadPose.ThePose, useHmdToEyeViewOffset, g_eye_poses);
	OVR::Posef pose = state.HeadPose.ThePose;
#else
	ovrHmd_GetEyePoses(hmd, g_ovr_frameindex, useHmdToEyeViewOffset, g_eye_poses, nullptr);
	OVR::Posef pose = g_eye_poses[ovrEye_Left];
#endif
#endif
	//ovrTrackingState ss = ovrHmd_GetTrackingState(hmd, g_rift_frame_timing.ScanoutMidpointSeconds);
	//if (ss.StatusFlags & (ovrStatus_OrientationTracked | ovrStatus_PositionTracked))
	{
		//OVR::Posef pose = ss.HeadPose.ThePose;
		float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
		pose.Rotation.GetEulerAngles<OVR::Axis_Y, OVR::Axis_X, OVR::Axis_Z>(&yaw, &pitch, &roll);

		float x = 0, y = 0, z = 0;
		roll = -RADIANS_TO_DEGREES(roll);  // ???
		pitch = -RADIANS_TO_DEGREES(pitch); // should be degrees down
		yaw = -RADIANS_TO_DEGREES(yaw);   // should be degrees right
		x = pose.Translation.x;
		y = pose.Translation.y;
		z = pose.Translation.z;
		g_head_tracking_position[0] = -x;
		g_head_tracking_position[1] = -y;
#if OVR_MAJOR_VERSION <= 4
		g_head_tracking_position[2] = 0.06f - z;
#else
		g_head_tracking_position[2] = -z;
#endif
		Matrix33 m, yp, ya, p, r;
		Matrix33::RotateY(ya, DEGREES_TO_RADIANS(yaw));
		Matrix33::RotateX(p, DEGREES_TO_RADIANS(pitch));
		Matrix33::Multiply(p, ya, yp);
		Matrix33::RotateZ(r, DEGREES_TO_RADIANS(roll));
		Matrix33::Multiply(r, yp, m);
		Matrix44::LoadMatrix33(g_head_tracking_matrix, m);
	}
}
#endif

#ifdef HAVE_OPENVR
void UpdateSteamVRHeadTracking()
{
	// Process SteamVR events
	vr::VREvent_t event;
	while (m_pHMD->PollNextEvent(&event))
	{
		ProcessVREvent(event);
	}

	// Process SteamVR controller state
	for (vr::TrackedDeviceIndex_t unDevice = 0; unDevice < vr::k_unMaxTrackedDeviceCount; unDevice++)
	{
		vr::VRControllerState_t state;
		if (m_pHMD->GetControllerState(unDevice, &state))
		{
			m_rbShowTrackedDevice[unDevice] = state.ulButtonPressed == 0;
		}
	}
	float fSecondsUntilPhotons = 0.0f;
	m_pHMD->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseSeated, fSecondsUntilPhotons, m_rTrackedDevicePose, vr::k_unMaxTrackedDeviceCount);
	m_iValidPoseCount = 0;
	//for ( int nDevice = 0; nDevice < vr::k_unMaxTrackedDeviceCount; ++nDevice )
	//{
	//	if ( m_rTrackedDevicePose[nDevice].bPoseIsValid )
	//	{
	//		m_iValidPoseCount++;
	//		//m_rTrackedDevicePose[nDevice].mDeviceToAbsoluteTracking;
	//	}
	//}

	if (m_rTrackedDevicePose[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
	{
		float x = m_rTrackedDevicePose[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking.m[0][3];
		float y = m_rTrackedDevicePose[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking.m[1][3];
		float z = m_rTrackedDevicePose[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking.m[2][3];
		g_head_tracking_position[0] = -x;
		g_head_tracking_position[1] = -y;
		g_head_tracking_position[2] = -z;
		Matrix33 m;
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
				m.data[r * 3 + c] = m_rTrackedDevicePose[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking.m[c][r];
		Matrix44::LoadMatrix33(g_head_tracking_matrix, m);
	}
}
#endif

#ifdef _WIN32
void UpdateVuzixHeadTracking()
{
	LONG ya = 0, p = 0, r = 0;
	if (Vuzix_GetTracking(&ya, &p, &r) == ERROR_SUCCESS)
	{
		float yaw = -ya * 180.0f / 32767.0f;
		float pitch = p * -180.0f / 32767.0f;
		float roll = r * 180.0f / 32767.0f;
		// todo: use head and neck model
		float x = 0;
		float y = 0;
		float z = 0;
		Matrix33 m, yp, ya, p, r;
		Matrix33::RotateY(ya, DEGREES_TO_RADIANS(yaw));
		Matrix33::RotateX(p, DEGREES_TO_RADIANS(pitch));
		Matrix33::Multiply(p, ya, yp);
		Matrix33::RotateZ(r, DEGREES_TO_RADIANS(roll));
		Matrix33::Multiply(r, yp, m);
		Matrix44::LoadMatrix33(g_head_tracking_matrix, m);
		g_head_tracking_position[0] = -x;
		g_head_tracking_position[1] = -y;
		g_head_tracking_position[2] = -z;
	}
}
#endif

bool UpdateHeadTrackingIfNeeded()
{
	if (g_new_tracking_frame) {
		g_new_tracking_frame = false;
#ifdef _WIN32
		if (g_has_vr920 && Vuzix_GetTracking)
			UpdateVuzixHeadTracking();
#endif
#ifdef HAVE_OPENVR
		if (g_has_steamvr)
			UpdateSteamVRHeadTracking();
#endif
#ifdef OVR_MAJOR_VERSION
		if (g_has_rift)
			UpdateOculusHeadTracking();
#endif
		return true;
	} else {
		return false;
	}
}

void VR_GetProjectionHalfTan(float &hmd_halftan)
{
#ifdef OVR_MAJOR_VERSION
	if (g_has_rift)
	{
		hmd_halftan = fabs(g_eye_fov[0].LeftTan);
		if (fabs(g_eye_fov[0].RightTan) > hmd_halftan)
			hmd_halftan = fabs(g_eye_fov[0].RightTan);
		if (fabs(g_eye_fov[0].UpTan) > hmd_halftan)
			hmd_halftan = fabs(g_eye_fov[0].UpTan);
		if (fabs(g_eye_fov[0].DownTan) > hmd_halftan)
			hmd_halftan = fabs(g_eye_fov[0].DownTan);
	}
	else
#endif
		if (g_has_steamvr)
		{
			// rough approximation, can't be bothered to work this out properly
			hmd_halftan = tan(DEGREES_TO_RADIANS(100.0f / 2));
		}
		else
		{
			hmd_halftan = tan(DEGREES_TO_RADIANS(32.0f / 2))*3.0f / 4.0f;
		}
}

void VR_GetProjectionMatrices(Matrix44 &left_eye, Matrix44 &right_eye, float znear, float zfar, bool isOpenGL)
{
#ifdef OVR_MAJOR_VERSION
	if (g_has_rift)
	{
#if OVR_MAJOR_VERSION >= 5
		unsigned flags = ovrProjection_None;
		flags |= ovrProjection_RightHanded; // is this right for Dolphin VR?
		if (isOpenGL)
			flags |= ovrProjection_ClipRangeOpenGL;
		if (isinf(zfar))
			flags |= ovrProjection_FarClipAtInfinity;
#else
		bool flags = true; // right handed
#endif
		//INFO_LOG(VR, "GetProjectionMatrices(%g, %g, %d)", znear, zfar, flags);
		ovrMatrix4f rift_left = ovrMatrix4f_Projection(g_eye_fov[0], znear, zfar, flags);
		ovrMatrix4f rift_right = ovrMatrix4f_Projection(g_eye_fov[1], znear, zfar, flags);
		Matrix44::Set(left_eye, rift_left.M[0]);
		Matrix44::Set(right_eye, rift_right.M[0]);
		// Oculus don't give us the correct z values for infinite zfar
		if (isinf(zfar))
		{
			left_eye.zz = -1.0f;
			left_eye.zw = -2.0f * znear;
			right_eye.zz = left_eye.zz;
			right_eye.zw = left_eye.zw;
		}
	}
	else
#endif
#ifdef HAVE_OPENVR
		if (g_has_steamvr)
		{
			vr::GraphicsAPIConvention flags = isOpenGL ? vr::API_OpenGL : vr::API_DirectX;
			vr::HmdMatrix44_t mat = m_pHMD->GetProjectionMatrix(vr::Eye_Left, znear, zfar, flags);
			for (int r = 0; r < 4; ++r)
				for (int c = 0; c < 4; ++c)
					left_eye.data[r * 4 + c] = mat.m[r][c];
			mat = m_pHMD->GetProjectionMatrix(vr::Eye_Right, znear, zfar, vr::API_DirectX);
			for (int r = 0; r < 4; ++r)
				for (int c = 0; c < 4; ++c)
					right_eye.data[r * 4 + c] = mat.m[r][c];
		}
		else
#endif
		{
			Matrix44::LoadIdentity(left_eye);
			left_eye.data[10] = -znear / (zfar - znear);
			left_eye.data[11] = -zfar*znear / (zfar - znear);
			left_eye.data[14] = -1.0f;
			left_eye.data[15] = 0.0f;
			// 32 degrees HFOV, 4:3 aspect ratio
			left_eye.data[0 * 4 + 0] = 1.0f / tan(32.0f / 2.0f * 3.1415926535f / 180.0f);
			left_eye.data[1 * 4 + 1] = 4.0f / 3.0f * left_eye.data[0 * 4 + 0];
			Matrix44::Set(right_eye, left_eye.data);
		}
}

void VR_GetEyePos(float *posLeft, float *posRight)
{
#ifdef OVR_MAJOR_VERSION
	if (g_has_rift)
	{
#ifdef OCULUSSDK042
		posLeft[0] = g_eye_render_desc[0].ViewAdjust.x;
		posLeft[1] = g_eye_render_desc[0].ViewAdjust.y;
		posLeft[2] = g_eye_render_desc[0].ViewAdjust.z;
		posRight[0] = g_eye_render_desc[1].ViewAdjust.x;
		posRight[1] = g_eye_render_desc[1].ViewAdjust.y;
		posRight[2] = g_eye_render_desc[1].ViewAdjust.z;
#else
		posLeft[0] = g_eye_render_desc[0].HmdToEyeViewOffset.x;
		posLeft[1] = g_eye_render_desc[0].HmdToEyeViewOffset.y;
		posLeft[2] = g_eye_render_desc[0].HmdToEyeViewOffset.z;
		posRight[0] = g_eye_render_desc[1].HmdToEyeViewOffset.x;
		posRight[1] = g_eye_render_desc[1].HmdToEyeViewOffset.y;
		posRight[2] = g_eye_render_desc[1].HmdToEyeViewOffset.z;
#if OVR_MAJOR_VERSION >= 6
		for (int i = 0; i<3; ++i)
		{
			posLeft[i] = -posLeft[i];
			posRight[i] = -posRight[i];
		}
#endif
#endif
	}
	else
#endif
#ifdef HAVE_OPENVR
		if (g_has_steamvr)
		{
			// assume 62mm IPD
			posLeft[0] = 0.031f;
			posRight[0] = -0.031f;
			posLeft[1] = posRight[1] = 0;
			posLeft[2] = posRight[2] = 0;
		}
		else
#endif
		{
			// assume 62mm IPD
			posLeft[0] = 0.031f;
			posRight[0] = -0.031f;
			posLeft[1] = posRight[1] = 0;
			posLeft[2] = posRight[2] = 0;
		}
}

void VR_GetFovTextureSize(int *width, int *height)
{
#if defined(OVR_MAJOR_VERSION)
	if (g_has_rift)
	{
		ovrSizei size = ovrHmd_GetFovTextureSize(hmd, ovrEye_Left, g_eye_fov[0], 1.0f);
		*width = size.w;
		*height = size.h;
	}
#endif
}

bool VR_GetLeftHydraPos(float *pos)
{
	pos[0] = -0.15f;
	pos[1] = -0.30f;
	pos[2] = -0.4f;
	return true;
}

bool VR_GetRightHydraPos(float *pos)
{
	pos[0] = 0.15f;
	pos[1] = -0.30f;
	pos[2] = -0.4f;
	return true;
}

void VR_SetGame()
{
	vr_left_controller = CS_PSP_LEFT;
	vr_right_controller = CS_PSP_RIGHT;
}

ControllerStyle VR_GetHydraStyle(int hand)
{
	if (hand)
		return vr_right_controller;
	else
		return vr_left_controller;
}

void ScaleView(float scale)
{
	// keep the camera in the same virtual world location when scaling the virtual world
	for (int i = 0; i < 3; i++)
		s_fViewTranslationVector[i] *= scale;

	if (s_fViewTranslationVector[0] || s_fViewTranslationVector[1] || s_fViewTranslationVector[2])
		bFreeLookChanged = true;
	else
		bFreeLookChanged = false;

	bProjectionChanged = true;
}

// Moves the freelook camera a number of scaled metres relative to the current freelook camera direction
void TranslateView(float left_metres, float forward_metres, float down_metres)
{
	float result[3];
	float vector[3] = { left_metres, down_metres, forward_metres };

	// use scaled metres in VR, or real metres otherwise
	if (g_has_hmd && g_Config.bEnableVR && g_Config.bScaleFreeLook)
		for (int i = 0; i < 3; ++i)
			vector[i] *= g_Config.fScale;

	Matrix33::Multiply(s_viewInvRotationMatrix, vector, result);

	for (int i = 0; i < 3; i++)
	{
		s_fViewTranslationVector[i] += result[i];
		vr_freelook_speed += result[i];
	}

	if (s_fViewTranslationVector[0] || s_fViewTranslationVector[1] || s_fViewTranslationVector[2])
		bFreeLookChanged = true;
	else
		bFreeLookChanged = false;

	bProjectionChanged = true;
}

void RotateView(float x, float y)
{
	s_fViewRotation[0] += x;
	s_fViewRotation[1] += y;

	Matrix33 mx;
	Matrix33 my;
	Matrix33::RotateX(mx, s_fViewRotation[1]);
	Matrix33::RotateY(my, s_fViewRotation[0]);
	Matrix33::Multiply(mx, my, s_viewRotationMatrix);

	// reverse rotation
	Matrix33::RotateX(mx, -s_fViewRotation[1]);
	Matrix33::RotateY(my, -s_fViewRotation[0]);
	Matrix33::Multiply(my, mx, s_viewInvRotationMatrix);

	if (s_fViewRotation[0] || s_fViewRotation[1])
		bFreeLookChanged = true;
	else
		bFreeLookChanged = false;

	bProjectionChanged = true;
}

void ResetView()
{
	memset(s_fViewTranslationVector, 0, sizeof(s_fViewTranslationVector));
	Matrix33::LoadIdentity(s_viewRotationMatrix);
	Matrix33::LoadIdentity(s_viewInvRotationMatrix);
	s_fViewRotation[0] = s_fViewRotation[1] = 0.0f;

	bFreeLookChanged = false;
	bProjectionChanged = true;
}

// Bruteforce culling codes by checking every function in the game to see if disabling it changes how much is drawn
bool g_bruteforcing = false;
extern bool g_TakeScreenshot;
extern char g_ScreenshotName[2048];
extern int LoadCountThisSession;
bool BruteForceInitialised = false;
std::vector<u32> function_addrs;
std::vector<u32> function_sizes;
std::vector<std::string> function_names;

// only checks for conditial jumps, not conditional assignments
bool isMipsIf(u32 value)
{
	s16 offset;
	switch ((value & 0xFC000000) >> 24) {
	case 0x04: // bgez, bltz, bgezal, bltzal, bgezall, or bltzall - al (or all) when this bit is set
		switch ((value & 0x1F0000) >> 16) {
		case 0x00: // bltz
		case 0x01: // bgez
		case 0x02: // bltzl
		case 0x03: // bgezl
			offset = (s16)(value & 0x0000FFFF);
			return offset > 0;
		default:
			return false;
		}
	case 0x10: // beq
	case 0x14: // bne
	case 0x18: // blez
	case 0x1C: // bgtz
	case 0x50: // beql
	case 0x54: // bnel
	case 0x58: // blezl
	case 0x5C: // bgtzl
		offset = (s16)(value & 0x0000FFFF);
		return offset > 0;
	case 0x44: // floating point
		if ((value & 0xFFE00000) == 0x450) { // bc1f, bc1t, bc1fl, bc1tl
			offset = (s16)(value & 0x0000FFFF);
			return offset > 0;
		}
		return false;
	default:
		return false;
	}
}

// branches in Mips can be "likely", which nullifies the next instruction if falling through
bool isMipsLikely(u32 value)
{
	switch ((value & 0xFC000000) >> 24) {
	case 0x04: // bgez, bltz, bgezl, bltzl, bgezal, bltzal, bgezall, or bltzall
		switch ((value & 0x1F0000) >> 16) {
		case 0x02: // bltzl
		case 0x03: // bgezl
			return true;
		default:
			return false;
		}
	case 0x50: // beql
	case 0x54: // bnel
	case 0x58: // blezl
	case 0x5C: // bgtzl
		return true;
	case 0x44: // floating point
		if ((value & 0xFFE00000) == 0x450) // bc1f, bc1t, bc1fl, bc1tl
			return (value & 0x20000) != 0; // bc1fl or bc1tl
		return false;
	default:
		return false;
	}
}

void MakeMipsFunctionReturn(u32 addr, u32 size, s16 result)
{
	addr &= ~3;
	if (size < 12) {
		currentMIPS->InvalidateICache(addr, 8);
		Memory::Write_U32(0x03E00008, addr);     // jr ra    // return
		Memory::Write_U32(0x20020000 | (result & 0xFFFF), addr + 4); // li v0, result // int_result = result
	} else if (result==0) {
		currentMIPS->InvalidateICache(addr, 12);
		Memory::Write_U32(0x44800000, addr);     // mtc1 zero, f0 // float_result = 0.0f
		Memory::Write_U32(0x03E00008, addr + 4);     // jr ra    // return
		Memory::Write_U32(0x20020000 | (result & 0xFFFF), addr + 8); // li v0, result // int_result = result
	} else if (size < 16) {
		currentMIPS->InvalidateICache(addr, 12);
		Memory::Write_U32(0x449F0000, addr);     // mtc1 ra, f0 // float_result = a very small positive non-zero value
		Memory::Write_U32(0x03E00008, addr + 4);     // jr ra    // return
		Memory::Write_U32(0x20020000 | (result & 0xFFFF), addr + 8); // li v0, result // int_result = result
	} else {
		currentMIPS->InvalidateICache(addr, 16);
		Memory::Write_U32(0x3C01447A, addr);     // lui at, 0x447a // at = 1000.0f
		Memory::Write_U32(0x44810000, addr + 4); // mtc1 at, f0    // float_result = at
		Memory::Write_U32(0x03E00008, addr + 8); // jr ra          // return
		Memory::Write_U32(0x20020000 | (result & 0xFFFF), addr + 12); // li v0, result // int_result = result
	}
}

void MakeMipsIfAlways(u32 addr, u32 value, bool doif)
{
	addr &= ~3;
	if (doif) {
		s32 offset = (s32)((s16)(value & 0x0000FFFF)) << 2;
		u32 target = addr + 4 + offset;
		value = 0x08000000 | ((target >> 2) & 0x00FFFFFF); // j addr
		currentMIPS->InvalidateICache(addr, 4);
		Memory::Write_U32(value, addr);
	} else {
		// In MIPS, the next instruction is usually executed before the jump
		// But for a branch likely we also should nop out the next instruction because it is said to be nullified when we fall through
		if (isMipsLikely(value)) {
			currentMIPS->InvalidateICache(addr, 8);
			Memory::Write_U64(0, addr); // nop, nop (do nothing)
		} else {
			currentMIPS->InvalidateICache(addr, 4);
			Memory::Write_U32(0, addr); // nop, nop (do nothing)
		}
	}
}

void VR_BruteForceResume()
{
	g_bruteforcing = true;
	g_Config.bEnableVR = false;
	g_Config.bHardwareTransform = true;
	g_Config.iAnisotropyLevel = 0;
	g_Config.iTexScalingLevel = 0;
	g_Config.iInternalResolution = 1;
	g_Config.bEnableSound = false;
	g_Config.bVSync = false;
	g_Config.bEnableLogging = false;
	PSP_CoreParameter().unthrottle = true;
	for (int i = 0; i < 3; ++i)
		s_fViewTranslationVector[i] = g_Config.BruteForceFreeLook[i];

	int count = 0;
	for (auto it = g_symbolMap->activeFunctions.begin(), end = g_symbolMap->activeFunctions.end(); it != end; ++it) {
		const SymbolMap::FunctionEntry& entry = it->second;
		// skip functions with length 4, which are probably just a return and nothing else, and are impossible to patch
		if (entry.size <= 4)
			continue;
		const char* name = g_symbolMap->GetLabelName(it->first);
		char temp[2048];
		if (name != NULL)
			sprintf(temp, "%s", name);
		else
			sprintf(temp, "0x%08X", it->first);
		// only add this function to the list if it isn't a known system function
		if (strncmp(temp, "sce", 3) && strncmp(temp, "zz_sce", 6) && strncmp(temp, "zz__sce", 7) && strncmp(temp, "zz___sce", 7)
			&& strncmp(temp, "_malloc", 7) && strncmp(temp, "str", 3) && strncmp(temp, "memset", 6) && strncmp(temp, "memcpy", 6)) {
			NOTICE_LOG(VR, "Func[%d]: %s at %8X", count, temp, it->first);
			function_names.push_back(temp);
			function_addrs.push_back(it->first);
			function_sizes.push_back(entry.size);
			++count;
			if (g_Config.bBruteForceIfs) {
				// go through every instruction, except the last three which have to be a non-jump, ja $ra, and a non-jump,
				// looking for conditional branches we could force to always true or false
				for (u32 addr = it->first; addr < it->first + entry.size - 12; addr += 4) {
					u32 value = Memory::Read_U32(addr);
					if (isMipsIf(value)) {
						function_names.push_back("if");
						function_addrs.push_back(addr);
						function_sizes.push_back(0);
						++count;
					}
				}
			}
		}
	}
	NOTICE_LOG(VR, "Func Count = %d", count);
	g_Config.bEnableLogging = false;
	g_Config.BruteForceFunctionCount = count;
	BruteForceInitialised = true;
}

void VR_BruteForceStart(bool TestIfs)
{
	BruteForceInitialised = false;
	if (g_Config.bBruteForcing)
	{
		VR_BruteForceCancel();
		return;
	}

	for (int i = 0; i < 3; ++i)
		g_Config.BruteForceFreeLook[i] = s_fViewTranslationVector[i];
	g_Config.bBruteForceIfs = TestIfs;
	g_Config.BruteForce_bEnableVR = g_Config.bEnableVR;
	g_Config.BruteForce_bEnableLogging = g_Config.bEnableLogging;
	g_Config.BruteForce_bVSync = g_Config.bVSync;
	g_Config.BruteForce_bHardwareTransform = g_Config.bHardwareTransform;
	g_Config.BruteForce_bEnableSound = g_Config.bEnableSound;
	g_Config.BruteForce_iInternalResolution = g_Config.iInternalResolution;
	g_Config.BruteForce_iAnisotropyLevel = g_Config.iAnisotropyLevel;
	g_Config.BruteForce_iTexScalingLevel = g_Config.iTexScalingLevel;
	g_Config.sBruteForceFileName = PSP_CoreParameter().fileToStart;
	VR_BruteForceResume();

	if (TestIfs)
		g_Config.BruteForceFramesToRunFor = 6;
	else
		g_Config.BruteForceFramesToRunFor = 4;
	SaveState::SaveSlot(PSP_CoreParameter().fileToStart, SaveState::SAVESTATESLOTS, nullptr);
	g_Config.BruteForceCurrentFunctionIndex = -2;
	g_Config.BruteForceFramesLeft = g_Config.BruteForceFramesToRunFor;
	g_Config.bBruteForcing = true;
	g_bruteforcing = true;
	g_Config.Save();
}

void VR_BruteForceRestoreSettings()
{
	g_bruteforcing = false;
	g_Config.bBruteForcing = false;
	g_Config.bEnableVR = g_Config.BruteForce_bEnableVR;
	g_Config.bEnableLogging = g_Config.BruteForce_bEnableLogging;
	g_Config.bVSync = g_Config.BruteForce_bVSync;
	g_Config.bHardwareTransform = g_Config.BruteForce_bHardwareTransform;
	g_Config.bEnableSound = g_Config.BruteForce_bEnableSound;
	g_Config.iInternalResolution = g_Config.BruteForce_iInternalResolution;
	g_Config.iAnisotropyLevel = g_Config.BruteForce_iAnisotropyLevel;
	g_Config.iTexScalingLevel = g_Config.BruteForce_iTexScalingLevel;
	PSP_CoreParameter().unthrottle = false;
	g_Config.Save();
}

void VR_BruteForceCancel()
{
	VR_BruteForceRestoreSettings();
}

void VR_BruteForceFinish()
{
	VR_BruteForceRestoreSettings();
	ExitProcess(0);
}

void CPU_Shutdown();

static bool isDifferent = false;
static int prev_verts = 0;

void VR_BruteForceBeginFrame()
{
	if (!g_Config.bBruteForcing)
		return;
	try {
		bFreeLookChanged = true;
		SaveState::Process();
		if (g_Config.BruteForceFramesLeft == 0) {
			if (isDifferent && g_Config.BruteForceReturnCode == 0) {
				++g_Config.BruteForceReturnCode;
			}
			else {
				++g_Config.BruteForceCurrentFunctionIndex;
				g_Config.BruteForceReturnCode = 0;
				prev_verts = 0;
			}
			gpuStats.numVertsSubmitted = 0;
			SaveState::LoadSlot(PSP_CoreParameter().fileToStart, SaveState::SAVESTATESLOTS, nullptr);
			SaveState::Process();
			if (g_Config.BruteForceCurrentFunctionIndex == g_Config.BruteForceFunctionCount) {
				// we already finished bruteforcing
				VR_BruteForceFinish();
				return;
			} else if (LoadCountThisSession > 1000) {
				// There's a memory leak with loading save states. 
				// We start to run out of memory after about 4000, so restart PPSSPP every 1000 loads.
				g_Config.BruteForceFramesLeft = g_Config.BruteForceFramesToRunFor;
				g_Config.Save();
				W32Util::ExitAndRestart();
				return;
			}
			else if (!BruteForceInitialised) {
				VR_BruteForceResume();
			}

			if (g_Config.BruteForceCurrentFunctionIndex < 0) {
				// Don't patch anything when doing the original state
			} else if (function_sizes[g_Config.BruteForceCurrentFunctionIndex]==0) {
				// Patch if statement
				u32 addr = function_addrs[g_Config.BruteForceCurrentFunctionIndex];
				u32 value = Memory::Read_U32(addr);
				MakeMipsIfAlways(addr, value, g_Config.BruteForceReturnCode != 0);
			} else {
				// Patch function
				u32 addr = function_addrs[g_Config.BruteForceCurrentFunctionIndex];
				MakeMipsFunctionReturn(addr, function_sizes[g_Config.BruteForceCurrentFunctionIndex], g_Config.BruteForceReturnCode);
			}
			g_Config.BruteForceFramesLeft = g_Config.BruteForceFramesToRunFor;
		}
		else if (!BruteForceInitialised) {
			VR_BruteForceResume();
		}
	} catch (std::bad_alloc) {
		VR_BruteForceCrash(true);
	} catch (...) {
		VR_BruteForceCrash(false);
	}
	// save that we are up to the next frame, in case we crash without detecting it
	int temp = g_Config.BruteForceFramesLeft;
	g_Config.BruteForceFramesLeft = 0;
	//g_Config.Save();
	g_Config.BruteForceFramesLeft = temp;
}

static std::string prev_name;

void VR_BruteForceEndFrame()
{
	if (!g_Config.bBruteForcing)
		return;
	--g_Config.BruteForceFramesLeft;
	// if end of function
	if (g_Config.BruteForceFramesLeft == 0 && g_Config.BruteForceCurrentFunctionIndex > -2) {
		// count how many draw calls
		isDifferent = false;
		if (g_Config.BruteForceCurrentFunctionIndex == -1)
		{
			g_Config.BruteForceOriginalVertexCount = gpuStats.numVertsSubmitted;
			isDifferent = true;
		}
		else {
			isDifferent = (gpuStats.numVertsSubmitted != g_Config.BruteForceOriginalVertexCount);
		}
		if (isDifferent) {
			char g_change_screenshot_name[2048];
			std::string path = GetSysDirectory(DIRECTORY_SCREENSHOT);
			while (path.length() > 0 && path.back() == '/') {
				path.resize(path.size() - 1);
			}
			std::string ext;
			if (g_Config.bScreenshotsAsPNG)
				ext = ".png";
			else
				ext = ".jpg";

			int verts = gpuStats.numVertsSubmitted;
			int i = g_Config.BruteForceCurrentFunctionIndex;
			if (i<0)
				snprintf(g_ScreenshotName, sizeof(g_ScreenshotName), "original %dv", verts);
			else {
				if (g_Config.BruteForceReturnCode>0) {
					if (verts != prev_verts) {
						// our best results are where the return code matters
						// todo: first rename the original screenshot
						if (verts > g_Config.BruteForceOriginalVertexCount || prev_verts > g_Config.BruteForceOriginalVertexCount) {
							snprintf(g_ScreenshotName, sizeof(g_ScreenshotName), "show test %8x %d %s %dv _L 0x%8x", function_addrs[i], g_Config.BruteForceReturnCode, function_names[i].c_str(), verts, function_addrs[i] - 0x8800000 + 0x20000000);
							snprintf(g_change_screenshot_name, sizeof(g_change_screenshot_name), "show test %8x %d %s %dv _L 0x%8x", function_addrs[i], g_Config.BruteForceReturnCode - 1, function_names[i].c_str(), prev_verts, function_addrs[i] - 0x8800000 + 0x20000000);
						}
						else {
							snprintf(g_ScreenshotName, sizeof(g_ScreenshotName), "hide test %8x %d %s %dv _L 0x%8x", function_addrs[i], g_Config.BruteForceReturnCode, function_names[i].c_str(), verts, function_addrs[i] - 0x8800000 + 0x20000000);
							snprintf(g_change_screenshot_name, sizeof(g_change_screenshot_name), "hide test %8x %d %s %dv _L 0x%8x", function_addrs[i], g_Config.BruteForceReturnCode - 1, function_names[i].c_str(), prev_verts, function_addrs[i] - 0x8800000 + 0x20000000);
						}
						if (!prev_name.empty()) {
							if (rename(prev_name.c_str(), ((path + "/") + g_change_screenshot_name + ext).c_str())) {
								ERROR_LOG(VR, "RENAME FAILED: '%s' to '%s'", prev_name.c_str(), ((path + "/") + g_change_screenshot_name + ext).c_str());
							}
						}
					} else {
						isDifferent = false;
					}
				}
				else if (verts == 0)
					snprintf(g_ScreenshotName, sizeof(g_ScreenshotName), "blank %8x %d %s", function_addrs[i], g_Config.BruteForceReturnCode, function_names[i].c_str());
				else if (verts > g_Config.BruteForceOriginalVertexCount)
					snprintf(g_ScreenshotName, sizeof(g_ScreenshotName), "show %dv %8x %d %s _L 0x%8X", verts, function_addrs[i], g_Config.BruteForceReturnCode, function_names[i].c_str(), function_addrs[i] - 0x8800000 + 0x20000000);
				else
					snprintf(g_ScreenshotName, sizeof(g_ScreenshotName), "hide %dv %8x %d %s _L 0x%8X", verts, function_addrs[i], g_Config.BruteForceReturnCode, function_names[i].c_str(), function_addrs[i] - 0x8800000 + 0x20000000);
			}
			if (verts > 0 || prev_verts > 0) {
				prev_name = (path + "/") + g_ScreenshotName + ext;
				g_TakeScreenshot = isDifferent;
			} else {
				prev_name = "";
			}
			prev_verts = verts;
		}
	}
}

void VR_BruteForceCrash(bool outofmemory)
{
	if (outofmemory) {
		// we ran out of memory, so it's safe to test this function again
		g_Config.BruteForceFramesLeft = g_Config.BruteForceFramesToRunFor;
		// give ourselves some breathing room by freeing memory
		bool temp = g_Config.bAutoSaveSymbolMap;
		g_Config.bAutoSaveSymbolMap = false;
		try {
			CPU_Shutdown();
		}
		catch (...) {}
		g_Config.bAutoSaveSymbolMap = temp;
		try {
			GPU_Shutdown();
		}
		catch (...) {}
	} else {
		// we crashed, so skip this function when we restart
		g_Config.BruteForceFramesLeft = 0;
	}
	try {
		g_Config.Save();
	} catch (...) {
		// this could actually be a problem...
		// we might end up starting again from scratch
	}
	W32Util::ExitAndRestart();

}