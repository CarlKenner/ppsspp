// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <string.h>

#include "base/logging.h"
#include "base/timeutil.h"
#include "gfx/gl_common.h"
#include "gfx_es2/glsl_program.h"
#include "math/lin/matrix4x4.h"
#include "profiler/profiler.h"
#include "base/NativeApp.h"
#include "base/mutex.h"
#include "i18n/i18n.h"
#include "util/text/utf8.h"
#include "Common/StringUtils.h"
#include "../Globals.h"
#include "Windows/W32Util/Misc.h"
#include "Windows/MainWindow.h"
#include "Windows/resource.h"
#include "Core/Core.h"
#include "thread/thread.h"
#include "thread/threadutil.h"
#include "gfx/gl_debug_log.h"

#include <tchar.h>
#include <process.h>
#include <intrin.h>
#pragma intrinsic(_InterlockedExchange)

#ifdef _WIN32
#ifndef _XBOX
#include "Windows/OpenGLBase.h"
#endif
#endif

#include "Common/ColorConv.h"
#include "Common/Common.h"
#include "Common/Log.h"
#include "Core/Config.h"
#include "Core/Host.h"
#include "Core/MemMap.h"
#include "Core/Reporting.h"
#include "Core/System.h"
#include "Core/HLE/sceDisplay.h"
#include "GPU/ge_constants.h"
#include "GPU/GPUState.h"
#include "GPU/Common/DepalettizeShaderCommon.h"
#include "GPU/Common/FramebufferCommon.h"
#include "GPU/Common/PostShader.h"
#include "GPU/Common/TextureDecoder.h"
#include "GPU/Common/VR.h"
#include "GPU/Debugger/Stepping.h"
#include "GPU/GLES/GLStateCache.h"
#include "GPU/GLES/FBO.h"
#include "GPU/GLES/Framebuffer.h"
#include "GPU/GLES/ShaderManager.h"
#include "GPU/GLES/TextureCache.h"
#include "GPU/GLES/TransformPipeline.h"
#include "GPU/GLES/VROGL.h"

// Oculus Rift
#ifdef OVR_MAJOR_VERSION
ovrGLTexture g_eye_texture[2];
#endif

namespace OGL
{

void VR_DoPresentHMDFrame(bool valid);

recursive_mutex vrThreadLock;
recursive_mutex AsyncTimewarpLock;
volatile long vrThreadReady;
static std::thread *vrThread;
volatile long s_stop_vr_thread = false;
static bool s_vr_thread_failure = false;
HANDLE start_vr_thread_event = NULL;
bool vr_gui_valid = true;
bool vr_frame_valid = true;
bool vr_drew_frame = false; // we are just drawing the GUI if false

// Front buffers for Asynchronous Timewarp, to be swapped with either m_efbColor or m_resolvedColorTexture
// at the end of a frame. The back buffer is rendered to while the front buffer is displayed, then they are flipped.
GLuint m_eyeFramebuffer[2];
GLuint m_frontBuffer[2];
bool m_stereo3d;
int m_eye_count;

#if defined(OVR_MAJOR_VERSION) && OVR_MAJOR_VERSION >= 6
//--------------------------------------------------------------------------
struct TextureBuffer
{
	ovrSwapTextureSet* TextureSet;
	GLuint        texId;
	GLuint        fboId[8];
	ovrSizei      texSize;
 	ovrPosef      eyePose[8];

	TextureBuffer(ovrHmd hmd, bool rendertarget, bool displayableOnHmd, OVR::Sizei size, int mipLevels, unsigned char * data, int sampleCount)
	{
		GL_CHECK();
		//OVR_ASSERT(sampleCount <= 1); // The code doesn't currently handle MSAA textures.

		texSize = size;

		if (displayableOnHmd) {
			// This texture isn't necessarily going to be a rendertarget, but it usually is.
			//OVR_ASSERT(hmd); // No HMD? A little odd.
			//OVR_ASSERT(sampleCount == 1); // ovrHmd_CreateSwapTextureSetD3D11 doesn't support MSAA.

#if OVR_MAJOR_VERSION >= 7
			ovr_CreateSwapTextureSetGL(hmd, GL_SRGB8_ALPHA8, size.w, size.h, &TextureSet);
			GL_CHECK();
#else
			ovrHmd_CreateSwapTextureSetGL(hmd, GL_RGBA, size.w, size.h, &TextureSet);
			GL_CHECK();
#endif
			for (int i = 0; i < TextureSet->TextureCount; ++i)
			{
				ovrGLTexture* tex = (ovrGLTexture*)&TextureSet->Textures[i];
				GL_CHECK();
				glBindTexture(GL_TEXTURE_2D, tex->OGL.TexId);
				GL_CHECK();

				if (rendertarget)
				{
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
					GL_CHECK();
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
					GL_CHECK();
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
					GL_CHECK();
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
					GL_CHECK();
				}
				else
				{
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
					GL_CHECK();
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
					GL_CHECK();
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
					GL_CHECK();
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
					GL_CHECK();
				}
			}
		}
		else {
			glGenTextures(1, &texId);
			GL_CHECK();
			glBindTexture(GL_TEXTURE_2D, texId);
			GL_CHECK();

			if (rendertarget)
			{
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				GL_CHECK();
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				GL_CHECK();
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				GL_CHECK();
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				GL_CHECK();
			}
			else
			{
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
				GL_CHECK();
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				GL_CHECK();
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
				GL_CHECK();
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
				GL_CHECK();
			}

			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texSize.w, texSize.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
			GL_CHECK();
		}

		if (mipLevels > 1)
		{
			glGenerateMipmap(GL_TEXTURE_2D);
			GL_CHECK();
		}

		GL_CHECK();
		glGenFramebuffers(TextureSet->TextureCount, fboId);
		for (int i = 0; i < TextureSet->TextureCount; ++i)
		{
			glBindFramebuffer(GL_FRAMEBUFFER, fboId[i]);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ((ovrGLTexture*)&TextureSet->Textures[i])->OGL.TexId, 0);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	ovrSizei GetSize(void) const
	{
		return texSize;
	}

	void SetAndClearRenderSurface()
	{
		int WriteIndex;
		if (g_asyc_timewarp_active)
			WriteIndex = (TextureSet->CurrentIndex + 1) % TextureSet->TextureCount;
		else
			WriteIndex = TextureSet->CurrentIndex;
		GL_CHECK();
		ovrGLTexture* tex = (ovrGLTexture*)&TextureSet->Textures[WriteIndex];
		GL_CHECK();
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fboId[WriteIndex]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex->OGL.TexId, 0);
		GL_CHECK();
		glViewport(0, 0, texSize.w, texSize.h);
		GL_CHECK();
		//glFlush();
		GL_CHECK();
		//glFinish();
		GL_CHECK();

		//glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	}

	void UnsetRenderSurface()
	{
		int WriteIndex = TextureSet->CurrentIndex;
		glBindFramebuffer(GL_FRAMEBUFFER, fboId[WriteIndex]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
	}
};

TextureBuffer * eyeRenderTexture[2];
TextureBuffer * guiRenderTexture;
ovrRecti eyeRenderViewport[2];
ovrRecti guiRenderViewport;
ovrGLTexture * mirrorTexture;
int mirror_width = 0, mirror_height = 0;
GLuint mirrorFBO = 0;
#endif

#ifdef HAVE_OPENVR
///////////////////////////////////////////////////////////////////////////////
// 2D vector
///////////////////////////////////////////////////////////////////////////////
struct Vector2
{
	float x;
	float y;

	// ctors
	Vector2() : x(0), y(0) {};
	Vector2(float x, float y) : x(x), y(y) {};

	// utils functions
	void        set(float x, float y);
	float       length() const;                         //
	float       distance(const Vector2& vec) const;     // distance between two vectors
	Vector2&    normalize();                            //
	float       dot(const Vector2& vec) const;          // dot product
	bool        equal(const Vector2& vec, float e) const; // compare with epsilon

	// operators
	Vector2     operator-() const;                      // unary operator (negate)
	Vector2     operator+(const Vector2& rhs) const;    // add rhs
	Vector2     operator-(const Vector2& rhs) const;    // subtract rhs
	Vector2&    operator+=(const Vector2& rhs);         // add rhs and update this object
	Vector2&    operator-=(const Vector2& rhs);         // subtract rhs and update this object
	Vector2     operator*(const float scale) const;     // scale
	Vector2     operator*(const Vector2& rhs) const;    // multiply each element
	Vector2&    operator*=(const float scale);          // scale and update this object
	Vector2&    operator*=(const Vector2& rhs);         // multiply each element and update this object
	Vector2     operator/(const float scale) const;     // inverse scale
	Vector2&    operator/=(const float scale);          // scale and update this object
	bool        operator==(const Vector2& rhs) const;   // exact compare, no epsilon
	bool        operator!=(const Vector2& rhs) const;   // exact compare, no epsilon
	bool        operator<(const Vector2& rhs) const;    // comparison for sort
	float       operator[](int index) const;            // subscript operator v[0], v[1]
	float&      operator[](int index);                  // subscript operator v[0], v[1]

	friend Vector2 operator*(const float a, const Vector2 vec);
	friend std::ostream& operator<<(std::ostream& os, const Vector2& vec);
};

struct VertexDataLens
{
	Vector2 position;
	Vector2 texCoordRed;
	Vector2 texCoordGreen;
	Vector2 texCoordBlue;
};

GLuint m_left_texture = 0, m_right_texture = 0;

GLuint m_unSceneProgramID = 0, m_unLensProgramID = 0, m_unControllerTransformProgramID = 0, m_unRenderModelProgramID = 0;

GLint m_nSceneMatrixLocation = -1, m_nControllerMatrixLocation = -1, m_nRenderModelMatrixLocation = -1;

unsigned int m_uiVertcount;

GLuint m_glSceneVertBuffer;
GLuint m_unLensVAO;
GLuint m_glIDVertBuffer;
GLuint m_glIDIndexBuffer;
unsigned int m_uiIndexSize;


//-----------------------------------------------------------------------------
// Purpose: Creates all the shaders used by HelloVR SDL
//-----------------------------------------------------------------------------
bool CreateAllShaders()
{
	return true;
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void SetupDistortion()
{
	if (!m_pHMD)
		return;

	GLushort m_iLensGridSegmentCountH = 43;
	GLushort m_iLensGridSegmentCountV = 43;

	float w = (float)(1.0 / float(m_iLensGridSegmentCountH - 1));
	float h = (float)(1.0 / float(m_iLensGridSegmentCountV - 1));

	float u, v = 0;

	std::vector<VertexDataLens> vVerts(0);
	VertexDataLens vert;

	//left eye distortion verts
	float Xoffset = -1;
	for (int y = 0; y<m_iLensGridSegmentCountV; y++)
	{
		for (int x = 0; x<m_iLensGridSegmentCountH; x++)
		{
			u = x*w; v = 1 - y*h;
			vert.position = Vector2(Xoffset + u, -1 + 2 * y*h);

			vr::DistortionCoordinates_t dc0 = m_pHMD->ComputeDistortion(vr::Eye_Left, u, v);

			vert.texCoordRed = Vector2(dc0.rfRed[0], 1 - dc0.rfRed[1]);
			vert.texCoordGreen = Vector2(dc0.rfGreen[0], 1 - dc0.rfGreen[1]);
			vert.texCoordBlue = Vector2(dc0.rfBlue[0], 1 - dc0.rfBlue[1]);

			vVerts.push_back(vert);
		}
	}

	//right eye distortion verts
	Xoffset = 0;
	for (int y = 0; y<m_iLensGridSegmentCountV; y++)
	{
		for (int x = 0; x<m_iLensGridSegmentCountH; x++)
		{
			u = x*w; v = 1 - y*h;
			vert.position = Vector2(Xoffset + u, -1 + 2 * y*h);

			vr::DistortionCoordinates_t dc0 = m_pHMD->ComputeDistortion(vr::Eye_Right, u, v);

			vert.texCoordRed = Vector2(dc0.rfRed[0], 1 - dc0.rfRed[1]);
			vert.texCoordGreen = Vector2(dc0.rfGreen[0], 1 - dc0.rfGreen[1]);
			vert.texCoordBlue = Vector2(dc0.rfBlue[0], 1 - dc0.rfBlue[1]);

			vVerts.push_back(vert);
		}
	}

	std::vector<GLushort> vIndices;
	GLushort a, b, c, d;

	GLushort offset = 0;
	for (GLushort y = 0; y<m_iLensGridSegmentCountV - 1; y++)
	{
		for (GLushort x = 0; x<m_iLensGridSegmentCountH - 1; x++)
		{
			a = m_iLensGridSegmentCountH*y + x + offset;
			b = m_iLensGridSegmentCountH*y + x + 1 + offset;
			c = (y + 1)*m_iLensGridSegmentCountH + x + 1 + offset;
			d = (y + 1)*m_iLensGridSegmentCountH + x + offset;
			vIndices.push_back(a);
			vIndices.push_back(b);
			vIndices.push_back(c);

			vIndices.push_back(a);
			vIndices.push_back(c);
			vIndices.push_back(d);
		}
	}

	offset = (m_iLensGridSegmentCountH)*(m_iLensGridSegmentCountV);
	for (GLushort y = 0; y<m_iLensGridSegmentCountV - 1; y++)
	{
		for (GLushort x = 0; x<m_iLensGridSegmentCountH - 1; x++)
		{
			a = m_iLensGridSegmentCountH*y + x + offset;
			b = m_iLensGridSegmentCountH*y + x + 1 + offset;
			c = (y + 1)*m_iLensGridSegmentCountH + x + 1 + offset;
			d = (y + 1)*m_iLensGridSegmentCountH + x + offset;
			vIndices.push_back(a);
			vIndices.push_back(b);
			vIndices.push_back(c);

			vIndices.push_back(a);
			vIndices.push_back(c);
			vIndices.push_back(d);
		}
	}
	m_uiIndexSize = (unsigned int)vIndices.size();

	glGenVertexArrays(1, &m_unLensVAO);
	GL_CHECK();
	glBindVertexArray(m_unLensVAO);
	GL_CHECK();

	glGenBuffers(1, &m_glIDVertBuffer);
	GL_CHECK();
	glBindBuffer(GL_ARRAY_BUFFER, m_glIDVertBuffer);
	GL_CHECK();
	glBufferData(GL_ARRAY_BUFFER, vVerts.size()*sizeof(VertexDataLens), &vVerts[0], GL_STATIC_DRAW);
	GL_CHECK();

	glGenBuffers(1, &m_glIDIndexBuffer);
	GL_CHECK();
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_glIDIndexBuffer);
	GL_CHECK();
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, vIndices.size()*sizeof(GLushort), &vIndices[0], GL_STATIC_DRAW);
	GL_CHECK();

	glEnableVertexAttribArray(0);
	GL_CHECK();
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(VertexDataLens), (void *)offsetof(VertexDataLens, position));
	GL_CHECK();

	glEnableVertexAttribArray(1);
	GL_CHECK();
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(VertexDataLens), (void *)offsetof(VertexDataLens, texCoordRed));
	GL_CHECK();

	glEnableVertexAttribArray(2);
	GL_CHECK();
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(VertexDataLens), (void *)offsetof(VertexDataLens, texCoordGreen));
	GL_CHECK();

	glEnableVertexAttribArray(3);
	GL_CHECK();
	glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(VertexDataLens), (void *)offsetof(VertexDataLens, texCoordBlue));
	GL_CHECK();

	glBindVertexArray(0);
	GL_CHECK();

	glDisableVertexAttribArray(0);
	GL_CHECK();
	glDisableVertexAttribArray(1);
	GL_CHECK();
	glDisableVertexAttribArray(2);
	GL_CHECK();
	glDisableVertexAttribArray(3);
	GL_CHECK();

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	GL_CHECK();
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	GL_CHECK();
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
bool BInitGL()
{
	if (!CreateAllShaders())
		return false;
	
	//SetupTexturemaps();
	//SetupScene();
	//SetupCameras();
	//SetupStereoRenderTargets();
	SetupDistortion();

	//SetupRenderModels();
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: SteamVR Mirror window, not needed for compositor
//-----------------------------------------------------------------------------
void RenderDistortion()
{
}
#endif


void VR_ConfigureHMD()
{
	GL_CHECK();
#ifdef HAVE_OPENVR
	if (g_has_steamvr && m_pCompositor)
	{
		//m_pCompositor->SetGraphicsDevice(vr::Compositor_DeviceType_OpenGL, nullptr);
	}
#endif
#ifdef OVR_MAJOR_VERSION
	if (g_has_rift)
	{
#if OVR_MAJOR_VERSION <= 5
		ovrGLConfig cfg;
		cfg.OGL.Header.API = ovrRenderAPI_OpenGL;
#ifdef OCULUSSDK044ORABOVE
		// Set based on window size, not statically based on rift internals.
		cfg.OGL.Header.BackBufferSize.w = Renderer::GetBackbufferWidth();
		cfg.OGL.Header.BackBufferSize.h = Renderer::GetBackbufferHeight();
#else
		cfg.OGL.Header.RTSize.w = Renderer::GetBackbufferWidth();
		cfg.OGL.Header.RTSize.h = Renderer::GetBackbufferHeight();
#endif
		cfg.OGL.Header.Multisample = 0;
#ifdef _WIN32
		cfg.OGL.Window = (HWND)((cInterfaceWGL*)GLInterface)->m_window_handle;
		cfg.OGL.DC = GetDC(cfg.OGL.Window);
#ifndef OCULUSSDK042
#if OVR_MAJOR_VERSION <= 5
		if (g_is_direct_mode) //If in Direct Mode
		{
			ovrHmd_AttachToWindow(hmd, cfg.OGL.Window, nullptr, nullptr); //Attach to Direct Mode.
			GL_CHECK();
		}
#endif
#endif
#else
		cfg.OGL.Disp = (Display*)((cInterfaceGLX*)GLInterface)->getDisplay();
#ifdef OCULUSSDK043
		cfg.OGL.Win = glXGetCurrentDrawable();
#endif
#endif
		int caps = 0;
#if OVR_MAJOR_VERSION <= 4
		if (g_Config.bChromatic)
			caps |= ovrDistortionCap_Chromatic;
#endif
#ifdef __linux__
		caps |= ovrDistortionCap_LinuxDevFullscreen;
#endif
		if (g_Config.bTimewarp)
			caps |= ovrDistortionCap_TimeWarp;
		if (g_Config.bVignette)
			caps |= ovrDistortionCap_Vignette;
		if (g_Config.bNoRestore)
			caps |= ovrDistortionCap_NoRestore;
		if (g_Config.bFlipVertical)
			caps |= ovrDistortionCap_FlipInput;
		if (g_Config.bSRGB)
			caps |= ovrDistortionCap_SRGB;
		if (g_Config.bOverdrive)
			caps |= ovrDistortionCap_Overdrive;
		if (g_Config.bHqDistortion)
			caps |= ovrDistortionCap_HqDistortion;
		ovrHmd_ConfigureRendering(hmd, &cfg.Config, caps,
			g_eye_fov, g_eye_render_desc);
		GL_CHECK();
#if OVR_MAJOR_VERSION <= 4
		ovrhmd_EnableHSWDisplaySDKRender(hmd, false); //Disable Health and Safety Warning.
		GL_CHECK();
#endif

#else
		for (int i = 0; i < ovrEye_Count; ++i)
			g_eye_render_desc[i] = ovrHmd_GetRenderDesc(hmd, (ovrEyeType)i, g_eye_fov[i]);
		GL_CHECK();
#endif
	}
#endif
}

#if defined(OVR_MAJOR_VERSION) && OVR_MAJOR_VERSION >= 6
void RecreateMirrorTextureIfNeeded()
{
	GL_CHECK();
	int w = PSP_CoreParameter().pixelWidth;  //Renderer::GetBackbufferWidth();
	int h = PSP_CoreParameter().pixelHeight; //Renderer::GetBackbufferHeight();
	if (w != mirror_width || h != mirror_height || ((mirrorTexture == nullptr)!=g_Config.bNoMirrorToWindow))
	{
		if (mirrorTexture)
		{
			glDeleteFramebuffers(1, &mirrorFBO);
			GL_CHECK();
			ovrHmd_DestroyMirrorTexture(hmd, (ovrTexture*)mirrorTexture);
			GL_CHECK();
			mirrorTexture = nullptr;
		}
		if (!g_Config.bNoMirrorToWindow)
		{
			// Create mirror texture and an FBO used to copy mirror texture to back buffer
			mirror_width = w;
			mirror_height = h;
#if OVR_MAJOR_VERSION >= 7
			ovr_CreateMirrorTextureGL(hmd, GL_SRGB8_ALPHA8, mirror_width, mirror_height, (ovrTexture**)&mirrorTexture);
			GL_CHECK();
#else
			ovrHmd_CreateMirrorTextureGL(hmd, GL_RGBA, mirror_width, mirror_height, (ovrTexture**)&mirrorTexture);
			GL_CHECK();
#endif
			// Configure the mirror read buffer
			glGenFramebuffers(1, &mirrorFBO);
			GL_CHECK();
			glBindFramebuffer(GL_READ_FRAMEBUFFER, mirrorFBO);
			GL_CHECK();
			glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mirrorTexture->OGL.TexId, 0);
			GL_CHECK();
			glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
			GL_CHECK();
			glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
			GL_CHECK();
		}
	}
}
#endif

void VR_StartGUI(int target_width, int target_height)
{
#if defined(OVR_MAJOR_VERSION) && OVR_MAJOR_VERSION >= 6
	if (g_has_rift)
	{
		GL_SwapInterval(0);
		GL_CHECK();

		ovrSizei target_size;
		target_size.w = target_width;
		target_size.h = target_height;
		guiRenderTexture = new TextureBuffer(hmd, true, true, target_size, 1, nullptr, 1);
		guiRenderViewport.Pos.x = 0;
		guiRenderViewport.Pos.y = 0;
		guiRenderViewport.Size = target_size;
	}
#endif
}

void VR_StopGUI()
{
	GL_CHECK();
#if defined(OVR_MAJOR_VERSION) && OVR_MAJOR_VERSION >= 6
	if (g_has_rift)
	{
		if (guiRenderTexture)
		{
			ovrHmd_DestroySwapTextureSet(hmd, guiRenderTexture->TextureSet);
			GL_CHECK();
			delete guiRenderTexture;
			guiRenderTexture = nullptr;
		}
	}
#endif
}

void VR_StartFramebuffer(int target_width, int target_height)
{
	GL_CHECK();
	m_eyeFramebuffer[0] = 0;
	m_eyeFramebuffer[1] = 0;
	m_frontBuffer[0] = 0;
	m_frontBuffer[1] = 0;

#if defined(OVR_MAJOR_VERSION) && OVR_MAJOR_VERSION >= 6
	if (g_has_rift)
	{
		GL_SwapInterval(0);
		GL_CHECK();

		for (int eye = 0; eye<2; ++eye)
		{
			ovrSizei target_size;
			target_size.w = target_width;
			target_size.h = target_height;
			eyeRenderTexture[eye] = new TextureBuffer(hmd, true, true, target_size, 1, nullptr, 1);
			eyeRenderViewport[eye].Pos.x = 0;
			eyeRenderViewport[eye].Pos.y = 0;
			eyeRenderViewport[eye].Size = target_size;
		}

		RecreateMirrorTextureIfNeeded();
		GL_CHECK();
	}
	else
#endif
	if (g_has_vr920)
	{
#ifdef _WIN32
		VR920_StartStereo3D();
#endif
	}
#if (defined(OVR_MAJOR_VERSION) && OVR_MAJOR_VERSION <= 5) || defined(HAVE_OPENVR)
	else if (g_has_rift || g_has_steamvr)
	{
		// create the eye textures
		glGenTextures(2, m_frontBuffer);
		for (int eye = 0; eye < 2; ++eye)
		{
			glBindTexture(GL_TEXTURE_2D, m_frontBuffer[eye]);
			GL_CHECK();
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
			GL_CHECK();
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			GL_CHECK();
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			GL_CHECK();
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, target_width, target_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
			GL_CHECK();
		}

		// create the eye framebuffers
		glGenFramebuffers(2, m_eyeFramebuffer);
		GL_CHECK();
		for (int eye = 0; eye < 2; ++eye)
		{
			glBindFramebuffer(GL_FRAMEBUFFER, m_eyeFramebuffer[eye]);
			GL_CHECK();
			glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, m_frontBuffer[eye], 0);
			GL_CHECK();
		}

#if defined(OVR_MAJOR_VERSION) && OVR_MAJOR_VERSION <= 5
		if (g_has_rift)
		{
			g_eye_texture[0].OGL.Header.API = ovrRenderAPI_OpenGL;
			g_eye_texture[0].OGL.Header.TextureSize.w = target_width;
			g_eye_texture[0].OGL.Header.TextureSize.h = target_height;
			g_eye_texture[0].OGL.Header.RenderViewport.Pos.x = 0;
			g_eye_texture[0].OGL.Header.RenderViewport.Pos.y = 0;
			g_eye_texture[0].OGL.Header.RenderViewport.Size.w = target_width;
			g_eye_texture[0].OGL.Header.RenderViewport.Size.h = target_height;
			g_eye_texture[0].OGL.TexId = m_frontBuffer[0];
			g_eye_texture[1] = g_eye_texture[0];
			if (g_ActiveConfig.iStereoMode == STEREO_OCULUS)
				g_eye_texture[1].OGL.TexId = m_frontBuffer[1];
		}
#endif
#if defined(HAVE_OPENVR)
		if (g_has_steamvr)
		{
			m_left_texture = m_frontBuffer[0];
			m_right_texture = m_frontBuffer[1];
		}
#endif
	}
#endif
	else
	{
		// no VR 
	}
}

void VR_StopFramebuffer()
{
#if defined(OVR_MAJOR_VERSION) && OVR_MAJOR_VERSION >= 6
	if (g_has_rift)
	{
		glDeleteFramebuffers(1, &mirrorFBO);
		GL_CHECK();
		ovrHmd_DestroyMirrorTexture(hmd, (ovrTexture*)mirrorTexture);
		GL_CHECK();
		mirrorTexture = nullptr;

		// On Oculus SDK 0.6.0 and above, we need to destroy the eye textures Oculus created for us.
		for (int eye = 0; eye < 2; eye++)
		{
			if (eyeRenderTexture[eye])
			{
				ovrHmd_DestroySwapTextureSet(hmd, eyeRenderTexture[eye]->TextureSet);
				GL_CHECK();
				delete eyeRenderTexture[eye];
				eyeRenderTexture[eye] = nullptr;
			}
		}
	}
#endif
#if defined(OVR_MAJOR_VERSION) && OVR_MAJOR_VERSION <= 5
	if (g_has_rift)
	{
		glDeleteFramebuffers(2, m_eyeFramebuffer);
		m_eyeFramebuffer[0] = 0;
		m_eyeFramebuffer[1] = 0;

		glDeleteTextures(2, m_frontBuffer);
		m_frontBuffer[0] = 0;
		m_frontBuffer[1] = 0;
	}
#endif
#if defined(HAVE_OPENVR)
	if (g_has_steamvr)
	{
		glDeleteFramebuffers(2, m_eyeFramebuffer);
		m_eyeFramebuffer[0] = 0;
		m_eyeFramebuffer[1] = 0;

		glDeleteTextures(2, m_frontBuffer);
		m_frontBuffer[0] = 0;
		m_frontBuffer[1] = 0;
	}
#endif
}

void VR_BeginFrame()
{
	GL_CHECK();
	glFlush();
	//GL_CHECK();
	glFinish();
	//GL_CHECK();
	if (g_Config.bDisableNearClipping)
		glEnable(GL_DEPTH_CLAMP);
	else
		glDisable(GL_DEPTH_CLAMP);
	GL_CHECK();

	// At the start of a frame, we get the frame timing and begin the frame.
#ifdef OVR_MAJOR_VERSION
	if (g_has_rift)
	{
#if OVR_MAJOR_VERSION >= 6
		++g_ovr_frameindex;
		// On Oculus SDK 0.6.0 and above, we get the frame timing manually, then swap each eye texture 
		g_rift_frame_timing = ovrHmd_GetFrameTiming(hmd, 0);
		if (!g_asyc_timewarp_active) {
			lock_guard guard(AsyncTimewarpLock);
			for (int eye = 0; eye < 2; eye++)
			{
				// Increment to use next texture, just before writing
				eyeRenderTexture[eye]->TextureSet->CurrentIndex = (eyeRenderTexture[eye]->TextureSet->CurrentIndex + 1) % eyeRenderTexture[eye]->TextureSet->TextureCount;
				eyeRenderTexture[eye]->eyePose[eyeRenderTexture[eye]->TextureSet->CurrentIndex] = g_eye_poses[eye];
			}
		}
#else
		ovrHmd_DismissHSWDisplay(hmd);
		GL_CHECK();
		g_rift_frame_timing = ovrHmd_BeginFrame(hmd, ++g_ovr_frameindex);
#endif
	}
#endif
}

static bool began_gui = false, has_gui = false;

void VR_BeginGUI()
{
	began_gui = true;
	GL_CHECK();
#ifdef OVR_MAJOR_VERSION
#if OVR_MAJOR_VERSION >= 6
	if (g_has_rift)
	{
		if (!g_asyc_timewarp_active) {
			guiRenderTexture->TextureSet->CurrentIndex = (guiRenderTexture->TextureSet->CurrentIndex + 1) % guiRenderTexture->TextureSet->TextureCount;
		}
		g_rift_frame_timing = ovrHmd_GetFrameTiming(hmd, 0);
		lock_guard guard(AsyncTimewarpLock);
		GL_CHECK();
		// Switch to eye render target
		VR_RenderToGUI();
		GL_CHECK();
		glClearColor(0, 0, 0, 0);
		glClear(GL_COLOR_BUFFER_BIT);
		GLenum err = glGetError();
		vr_gui_valid = (err == GL_NO_ERROR);
	}
#endif
#endif
}

void VR_EndGUI()
{
	if (!began_gui)
		return;
	began_gui = false;
	GL_CHECK();
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	GL_CHECK();
	if (g_has_rift)
	{
		lock_guard guard(AsyncTimewarpLock);
		if (g_asyc_timewarp_active) {
			glFlush();
			glFinish();
			guiRenderTexture->TextureSet->CurrentIndex = (guiRenderTexture->TextureSet->CurrentIndex + 1) % guiRenderTexture->TextureSet->TextureCount;
		}
		if (!vr_drew_frame) {
			VR_DoPresentHMDFrame(true);
		}
		vr_drew_frame = false;
	}
	has_gui = true;
}

void VR_RenderToGUI()
{
	GL_CHECK();
#if defined(OVR_MAJOR_VERSION) && OVR_MAJOR_VERSION >= 6
	if (g_has_rift)
	{
		guiRenderTexture->UnsetRenderSurface();
		guiRenderTexture->SetAndClearRenderSurface();
	}
	else
#endif
	{
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}
}

void VR_RenderToEyebuffer(int eye)
{
	GL_CHECK();
#if defined(OVR_MAJOR_VERSION) && OVR_MAJOR_VERSION >= 6
	if (g_has_rift)
	{
		eyeRenderTexture[eye]->SetAndClearRenderSurface();
		GL_CHECK();
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		GL_CHECK();
		GLenum fbStatus = 0;
		fbStatus = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
		vr_frame_valid = fbStatus == GL_FRAMEBUFFER_COMPLETE;
	}
#endif
#if (defined(OVR_MAJOR_VERSION) && OVR_MAJOR_VERSION <= 5)
	if (g_has_rift)
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_eyeFramebuffer[eye]);
	GL_CHECK();
#endif
#if defined(HAVE_OPENVR)
	if (g_has_steamvr)
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_eyeFramebuffer[eye]);
	GL_CHECK();
#endif
}

void PresentFrameSDK6()
{
#if defined(OVR_MAJOR_VERSION) && OVR_MAJOR_VERSION >= 6
	if (g_has_rift)
	{
		int count = 0;
		ovrLayerHeader* LayerList[2] = { nullptr, nullptr };
		ovrLayerEyeFov ld;
		ld.Header.Flags = (g_Config.bFlipVertical ? 0 : ovrLayerFlag_TextureOriginAtBottomLeft) | (g_Config.bHqDistortion ? ovrLayerFlag_HighQuality : 0);
		{
			lock_guard guard(AsyncTimewarpLock);
			if (eyeRenderTexture[0] && eyeRenderTexture[1] && eyeRenderTexture[0]->TextureSet) {
				++count;
				ld.Header.Type = ovrLayerType_EyeFov;
				for (int eye = 0; eye < 2; eye++)
				{
					eyeRenderTexture[eye]->UnsetRenderSurface();
					ld.ColorTexture[eye] = eyeRenderTexture[eye]->TextureSet;
					ld.Viewport[eye] = eyeRenderViewport[eye];
					ld.Fov[eye] = g_eye_fov[eye];
					ld.RenderPose[eye] = eyeRenderTexture[eye]->eyePose[eyeRenderTexture[eye]->TextureSet->CurrentIndex];
				}
				LayerList[count-1] = &ld.Header;
			}
			ovrLayerQuad lg;
			if (guiRenderTexture && has_gui) {
				++count;
				lg.Header.Type = ovrLayerType_QuadInWorld;
				lg.Header.Flags = ld.Header.Flags;
				lg.ColorTexture = guiRenderTexture->TextureSet;
				lg.Viewport = guiRenderViewport;
				lg.QuadSize.x = 1.6f / 2.0f; // metres
				lg.QuadSize.y = 0.9f / 2.0f; // metres
				lg.QuadPoseCenter.Position.x = 0; // metres
				lg.QuadPoseCenter.Position.y = 0; // metres
				lg.QuadPoseCenter.Position.z = -0.7; // metres (negative means in front of us)
				lg.QuadPoseCenter.Orientation.w = 1;
				lg.QuadPoseCenter.Orientation.x = 0;
				lg.QuadPoseCenter.Orientation.y = 0;
				lg.QuadPoseCenter.Orientation.z = 0;
				LayerList[count-1] = &lg.Header;
			}
			ovrResult result = ovrHmd_SubmitFrame(hmd, 0, nullptr, LayerList, count);
		}
	}
#endif 
}

int reals = 1;
int timewarps = 0;
int denominator = 1;
int tcount = 0;

void VR_DoPresentHMDFrame(bool valid)
{
	if (g_asyc_timewarp_active)
		return;
#ifdef HAVE_OPENVR
	if (m_pCompositor)
	{
		m_pCompositor->Submit(vr::Eye_Left, vr::API_OpenGL, (void*)(size_t)m_left_texture, nullptr);
		m_pCompositor->Submit(vr::Eye_Right, vr::API_OpenGL, (void*)(size_t)m_right_texture, nullptr);
		m_pCompositor->WaitGetPoses(m_rTrackedDevicePose, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
		uint32_t unSize = m_pCompositor->GetLastError(NULL, 0);
		if (unSize > 1)
		{
			char* buffer = new char[unSize];
			m_pCompositor->GetLastError(buffer, unSize);
			ERROR_LOG(VR, "Compositor - %s\n", buffer);
			delete[] buffer;
		}
		if (!g_Config.bNoMirrorToWindow)
		{
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
			// Blit mirror texture to back buffer
			glBindFramebuffer(GL_READ_FRAMEBUFFER, m_eyeFramebuffer[0]);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
			GLint w = PSP_CoreParameter().renderWidth;  //Renderer::GetTargetWidth();
			GLint h = PSP_CoreParameter().renderHeight; //Renderer::GetTargetHeight();
			glBlitFramebuffer(0, 0, w, h,
				0, 0, PSP_CoreParameter().pixelWidth, PSP_CoreParameter().pixelHeight,
				GL_COLOR_BUFFER_BIT, GL_NEAREST);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
			//GL_SwapBuffers();
		}
	}
#endif
#ifdef OVR_MAJOR_VERSION
	if (g_has_rift)
	{
		//ovrHmd_EndEyeRender(hmd, ovrEye_Left, g_left_eye_pose, &FramebufferManager::m_eye_texture[ovrEye_Left].Texture);
		//ovrHmd_EndEyeRender(hmd, ovrEye_Right, g_right_eye_pose, &FramebufferManager::m_eye_texture[ovrEye_Right].Texture);
#if OVR_MAJOR_VERSION <= 5
		// Let OVR do distortion rendering, Present and flush/sync.
		ovrHmd_EndFrame(hmd, g_eye_poses, &g_eye_texture[0].Texture);
#else
		RecreateMirrorTextureIfNeeded();
		PresentFrameSDK6();
		if (g_Config.bSynchronousTimewarp)
		{
			tcount += timewarps;
			while (tcount >= reals) {
				PresentFrameSDK6();
				tcount -= reals;
			}
		} else {
			tcount = 0;
		}

		if (!g_Config.bNoMirrorToWindow && mirrorTexture)
		{
			// Blit mirror texture to back buffer
			glBindFramebuffer(GL_READ_FRAMEBUFFER, mirrorFBO);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
			GLint w = mirrorTexture->OGL.Header.TextureSize.w;
			GLint h = mirrorTexture->OGL.Header.TextureSize.h;
			glBlitFramebuffer(0, h, w, 0,
				0, 0, w, h,
				GL_COLOR_BUFFER_BIT, GL_NEAREST);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
			//GL_SwapBuffers();
		}
#endif
	}
#endif
	has_gui = false;
}

void VR_PresentHMDFrame(bool valid, ovrPosef *frame_eye_poses)
{
	float vps, f_fps, actual_fps;
	__DisplayGetFPS(&vps, &f_fps, &actual_fps);

	static int oldfps = 0;
	static int refresh = 75; // todo: hmd's refresh rate
	int newfps = (int)(f_fps + 0.5f);
	int fps;
	if (g_asyc_timewarp_active) {
		oldfps = 0;
		reals = 1;
		timewarps = 0;
		denominator = 1;
	} else if (abs(newfps - oldfps) > 2) {
		// fps has changed!
		bool nonstandard = false;
		fps = newfps;
		// I haven't implemented frameskipping yet, so if going faster than the hmd, lock it to the hmd's refresh rate
		if (fps >= refresh) {
			fps = refresh;
			reals = 1;
			timewarps = 0;
		}
		else if (fps >= 118 && fps <= 122) {
			fps = 120;
			nonstandard = true;
		}
		else if (fps >= 88 && fps <= 92) {
			fps = 90;
			switch (refresh) {
			case 120: reals = 3; timewarps = 1; break;
			default: nonstandard = true;
			}
		}
		else if (fps >= 73 && fps <= 77) {
			fps = 75;
			switch (refresh) {
			case 120: reals = 5; timewarps = 3; break;
			case 90: reals = 5; timewarps = 1; break;
			default: nonstandard = true;
			}
		}
		else if (fps >= 58 && fps <= 62) {
			fps = 60;
			switch (refresh) {
			case 120: reals = 1; timewarps = 1; break;
			case 90: reals = 2; timewarps = 1; break;
			case 75: reals = 4; timewarps = 1; break;
			default: nonstandard = true;
			}
		}
		else if (fps >= 28 && fps <= 32) {
			fps = 30;
			switch (refresh) {
			case 120: reals = 1; timewarps = 3; break;
			case 90: reals = 1; timewarps = 2; break;
			case 75: reals = 2; timewarps = 3; break;
			case 60: reals = 1; timewarps = 1; break;
			default: nonstandard = true;
			}
		}
		else if (fps >= 18 && fps <= 22) {
			fps = 20;
			switch (refresh) {
			case 120: reals = 1; timewarps = 5; break;
			case 90: reals = 2; timewarps = 7; break;
			case 75: reals = 4; timewarps = 11; break;
			case 60: reals = 1; timewarps = 2; break;
			default: nonstandard = true;
			}
		}
		else if (fps <= 17) {
			fps = 15;
			switch (refresh) {
			case 120: reals = 1; timewarps = 7; break;
			case 105: reals = 1; timewarps = 6; break;
			case 90: reals = 1; timewarps = 5; break;
			case 75: reals = 1; timewarps = 4; break;
			case 60: reals = 1; timewarps = 3; break;
			default: nonstandard = true;
			}
		}
		else {
			fps = fps;
			nonstandard = true;
		}
		if (fps != oldfps) {
			oldfps = fps;
			if (nonstandard) {
				// todo: calculate lowest common denominator
				reals = 1; timewarps = 0;
			}
			denominator = reals + timewarps;
			tcount = 0;
		}
	}

	if (valid)
	{
		vr_drew_frame = true;
		glFinish();
		//Sleep(8);
		lock_guard guard(AsyncTimewarpLock);
		g_new_tracking_frame = true;
		if (g_asyc_timewarp_active) {
			for (int eye = 0; eye < 2; eye++)
			{
				if (eyeRenderTexture[eye] && eyeRenderTexture[eye]->TextureSet) {
					eyeRenderTexture[eye]->TextureSet->CurrentIndex = (eyeRenderTexture[eye]->TextureSet->CurrentIndex + 1) % eyeRenderTexture[eye]->TextureSet->TextureCount;
					if (frame_eye_poses)
						eyeRenderTexture[eye]->eyePose[eyeRenderTexture[eye]->TextureSet->CurrentIndex] = frame_eye_poses[eye];
					else
						eyeRenderTexture[eye]->eyePose[eyeRenderTexture[eye]->TextureSet->CurrentIndex] = g_eye_poses[eye];
				}
				eyeRenderTexture[eye]->UnsetRenderSurface();
			}
			has_gui = false;
			return;
		} else {
			for (int eye = 0; eye < 2; eye++)
			{
				if (eyeRenderTexture[eye] && eyeRenderTexture[eye]->TextureSet) {
					if (frame_eye_poses)
						eyeRenderTexture[eye]->eyePose[eyeRenderTexture[eye]->TextureSet->CurrentIndex] = frame_eye_poses[eye];
					else
						eyeRenderTexture[eye]->eyePose[eyeRenderTexture[eye]->TextureSet->CurrentIndex] = g_eye_poses[eye];
				}
				eyeRenderTexture[eye]->UnsetRenderSurface();
			}
		}
	}
	else
	{
		g_new_tracking_frame = true;
	}

	VR_DoPresentHMDFrame(valid);
	//VR_DrawTimewarpFrame();
	//tcount += timewarps;
	//while (tcount >= reals) {
	//	VR_DrawTimewarpFrame();
	//	tcount -= reals;
	//}
}


void VR_DrawTimewarpFrame()
{
	GL_CHECK();
	// As far as I know, OpenVR doesn't support Timewarp yet
#if 0 && defined(HAVE_OPENVR)
	if (g_has_steamvr && m_pCompositor)
	{
		m_pCompositor->Submit(vr::Eye_Left, vr::API_OpenGL, (void*)m_left_texture, nullptr);
		GL_CHECK();
		m_pCompositor->Submit(vr::Eye_Right, vr::API_OpenGL, (void*)m_right_texture, nullptr);
		GL_CHECK();
		m_pCompositor->WaitGetPoses(m_rTrackedDevicePose, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
		GL_CHECK();
		uint32_t unSize = m_pCompositor->GetLastError(NULL, 0);
		if (unSize > 1)
		{
			char* buffer = new char[unSize];
			m_pCompositor->GetLastError(buffer, unSize);
			ERROR_LOG(VR, "Compositor - %s\n", buffer);
			delete[] buffer;
		}
	}
#endif
#ifdef OVR_MAJOR_VERSION
	if (g_has_rift)
	{
		ovrFrameTiming frameTime;
#if OVR_MAJOR_VERSION <= 5
		frameTime = ovrHmd_BeginFrame(hmd, ++g_ovr_frameindex);
		GL_CHECK();

		ovr_WaitTillTime(frameTime.NextFrameSeconds - g_ActiveConfig.fTimeWarpTweak);

		ovrHmd_EndFrame(hmd, g_eye_poses, &g_eye_texture[0].Texture);
		GL_CHECK();
#else
		++g_ovr_frameindex;
		// On Oculus SDK 0.6.0 and above, we get the frame timing manually
		frameTime = ovrHmd_GetFrameTiming(hmd, 0);
		Sleep(1);
		PresentFrameSDK6();
#endif
	}
#endif
}

void VR_DrawAsyncTimewarpFrame()
{
	GL_CHECK();
	PresentFrameSDK6();
	Sleep(8);
}

enum VRThreadStatus : long
{
	THREAD_NONE = 0,
	THREAD_INIT,
	THREAD_WAITING,
	THREAD_INIT2,
	THREAD_CORE_LOOP,
	THREAD_SHUTDOWN,
	THREAD_END,
};

HANDLE vrThread_GetThreadHandle()
{
	lock_guard guard(vrThreadLock);
	return vrThread;
}

void VRThread();

void VRThread_Start()
{
	lock_guard guard(vrThreadLock);
	if (!vrThread && g_can_async_timewarp)
	{
		vrThread = new std::thread(VRThread);
		start_vr_thread_event = CreateEvent(NULL, FALSE, FALSE, NULL);
	}
}

void VRThread_StartLoop()
{
	lock_guard guard(vrThreadLock);
	if (vrThread)
	{
		SetEvent(start_vr_thread_event);
	}
}

void VRThread_Stop()
{
	// Already stopped?
	{
		lock_guard guard(vrThreadLock);
		if (vrThread == NULL || vrThreadReady == THREAD_END)
			return;
	}

	{
		lock_guard guard(vrThreadLock);
		s_stop_vr_thread = true;
		SetEvent(start_vr_thread_event);
		vrThread->join();
		delete vrThread;
		vrThread = NULL;
		vrThreadReady = THREAD_NONE;
		CloseHandle(start_vr_thread_event);
	}

	// finished()
}

bool VRThread_Ready()
{
	return vrThreadReady == THREAD_CORE_LOOP;
}

HWND m_offscreen_window_handle = NULL; // should be INVALID_WINDOW_HANDLE
static HDC hOffscreenDC = NULL;       // Private GDI Device Context
HGLRC g_hOffscreenRC = NULL;     // Permanent Rendering Context

// Create offscreen rendering window and its secondary OpenGL context
// This is used for the normal rendering thread with asynchronous timewarp
bool CreateOffscreen()
{
	if (m_offscreen_window_handle != nullptr)
		return false;

	// Create offscreen window here!
	int width = 640;
	int height = 480;

	WNDCLASSEX wndClass;
	wndClass.cbSize = sizeof(WNDCLASSEX);
	wndClass.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC | CS_DBLCLKS;
	wndClass.lpfnWndProc = DefWindowProc;
	wndClass.cbClsExtra = 0;
	wndClass.cbWndExtra = 0;
	wndClass.hInstance = 0;
	wndClass.hIcon = 0;
	wndClass.hCursor = LoadCursor(0, IDC_ARROW);
	wndClass.hbrBackground = 0;
	wndClass.lpszMenuName = 0;
	wndClass.lpszClassName = _T("DolphinOffscreenOpenGL");
	wndClass.hIconSm = 0;
	RegisterClassEx(&wndClass);

	// Create the window. Position and size it.
	HWND wnd = CreateWindowEx(0,
		_T("DolphinOffscreenOpenGL"),
		_T("Dolphin offscreen OpenGL"),
		WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_POPUP,
		CW_USEDEFAULT, CW_USEDEFAULT, width, height,
		0, 0, 0, 0);
	m_offscreen_window_handle = wnd;

	PIXELFORMATDESCRIPTOR pfd =         // pfd Tells Windows How We Want Things To Be
	{
		sizeof(PIXELFORMATDESCRIPTOR),							// Size Of This Pixel Format Descriptor
		1,														// Version Number
		PFD_DRAW_TO_WINDOW |									// Format Must Support Window
		PFD_SUPPORT_OPENGL |									// Format Must Support OpenGL
		PFD_DOUBLEBUFFER,										// Must Support Double Buffering
		PFD_TYPE_RGBA,											// Request An RGBA Format
		24,														// Select Our Color Depth
		0, 0, 0, 0, 0, 0,										// Color Bits Ignored
		8,														// No Alpha Buffer
		0,														// Shift Bit Ignored
		0,														// No Accumulation Buffer
		0, 0, 0, 0,										// Accumulation Bits Ignored
		16,														// At least a 16Bit Z-Buffer (Depth Buffer)  
		8,														// 8-bit Stencil Buffer
		0,														// No Auxiliary Buffer
		PFD_MAIN_PLANE,								// Main Drawing Layer
		0,														// Reserved
		0, 0, 0												// Layer Masks Ignored
};

	int      PixelFormat;               // Holds The Results After Searching For A Match

	if (!(hOffscreenDC = GetDC(m_offscreen_window_handle)))
	{
		PanicAlert("(1) Can't create an OpenGL Device context. Fail.");
		return false;
	}

	if (!(PixelFormat = ChoosePixelFormat(hOffscreenDC, &pfd)))
	{
		PanicAlert("(2) Can't find a suitable PixelFormat.");
		return false;
	}

	if (!SetPixelFormat(hOffscreenDC, PixelFormat, &pfd))
	{
		PanicAlert("(3) Can't set the PixelFormat.");
		return false;
	}

	if (!(g_hOffscreenRC = wglCreateContext(hOffscreenDC)))
	{
		PanicAlert("(4) Can't create an OpenGL rendering context.");
		return false;
	}

	return true;
}

// VR Asynchronous Timewarp Thread
void VRThread()
{
	_InterlockedExchange(&vrThreadReady, THREAD_INIT);

	setCurrentThreadName("VR Thread");

	NOTICE_LOG(VR, "[VR Thread] Starting VR Thread");

	// Create our OpenGL context
	CreateOffscreen();

	_InterlockedExchange(&vrThreadReady, THREAD_WAITING);
	DWORD result = WaitForSingleObject(start_vr_thread_event, INFINITE);
	_InterlockedExchange(&vrThreadReady, THREAD_INIT2);
	if (result == WAIT_TIMEOUT) {
		ERROR_LOG(VR, "Wait for resume timed out. Resuming rendering");
	}
	
	NOTICE_LOG(VR, "[VR Thread] g_video_backend->Video_Prepare()");
	wglMakeCurrent(hOffscreenDC, g_hOffscreenRC);

	NOTICE_LOG(VR, "[VR Thread] Main VR loop");
	_InterlockedExchange(&vrThreadReady, THREAD_CORE_LOOP);
	while (!s_stop_vr_thread)
	{
		g_asyc_timewarp_active = g_Config.bAsynchronousTimewarp && g_can_async_timewarp;
		if (g_asyc_timewarp_active)
			VR_DrawAsyncTimewarpFrame();
		else
			Sleep(1000);
	}
	g_asyc_timewarp_active = false;
	_InterlockedExchange(&vrThreadReady, THREAD_SHUTDOWN);

	// cleanup
	if (g_hOffscreenRC)
	{
		if (!wglMakeCurrent(NULL, NULL))
			NOTICE_LOG(VR, "Could not release drawing context.");
		GL_CHECK();

		if (!wglDeleteContext(g_hOffscreenRC))
			ERROR_LOG(VR, "Attempt to release rendering context failed.");
		GL_CHECK();

		g_hOffscreenRC = NULL;
	}

	if (hOffscreenDC && !ReleaseDC(m_offscreen_window_handle, hOffscreenDC))
	{
		ERROR_LOG(VR, "Attempt to release device context failed.");
		hOffscreenDC = NULL;
	}

	NOTICE_LOG(VR, "[VR Thread] g_video_backend->Shutdown()");
	
	NOTICE_LOG(VR, "[VR Thread] Stopping VR Thread");

	_InterlockedExchange(&vrThreadReady, THREAD_END);
}

}