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
	GLuint        fboId;
	ovrSizei          texSize;

	TextureBuffer(ovrHmd hmd, bool rendertarget, bool displayableOnHmd, OVR::Sizei size, int mipLevels, unsigned char * data, int sampleCount)
	{
		//OVR_ASSERT(sampleCount <= 1); // The code doesn't currently handle MSAA textures.

		texSize = size;

		if (displayableOnHmd) {
			// This texture isn't necessarily going to be a rendertarget, but it usually is.
			//OVR_ASSERT(hmd); // No HMD? A little odd.
			//OVR_ASSERT(sampleCount == 1); // ovrHmd_CreateSwapTextureSetD3D11 doesn't support MSAA.

#if OVR_MAJOR_VERSION >= 7
			ovr_CreateSwapTextureSetGL(hmd, GL_SRGB8_ALPHA8, size.w, size.h, &TextureSet);
#else
			ovrHmd_CreateSwapTextureSetGL(hmd, GL_RGBA, size.w, size.h, &TextureSet);
#endif
			for (int i = 0; i < TextureSet->TextureCount; ++i)
			{
				ovrGLTexture* tex = (ovrGLTexture*)&TextureSet->Textures[i];
				glBindTexture(GL_TEXTURE_2D, tex->OGL.TexId);

				if (rendertarget)
				{
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				}
				else
				{
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
				}
			}
}
		else {
			glGenTextures(1, &texId);
			glBindTexture(GL_TEXTURE_2D, texId);

			if (rendertarget)
			{
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			}
			else
			{
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
			}

			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texSize.w, texSize.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
		}

		if (mipLevels > 1)
		{
			glGenerateMipmap(GL_TEXTURE_2D);
		}

		glGenFramebuffers(1, &fboId);
	}

	ovrSizei GetSize(void) const
	{
		return texSize;
	}

	void SetAndClearRenderSurface()
	{
		ovrGLTexture* tex = (ovrGLTexture*)&TextureSet->Textures[TextureSet->CurrentIndex];

		glBindFramebuffer(GL_FRAMEBUFFER, fboId);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex->OGL.TexId, 0);

		glViewport(0, 0, texSize.w, texSize.h);
		//glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	}

	void UnsetRenderSurface()
	{
		glBindFramebuffer(GL_FRAMEBUFFER, fboId);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
	}
};

TextureBuffer * eyeRenderTexture[2];
ovrSwapTextureSet * pTextureSet = 0;
ovrRecti eyeRenderViewport[2];
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
	glBindVertexArray(m_unLensVAO);

	glGenBuffers(1, &m_glIDVertBuffer);
	glBindBuffer(GL_ARRAY_BUFFER, m_glIDVertBuffer);
	glBufferData(GL_ARRAY_BUFFER, vVerts.size()*sizeof(VertexDataLens), &vVerts[0], GL_STATIC_DRAW);

	glGenBuffers(1, &m_glIDIndexBuffer);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_glIDIndexBuffer);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, vIndices.size()*sizeof(GLushort), &vIndices[0], GL_STATIC_DRAW);

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(VertexDataLens), (void *)offsetof(VertexDataLens, position));

	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(VertexDataLens), (void *)offsetof(VertexDataLens, texCoordRed));

	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(VertexDataLens), (void *)offsetof(VertexDataLens, texCoordGreen));

	glEnableVertexAttribArray(3);
	glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(VertexDataLens), (void *)offsetof(VertexDataLens, texCoordBlue));

	glBindVertexArray(0);

	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(2);
	glDisableVertexAttribArray(3);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
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
#if OVR_MAJOR_VERSION <= 4
		ovrhmd_EnableHSWDisplaySDKRender(hmd, false); //Disable Health and Safety Warning.
#endif

#else
		for (int i = 0; i < ovrEye_Count; ++i)
			g_eye_render_desc[i] = ovrHmd_GetRenderDesc(hmd, (ovrEyeType)i, g_eye_fov[i]);
#endif
	}
#endif
}

#if defined(OVR_MAJOR_VERSION) && OVR_MAJOR_VERSION >= 6
void RecreateMirrorTextureIfNeeded()
{
	int w = PSP_CoreParameter().pixelWidth;  //Renderer::GetBackbufferWidth();
	int h = PSP_CoreParameter().pixelHeight; //Renderer::GetBackbufferHeight();
	if (w != mirror_width || h != mirror_height || ((mirrorTexture == nullptr)!=g_Config.bNoMirrorToWindow))
	{
		if (mirrorTexture)
		{
			glDeleteFramebuffers(1, &mirrorFBO);
			ovrHmd_DestroyMirrorTexture(hmd, (ovrTexture*)mirrorTexture);
			mirrorTexture = nullptr;
		}
		if (!g_Config.bNoMirrorToWindow)
		{
			// Create mirror texture and an FBO used to copy mirror texture to back buffer
			mirror_width = w;
			mirror_height = h;
#if OVR_MAJOR_VERSION >= 7
			ovr_CreateMirrorTextureGL(hmd, GL_SRGB8_ALPHA8, mirror_width, mirror_height, (ovrTexture**)&mirrorTexture);
#else
			ovrHmd_CreateMirrorTextureGL(hmd, GL_RGBA, mirror_width, mirror_height, (ovrTexture**)&mirrorTexture);
#endif
			// Configure the mirror read buffer
			glGenFramebuffers(1, &mirrorFBO);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, mirrorFBO);
			glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mirrorTexture->OGL.TexId, 0);
			glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		}
	}
}
#endif

void VR_StartFramebuffer(int target_width, int target_height)
{
	m_eyeFramebuffer[0] = 0;
	m_eyeFramebuffer[1] = 0;
	m_frontBuffer[0] = 0;
	m_frontBuffer[1] = 0;

#if defined(OVR_MAJOR_VERSION) && OVR_MAJOR_VERSION >= 6
	if (g_has_rift)
	{
		GL_SwapInterval(0);

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
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, target_width, target_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		}

		// create the eye framebuffers
		glGenFramebuffers(2, m_eyeFramebuffer);
		for (int eye = 0; eye < 2; ++eye)
		{
			glBindFramebuffer(GL_FRAMEBUFFER, m_eyeFramebuffer[eye]);
			glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, m_frontBuffer[eye], 0);
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
		ovrHmd_DestroyMirrorTexture(hmd, (ovrTexture*)mirrorTexture);
		mirrorTexture = nullptr;
		ovrHmd_DestroySwapTextureSet(hmd, eyeRenderTexture[0]->TextureSet);
		ovrHmd_DestroySwapTextureSet(hmd, eyeRenderTexture[1]->TextureSet);

		// On Oculus SDK 0.6.0 and above, we need to destroy the eye textures Oculus created for us.
		for (int eye = 0; eye < 2; eye++)
		{
			if (eyeRenderTexture[eye])
			{
				ovrHmd_DestroySwapTextureSet(hmd, eyeRenderTexture[eye]->TextureSet);
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
	// At the start of a frame, we get the frame timing and begin the frame.
#ifdef OVR_MAJOR_VERSION
	if (g_has_rift)
	{
#if OVR_MAJOR_VERSION >= 6
		++g_ovr_frameindex;
		// On Oculus SDK 0.6.0 and above, we get the frame timing manually, then swap each eye texture 
		g_rift_frame_timing = ovrHmd_GetFrameTiming(hmd, 0);
		for (int eye = 0; eye < 2; eye++)
		{
			// Increment to use next texture, just before writing
			eyeRenderTexture[eye]->TextureSet->CurrentIndex = (eyeRenderTexture[eye]->TextureSet->CurrentIndex + 1) % eyeRenderTexture[eye]->TextureSet->TextureCount;
		}
#else
		ovrHmd_DismissHSWDisplay(hmd);
		g_rift_frame_timing = ovrHmd_BeginFrame(hmd, ++g_ovr_frameindex);
#endif
}
#endif
}

void VR_RenderToEyebuffer(int eye)
{
#if defined(OVR_MAJOR_VERSION) && OVR_MAJOR_VERSION >= 6
	if (g_has_rift)
	{
		eyeRenderTexture[eye]->UnsetRenderSurface();
		// Switch to eye render target
		eyeRenderTexture[eye]->SetAndClearRenderSurface();
	}
#endif
#if (defined(OVR_MAJOR_VERSION) && OVR_MAJOR_VERSION <= 5)
	if (g_has_rift)
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_eyeFramebuffer[eye]);
#endif
#if defined(HAVE_OPENVR)
	if (g_has_steamvr)
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_eyeFramebuffer[eye]);
#endif
}

void VR_PresentHMDFrame()
{
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
			GL_SwapBuffers();
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
		ovrLayerEyeFov ld;
		ld.Header.Type = ovrLayerType_EyeFov;
		ld.Header.Flags = (g_Config.bFlipVertical?0:ovrLayerFlag_TextureOriginAtBottomLeft) | (g_Config.bHqDistortion?ovrLayerFlag_HighQuality:0);
		for (int eye = 0; eye < 2; eye++)
		{
			ld.ColorTexture[eye] = eyeRenderTexture[eye]->TextureSet;
			ld.Viewport[eye] = eyeRenderViewport[eye];
			ld.Fov[eye] = g_eye_fov[eye];
			ld.RenderPose[eye] = g_eye_poses[eye];
		}
		ovrLayerHeader* layers = &ld.Header;
		ovrResult result = ovrHmd_SubmitFrame(hmd, 0, nullptr, &layers, 1);

		if (!g_Config.bNoMirrorToWindow)
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
			GL_SwapBuffers();
		}
#endif
	}
#endif
}

void VR_DrawTimewarpFrame()
{
	// As far as I know, OpenVR doesn't support Timewarp yet
#if 0 && defined(HAVE_OPENVR)
	if (g_has_steamvr && m_pCompositor)
	{
		m_pCompositor->Submit(vr::Eye_Left, vr::API_OpenGL, (void*)m_left_texture, nullptr);
		m_pCompositor->Submit(vr::Eye_Right, vr::API_OpenGL, (void*)m_right_texture, nullptr);
		m_pCompositor->WaitGetPoses(m_rTrackedDevicePose, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
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

		ovr_WaitTillTime(frameTime.NextFrameSeconds - g_ActiveConfig.fTimeWarpTweak);

		ovrHmd_EndFrame(hmd, g_eye_poses, &g_eye_texture[0].Texture);
#else
		++g_ovr_frameindex;
		// On Oculus SDK 0.6.0 and above, we get the frame timing manually, then swap each eye texture 
		frameTime = ovrHmd_GetFrameTiming(hmd, 0);

		//ovr_WaitTillTime(frameTime.NextFrameSeconds - g_ActiveConfig.fTimeWarpTweak);
		Sleep(1);

		ovrLayerEyeFov ld;
		ld.Header.Type = ovrLayerType_EyeFov;
		ld.Header.Flags = (g_Config.bFlipVertical?0:ovrLayerFlag_TextureOriginAtBottomLeft) | (g_Config.bHqDistortion?ovrLayerFlag_HighQuality:0);
		for (int eye = 0; eye < 2; eye++)
		{
			ld.ColorTexture[eye] = eyeRenderTexture[eye]->TextureSet;
			ld.Viewport[eye] = eyeRenderViewport[eye];
			ld.Fov[eye] = g_eye_fov[eye];
			ld.RenderPose[eye] = g_eye_poses[eye];
		}
		ovrLayerHeader* layers = &ld.Header;
		ovrResult result = ovrHmd_SubmitFrame(hmd, 0, nullptr, &layers, 1);
#endif
	}
#endif
}

void VR_DrawAsyncTimewarpFrame()
{
#ifdef OVR_MAJOR_VERSION
#if OVR_MAJOR_VERSION <= 5
	if (g_has_rift)
	{
		auto frameTime = ovrHmd_BeginFrame(hmd, ++g_ovr_frameindex);
		g_vr_lock.unlock();

		if (0 == frameTime.TimewarpPointSeconds) {
			ovr_WaitTillTime(frameTime.TimewarpPointSeconds - 0.002);
		}
		else {
			ovr_WaitTillTime(frameTime.NextFrameSeconds - 0.008);
		}

		g_vr_lock.lock();
		// Grab the most recent textures
		for (int eye = 0; eye < 2; eye++)
		{
			((ovrGLTexture&)(g_eye_texture[eye])).OGL.TexId = m_frontBuffer[eye];
		}
#ifdef _WIN32
		//HANDLE thread_handle = g_video_backend->m_video_thread->native_handle();
		//SuspendThread(thread_handle);
#endif
		ovrHmd_EndFrame(hmd, g_front_eye_poses, &g_eye_texture[0].Texture);
#ifdef _WIN32
		//ResumeThread(thread_handle);
#endif
	}
#endif
#endif
}

}