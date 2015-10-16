// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#ifdef _WIN32
#include <windows.h>
#include "GPU/Common/VR920.h"
#endif

#include "GPU/Common/VR.h"
#include "base/mutex.h"
#include "gfx/gl_debug_log.h"

#ifdef HAVE_OCULUSSDK
#include "OVR_CAPI_GL.h"
#else
#endif

#ifndef OVR_MAJOR_VERSION
typedef int ovrPosef;
#endif

namespace OGL
{

void VR_ConfigureHMD();
void VR_StartFramebuffer(int target_width, int target_height);
void VR_StopFramebuffer();
void VR_StartGUI(int target_width, int target_height);
void VR_StopGUI();

void VR_RenderToEyebuffer(int eye);
void VR_RenderToGUI();
void VR_BeginFrame();
void VR_BeginGUI();
void VR_EndGUI();
void VR_PresentHMDFrame(bool valid, ovrPosef *frame_eye_poses, int frame_index);
void VR_DrawTimewarpFrame();
void VR_DrawAsyncTimewarpFrame();

void VRThread_Start();
void VRThread_StartLoop();
void VRThread_Stop();
bool VRThread_Ready();

extern HGLRC g_hOffscreenRC;
extern bool vr_frame_valid;
extern recursive_mutex AsyncTimewarpLock;
}