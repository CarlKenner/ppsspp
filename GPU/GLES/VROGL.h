// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

// OpenGL VR rendering interface

#pragma once

#include "GPU/Common/VR.h"
#include "base/mutex.h"
#include "gfx/gl_debug_log.h"

namespace OGL
{

void VR_ConfigureHMD();
void VR_StartFramebuffer(int target_width, int target_height);
void VR_StopFramebuffer();
void VR_StartGUI(int target_width, int target_height);
void VR_StopGUI();

// Frame rendering interface
void VR_RenderToEyebuffer(int eye);
void VR_RenderToGUI();
void VR_BeginFrame();
void VR_BeginGUI();
void VR_EndGUI();
void VR_PresentHMDFrame(bool valid, VRPose *frame_eye_poses, int frame_index);

// Asynchronous Timewarp
void VRThread_Start();
void VRThread_WaitForContextCreation();
void VRThread_StartLoop();
void VRThread_Stop();
bool VRThread_Ready();
extern HGLRC g_hOffscreenRC;
extern bool vr_frame_valid;
extern recursive_mutex AsyncTimewarpLock;
}