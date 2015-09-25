// Copyright (c) 2012- PPSSPP Project.

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

#include <string.h>

#include "base/logging.h"
#include "gfx/gl_common.h"
#include "gfx/gl_debug_log.h"

#include "GPU/GLES/GLStateCache.h"
#include "GPU/GLES/FBO.h"

#ifdef IOS
extern void bindDefaultFBO();
#endif

struct FBO {
	GLuint handle[2];
	GLuint color_texture;
	GLuint z_stencil_buffer;  // Either this is set, or the two below.
	GLuint z_buffer;
	GLuint stencil_buffer;

	int width;
	int height;
	int m_EFBLayers;
	GLenum m_textureType;
	FBOColorDepth colorDepth;
	bool native_fbo;
};

static FBO *g_overriddenBackbuffer;

static GLuint currentDrawHandle_ = 0;
static GLuint currentReadHandle_ = 0;

// On PC, we always use GL_DEPTH24_STENCIL8. 
// On Android, we try to use what's available.

#ifndef USING_GLES2
FBO *fbo_ext_create(int width, int height, int num_color_textures, bool z_stencil, FBOColorDepth colorDepth) {
	GL_CHECK();
	FBO *fbo = new FBO();
	fbo->native_fbo = false;
	fbo->width = width;
	fbo->height = height;
	fbo->colorDepth = colorDepth;
	fbo->m_EFBLayers = 1;
	fbo->m_textureType = GL_TEXTURE_2D_ARRAY;

	// Color texture is same everywhere
	glGenFramebuffersEXT(fbo->m_EFBLayers, fbo->handle);
	glGenTextures(1, &fbo->color_texture);
	GL_CHECK();

	// Create the surfaces.
	glBindTexture(fbo->m_textureType, fbo->color_texture);
	glTexParameteri(fbo->m_textureType, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(fbo->m_textureType, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(fbo->m_textureType, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(fbo->m_textureType, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	GL_CHECK();

	// TODO: We could opt to only create 16-bit render targets on slow devices. For later.
	switch (colorDepth) {
	case FBO_8888:
		glTexImage3D(fbo->m_textureType, 0, GL_RGBA, width, height, fbo->m_EFBLayers, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		break;
	case FBO_4444:
		glTexImage3D(fbo->m_textureType, 0, GL_RGBA, width, height, fbo->m_EFBLayers, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, NULL);
		break;
	case FBO_5551:
		glTexImage3D(fbo->m_textureType, 0, GL_RGBA, width, height, fbo->m_EFBLayers, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, NULL);
		break;
	case FBO_565:
		glTexImage3D(fbo->m_textureType, 0, GL_RGB, width, height, fbo->m_EFBLayers, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
		break;
	}
	GL_CHECK();

	glTexParameteri(fbo->m_textureType, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(fbo->m_textureType, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(fbo->m_textureType, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(fbo->m_textureType, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	GL_CHECK();

	fbo->stencil_buffer = 0;
	fbo->z_buffer = 0;
	// 24-bit Z, 8-bit stencil
	glGenRenderbuffersEXT(1, &fbo->z_stencil_buffer);
	glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, fbo->z_stencil_buffer);
	glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_STENCIL_EXT, width, height);
	//glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8, width, height);
	GL_CHECK();

	// Bind it all together
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo->handle[0]);
	glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, fbo->m_textureType, fbo->color_texture, 0);
	glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, fbo->z_stencil_buffer);
	glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, fbo->z_stencil_buffer);
	GL_CHECK();

	GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
	switch(status) {
	case GL_FRAMEBUFFER_COMPLETE_EXT:
		// ILOG("Framebuffer verified complete.");
		break;
	case GL_FRAMEBUFFER_UNSUPPORTED_EXT:
		ELOG("GL_FRAMEBUFFER_UNSUPPORTED");
		break;
	case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT_EXT:
		ELOG("GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT ");
		break;
	default:
		FLOG("Other framebuffer error: %i", status);
		break;
	}
	// Unbind state we don't need
	glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, 0);
	glBindTexture(fbo->m_textureType, 0);
	GL_CHECK();

	currentDrawHandle_ = fbo->handle[0];
	currentReadHandle_ = fbo->handle[0];
	return fbo;
}
#endif

int fbo_check_framebuffer_status(FBO *fbo) {
	GLenum fbStatus;
#ifndef USING_GLES2
	if (!gl_extensions.ARB_framebuffer_object && gl_extensions.EXT_framebuffer_object) {
		fbStatus = glCheckFramebufferStatusEXT(GL_READ_FRAMEBUFFER);
	} else if (gl_extensions.ARB_framebuffer_object) {
		fbStatus = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
	} else {
		fbStatus = 0;
	}
#else
	fbStatus = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
#endif
	GL_CHECK();
	return (int)fbStatus;
}

FBO *fbo_create(int width, int height, int num_color_textures, bool z_stencil, FBOColorDepth colorDepth) {
	CheckGLExtensions();
	GL_CHECK();

#ifndef USING_GLES2
	if (!gl_extensions.ARB_framebuffer_object && gl_extensions.EXT_framebuffer_object) {
		return fbo_ext_create(width, height, num_color_textures, z_stencil, colorDepth);
	} else if (!gl_extensions.ARB_framebuffer_object) {
		return nullptr;
	}
	// If GLES2, we have basic FBO support and can just proceed.
#endif

	FBO *fbo = new FBO();
	fbo->native_fbo = false;
	fbo->width = width;
	fbo->height = height;
	fbo->colorDepth = colorDepth;
	fbo->m_EFBLayers = 2;
	fbo->m_textureType = GL_TEXTURE_2D_ARRAY;

	// Color texture is same everywhere
	glGenFramebuffers(fbo->m_EFBLayers, fbo->handle);
	glGenTextures(1, &fbo->color_texture);
	GL_CHECK();

	// Create the surfaces.
	glBindTexture(fbo->m_textureType, fbo->color_texture);
	glTexParameteri(fbo->m_textureType, GL_TEXTURE_MAX_LEVEL, 0);
	glTexParameteri(fbo->m_textureType, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(fbo->m_textureType, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(fbo->m_textureType, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(fbo->m_textureType, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	GL_CHECK();

	// TODO: We could opt to only create 16-bit render targets on slow devices. For later.
	switch (colorDepth) {
	case FBO_8888:
		glTexImage3D(fbo->m_textureType, 0, GL_RGBA, width, height, fbo->m_EFBLayers, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		break;
	case FBO_4444:
		glTexImage3D(fbo->m_textureType, 0, GL_RGBA, width, height, fbo->m_EFBLayers, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, NULL);
		break;
	case FBO_5551:
		glTexImage3D(fbo->m_textureType, 0, GL_RGBA, width, height, fbo->m_EFBLayers, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, NULL);
		break;
	case FBO_565:
		glTexImage3D(fbo->m_textureType, 0, GL_RGB, width, height, fbo->m_EFBLayers, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
		break;
	}
	GL_CHECK();

	glTexParameteri(fbo->m_textureType, GL_TEXTURE_MAX_LEVEL, 0);
	glTexParameteri(fbo->m_textureType, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(fbo->m_textureType, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(fbo->m_textureType, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(fbo->m_textureType, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	GL_CHECK();

	fbo->stencil_buffer = 0;
	fbo->z_buffer = 0;
	// 24-bit Z, 8-bit stencil
	glGenTextures(1, &fbo->z_stencil_buffer);
	glBindTexture(fbo->m_textureType, fbo->z_stencil_buffer);
	glTexParameteri(fbo->m_textureType, GL_TEXTURE_MAX_LEVEL, 0);
	glTexParameteri(fbo->m_textureType, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(fbo->m_textureType, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(fbo->m_textureType, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(fbo->m_textureType, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage3D(fbo->m_textureType, 0, GL_DEPTH24_STENCIL8, width, height, fbo->m_EFBLayers, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
	GL_CHECK();

	// Bind it all together
	glBindFramebuffer(GL_FRAMEBUFFER, fbo->handle[0]);
	glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, fbo->color_texture, 0);
	glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, fbo->z_stencil_buffer, 0);
	glFramebufferTexture(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, fbo->z_stencil_buffer, 0);
	GL_CHECK();

	glBindFramebuffer(GL_FRAMEBUFFER, fbo->handle[1]);
	glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, fbo->color_texture, 1);
	glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, fbo->z_stencil_buffer, 1);
	glFramebufferTexture(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, fbo->z_stencil_buffer, 1);
	GL_CHECK();

	glBindFramebuffer(GL_FRAMEBUFFER, fbo->handle[0]);
	GL_CHECK();

	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	switch(status) {
	case GL_FRAMEBUFFER_COMPLETE:
		// ILOG("Framebuffer verified complete.");
		break;
	case GL_FRAMEBUFFER_UNSUPPORTED:
		ELOG("GL_FRAMEBUFFER_UNSUPPORTED");
		break;
	case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
		ELOG("GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT ");
		break;
	default:
		FLOG("Other framebuffer error: %i", status);
		break;
	}
	GL_CHECK();

	// Unbind state we don't need
	//glBindTexture(m_textureType, 0);

	currentDrawHandle_ = fbo->handle[0];
	currentReadHandle_ = fbo->handle[0];
	return fbo;
}

static GLenum fbo_get_fb_target(bool read, GLuint **cached) {
	bool supportsBlit = gl_extensions.ARB_framebuffer_object;
	if (gl_extensions.IsGLES) {
		supportsBlit = (gl_extensions.GLES3 || gl_extensions.NV_framebuffer_blit);
	}
	GL_CHECK();

	// Note: GL_FRAMEBUFFER_EXT and GL_FRAMEBUFFER have the same value, same with _NV.
	if (supportsBlit) {
		if (read) {
			*cached = &currentReadHandle_;
			return GL_READ_FRAMEBUFFER;
		} else {
			*cached = &currentDrawHandle_;
			return GL_DRAW_FRAMEBUFFER;
		}
	} else {
		*cached = &currentDrawHandle_;
		return GL_FRAMEBUFFER;
	}
}

static void fbo_bind_fb_target(bool read, GLuint name) {
	GLuint *cached;
	GLenum target = fbo_get_fb_target(read, &cached);

	if (*cached != name) {
		if (gl_extensions.ARB_framebuffer_object || gl_extensions.IsGLES) {
			glBindFramebuffer(target, name);
		} else {
#ifndef USING_GLES2
			glBindFramebufferEXT(target, name);
#endif
		}
		*cached = name;
	}
}

void fbo_unbind() {
	if (g_overriddenBackbuffer) {
		fbo_bind_as_render_target(g_overriddenBackbuffer);
		return;
	}

	CheckGLExtensions();
#ifndef USING_GLES2
	if (gl_extensions.ARB_framebuffer_object || gl_extensions.IsGLES) {
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	} else if (gl_extensions.EXT_framebuffer_object) {
		glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
	}
#else
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
#endif

#ifdef IOS
	bindDefaultFBO();
#endif
	GL_CHECK();

	currentDrawHandle_ = 0;
	currentReadHandle_ = 0;
}

void fbo_override_backbuffer(FBO *fbo) {
	g_overriddenBackbuffer = fbo;
}

void fbo_bind_as_render_target(FBO *fbo) {
	// Without FBO_ARB / GLES3, this will collide with bind_for_read, but there's nothing
	// in ES 2.0 that actually separate them anyway of course, so doesn't matter.
	fbo_bind_fb_target(false, fbo->handle[0]);
	// Always restore viewport after render target binding
	glstate.viewport.restore();
	GL_CHECK();
}

// For GL_EXT_FRAMEBUFFER_BLIT and similar.
void fbo_bind_for_read(FBO *fbo, int eye) {
	fbo_bind_fb_target(true, fbo->handle[eye]);
	GL_CHECK();
}

void fbo_unbind_read() {
	fbo_bind_fb_target(true, 0);
	GL_CHECK();
}

void fbo_bind_color_as_texture(FBO *fbo, int color) {
	if (fbo) {
		glBindTexture(fbo->m_textureType, fbo->color_texture);
	}
	GL_CHECK();
}

void fbo_destroy(FBO *fbo) {
	if (fbo->native_fbo) {
		delete fbo;
		return;
	}

	if (gl_extensions.ARB_framebuffer_object || gl_extensions.IsGLES) {
		for (int i = 0; i < fbo->m_EFBLayers; ++i) {
			glBindFramebuffer(GL_FRAMEBUFFER, fbo->handle[i]);
			glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0, 0);
			glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, 0, 0);
		}
		GL_CHECK();
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glDeleteFramebuffers(fbo->m_EFBLayers, fbo->handle);
		glDeleteRenderbuffers(1, &fbo->z_stencil_buffer);
		glDeleteRenderbuffers(1, &fbo->z_buffer);
		glDeleteRenderbuffers(1, &fbo->stencil_buffer);
		GL_CHECK();
	}
	else if (gl_extensions.EXT_framebuffer_object) {
#ifndef USING_GLES2
		for (int i = 0; i < fbo->m_EFBLayers; ++i) {
			glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo->handle[i]);
			glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_ARRAY, 0, 0);
			glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER_EXT, 0);
		}
		GL_CHECK();
		glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
		glDeleteFramebuffersEXT(fbo->m_EFBLayers, fbo->handle);
		glDeleteRenderbuffersEXT(1, &fbo->z_stencil_buffer);
		GL_CHECK();
#endif
	}

	currentDrawHandle_ = 0;
	currentReadHandle_ = 0;

	glDeleteTextures(1, &fbo->color_texture);
	delete fbo;
}

void fbo_get_dimensions(FBO *fbo, int *w, int *h) {
	GL_CHECK();
	*w = fbo->width;
	*h = fbo->height;
}

int fbo_get_color_texture(FBO *fbo) {
	GL_CHECK();
	return fbo->color_texture;
}
