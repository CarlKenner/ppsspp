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

#ifdef _WIN32
#define SHADERLOG
#endif

#ifdef SHADERLOG
#include "Common/CommonWindows.h"
#endif

#include <map>

#include "base/logging.h"
#include "math/math_util.h"
#include "math/lin/matrix4x4.h"
#include "profiler/profiler.h"

#include "Core/Config.h"
#include "Core/Reporting.h"
#include "GPU/Math3D.h"
#include "GPU/GPUState.h"
#include "GPU/ge_constants.h"
#include "GPU/Common/VR.h"
#include "GPU/GLES/GLStateCache.h"
#include "GPU/GLES/ShaderManager.h"
#include "GPU/GLES/TransformPipeline.h"
#include "UI/OnScreenDisplay.h"
#include "Framebuffer.h"
#include "i18n/i18n.h"

#define HACK_LOG NOTICE_LOG

//VR Virtual Reality debugging variables
static float s_locked_skybox[3 * 4];
static bool s_had_skybox = false;
bool m_layer_on_top;
static bool bViewportChanged = true, bFreeLookChanged = true, bFrameChanged = true;
int vr_render_eye = -1;
int debug_viewportNum = 0;
//Viewport debug_vpList[64] = { 0 };
int debug_projNum = 0;
float debug_projList[64][7] = { 0 };
int vr_widest_3d_projNum = -1;
//EFBRectangle g_final_screen_region = EFBRectangle(0, 0, 640, 528);
//EFBRectangle g_requested_viewport = EFBRectangle(0, 0, 640, 528), g_rendered_viewport = EFBRectangle(0, 0, 640, 528);
enum ViewportType g_viewport_type = VIEW_FULLSCREEN, g_old_viewport_type = VIEW_FULLSCREEN;
enum SplitScreenType {
	SS_FULLSCREEN = 0,
	SS_2_PLAYER_SIDE_BY_SIDE,
	SS_2_PLAYER_OVER_UNDER,
	SS_QUADRANTS,
	SS_3_PLAYER_TOP,
	SS_3_PLAYER_LEFT,
	SS_3_PLAYER_RIGHT,
	SS_3_PLAYER_BOTTOM,
	SS_3_PLAYER_COLUMNS,
	SS_CUSTOM
};
enum SplitScreenType g_splitscreen_type = SS_FULLSCREEN, g_old_splitscreen_type = SS_FULLSCREEN;
bool g_is_skybox = false;

static float oldpos[3] = { 0, 0, 0 }, totalpos[3] = { 0, 0, 0 };

const char *GetViewportTypeName(ViewportType v)
{
	if (g_is_skybox)
		return "Skybox";
	switch (v)
	{
	case VIEW_FULLSCREEN:
		return "Fullscreen";
	case VIEW_LETTERBOXED:
		return "Letterboxed";
	case VIEW_HUD_ELEMENT:
		return "HUD element";
	case VIEW_OFFSCREEN:
		return "Offscreen";
	case VIEW_RENDER_TO_TEXTURE:
		return "Render to Texture";
	case VIEW_PLAYER_1:
		return "Player 1";
	case VIEW_PLAYER_2:
		return "Player 2";
	case VIEW_PLAYER_3:
		return "Player 3";
	case VIEW_PLAYER_4:
		return "Player 4";
	case VIEW_SKYBOX:
		return "Skybox";
	default:
		return "Error";
	}
}

void ClearDebugProj() { //VR
	bFrameChanged = true;

	debug_newScene = debug_nextScene;
	if (debug_newScene)
	{
		HACK_LOG(VR, "***** New scene *****");
		// General VR hacks
		vr_widest_3d_projNum = -1;
		vr_widest_3d_HFOV = 0;
		vr_widest_3d_VFOV = 0;
		vr_widest_3d_zNear = 0;
		vr_widest_3d_zFar = 0;
	}
	debug_nextScene = false;
	debug_projNum = 0;
	debug_viewportNum = 0;
	// Metroid Prime hacks
	//NewMetroidFrame();
}

void DoLogProj(int j, float p[], const char *s) { //VR
	if (j == g_Config.iSelectedLayer)
		HACK_LOG(VR, "** SELECTED LAYER:");
	if (p[6] != -1) { // orthographic projection
		//float right = p[0]-(p[0]*p[1]);
		//float left = right - 2/p[0];

		float left = -(p[1] + 1) / p[0];
		float right = left + 2 / p[0];
		float bottom = -(p[3] + 1) / p[2];
		float top = bottom + 2 / p[2];
		float zfar = p[5] / p[4];
		float znear = (1 + p[4] * zfar) / p[4];
		HACK_LOG(VR, "%d: 2D: %s (%g, %g) to (%g, %g); z: %g to %g  [%g, %g]", j, s, left, top, right, bottom, znear, zfar, p[4], p[5]);
	}
	else if (p[0] != 0 || p[2] != 0) { // perspective projection
		float f = p[5] / p[4];
		float n = f*p[4] / (p[4] - 1);
		if (p[1] != 0.0f || p[3] != 0.0f) {
			HACK_LOG(VR, "%d: %s OFF-AXIS Perspective: 2n/w=%.2f A=%.2f; 2n/h=%.2f B=%.2f; n=%.2f f=%.2f", j, s, p[0], p[1], p[2], p[3], p[4], p[5]);
			HACK_LOG(VR, "	HFOV: %.2f    VFOV: %.2f   Aspect Ratio: 16:%.1f", 2 * atan(1.0f / p[0])*180.0f / 3.1415926535f, 2 * atan(1.0f / p[2])*180.0f / 3.1415926535f, 16 / (2 / p[0])*(2 / p[2]));
		}
		else {
			HACK_LOG(VR, "%d: %s HFOV: %.2fdeg; VFOV: %.2fdeg; Aspect Ratio: 16:%.1f; near:%f, far:%f", j, s, 2 * atan(1.0f / p[0])*180.0f / 3.1415926535f, 2 * atan(1.0f / p[2])*180.0f / 3.1415926535f, 16 / (2 / p[0])*(2 / p[2]), n, f);
		}
	}
	else { // invalid
		HACK_LOG(VR, "%d: %s ZERO", j, s);
	}
}

void LogProj(const Matrix4x4 & m) { //VR
	float p[7];
	p[0] = m.xx;
	p[2] = m.yy;
	p[4] = m.zz;
	p[5] = m.wz;
	p[6] = m.zw;

	if (m.zw == -1) { // perspective projection
		p[1] = m.zx;
		p[3] = m.zy;
		// don't change this formula!
		// metroid layer detection depends on exact values
		float vfov = (2 * atan(1.0f / m.yy)*180.0f / 3.1415926535f);
		float hfov = (2 * atan(1.0f / m.xx)*180.0f / 3.1415926535f);
		float f = m.wz / m.zz;
		float n = f*m.zz / (m.zz - 1);

		if (debug_newScene && fabs(hfov) > vr_widest_3d_HFOV && fabs(hfov) <= 125 && (fabs(m.yy) != fabs(m.xx))) {
			DEBUG_LOG(VR, "***** New Widest 3D *****");

			vr_widest_3d_projNum = debug_projNum;
			vr_widest_3d_HFOV = fabs(hfov);
			vr_widest_3d_VFOV = fabs(vfov);
			vr_widest_3d_zNear = fabs(n);
			vr_widest_3d_zFar = fabs(f);
			DEBUG_LOG(VR, "%d: %g x %g deg, n=%g f=%g, p4=%g p5=%g; xs=%g ys=%g", vr_widest_3d_projNum, vr_widest_3d_HFOV, vr_widest_3d_VFOV, vr_widest_3d_zNear, vr_widest_3d_zFar, m.zz, m.wz, m.xx, m.yy);
		}
	}
	else
	{
		p[1] = m.wx;
		p[3] = m.wy;
		float left = -(m.wx + 1) / m.xx;
		float right = left + 2 / m.xx;
		float bottom = -(m.wy + 1) / m.yy;
		float top = bottom + 2 / m.yy;
		float zfar = m.wz / m.zz;
		float znear = (1 + m.zz * zfar) / m.zz;
	}

	if (debug_projNum >= 64)
		return;
	if (!debug_newScene) {
		for (int i = 0; i<7; i++) {
			if (debug_projList[debug_projNum][i] != p[i]) {
				debug_nextScene = true;
				debug_projList[debug_projNum][i] = p[i];
			}
		}
		// wait until next frame
		//if (debug_newScene) {
		//	INFO_LOG(VIDEO,"***** New scene *****");
		//	for (int j=0; j<debug_projNum; j++) {
		//		DoLogProj(j, debug_projList[j]);
		//	}
		//}
	}
	else
	{
		debug_nextScene = false;
		INFO_LOG(VR, "%f Units Per Metre", g_Config.fUnitsPerMetre);
		INFO_LOG(VR, "HUD is %.1fm away and %.1fm thick", g_Config.fHudDistance, g_Config.fHudThickness);
		DoLogProj(debug_projNum, debug_projList[debug_projNum], "unknown");
	}
	debug_projNum++;
}

Shader::Shader(const char *code, uint32_t shaderType, bool useHWTransform, const ShaderID &shaderID) : failed_(false), useHWTransform_(useHWTransform), id_(shaderID) {
	PROFILE_THIS_SCOPE("shadercomp");
	source_ = code;
#ifdef SHADERLOG
	OutputDebugStringUTF8(code);
#endif
	shader = glCreateShader(shaderType);
	glShaderSource(shader, 1, &code, 0);
	glCompileShader(shader);
	GLint success;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
	if (!success) {
#define MAX_INFO_LOG_SIZE 2048
		GLchar infoLog[MAX_INFO_LOG_SIZE];
		GLsizei len;
		glGetShaderInfoLog(shader, MAX_INFO_LOG_SIZE, &len, infoLog);
		infoLog[len] = '\0';
#ifdef ANDROID
		ELOG("Error in shader compilation! %s\n", infoLog);
		ELOG("Shader source:\n%s\n", (const char *)code);
#endif
		ERROR_LOG(G3D, "Error in shader compilation!\n");
		ERROR_LOG(G3D, "Info log: %s\n", infoLog);
		ERROR_LOG(G3D, "Shader source:\n%s\n", (const char *)code);
		Reporting::ReportMessage("Error in shader compilation: info: %s / code: %s", infoLog, (const char *)code);
#ifdef SHADERLOG
		OutputDebugStringUTF8(infoLog);
#endif
		failed_ = true;
		shader = 0;
	} else {
		DEBUG_LOG(G3D, "Compiled shader:\n%s\n", (const char *)code);
	}
}

Shader::~Shader() {
	if (shader)
		glDeleteShader(shader);
}

LinkedShader::LinkedShader(Shader *vs, Shader *fs, u32 vertType, bool useHWTransform, LinkedShader *previous)
		: useHWTransform_(useHWTransform), program(0), dirtyUniforms(0) {
	PROFILE_THIS_SCOPE("shaderlink");

	program = glCreateProgram();
	vs_ = vs;
	glAttachShader(program, vs->shader);
	glAttachShader(program, fs->shader);

	// Bind attribute locations to fixed locations so that they're
	// the same in all shaders. We use this later to minimize the calls to
	// glEnableVertexAttribArray and glDisableVertexAttribArray.
	glBindAttribLocation(program, ATTR_POSITION, "position");
	glBindAttribLocation(program, ATTR_TEXCOORD, "texcoord");
	glBindAttribLocation(program, ATTR_NORMAL, "normal");
	glBindAttribLocation(program, ATTR_W1, "w1");
	glBindAttribLocation(program, ATTR_W2, "w2");
	glBindAttribLocation(program, ATTR_COLOR0, "color0");
	glBindAttribLocation(program, ATTR_COLOR1, "color1");

#ifndef USING_GLES2
	if (gstate_c.featureFlags & GPU_SUPPORTS_DUALSOURCE_BLEND) {
		// Dual source alpha
		glBindFragDataLocationIndexed(program, 0, 0, "fragColor0");
		glBindFragDataLocationIndexed(program, 0, 1, "fragColor1");
	} else if (gl_extensions.VersionGEThan(3, 3, 0)) {
		glBindFragDataLocation(program, 0, "fragColor0");
	}
#endif

	glLinkProgram(program);

	GLint linkStatus = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);
	if (linkStatus != GL_TRUE) {
		GLint bufLength = 0;
		glGetProgramiv(program, GL_INFO_LOG_LENGTH, &bufLength);
		if (bufLength) {
			char* buf = new char[bufLength];
			glGetProgramInfoLog(program, bufLength, NULL, buf);
#ifdef ANDROID
			ELOG("Could not link program:\n %s", buf);
#endif
			ERROR_LOG(G3D, "Could not link program:\n %s", buf);
			ERROR_LOG(G3D, "VS:\n%s", vs->source().c_str());
			ERROR_LOG(G3D, "FS:\n%s", fs->source().c_str());
			Reporting::ReportMessage("Error in shader program link: info: %s / fs: %s / vs: %s", buf, fs->source().c_str(), vs->source().c_str());
#ifdef SHADERLOG
			OutputDebugStringUTF8(buf);
			OutputDebugStringUTF8(vs->source().c_str());
			OutputDebugStringUTF8(fs->source().c_str());
#endif
			delete [] buf;	// we're dead!
		}
		// Prevent a buffer overflow.
		numBones = 0;
		return;
	}

	INFO_LOG(G3D, "Linked shader: vs %i fs %i", (int)vs->shader, (int)fs->shader);

	u_tex = glGetUniformLocation(program, "tex");
	u_proj = glGetUniformLocation(program, "u_proj");
	u_proj_through = glGetUniformLocation(program, "u_proj_through");
	u_texenv = glGetUniformLocation(program, "u_texenv");
	u_fogcolor = glGetUniformLocation(program, "u_fogcolor");
	u_fogcoef = glGetUniformLocation(program, "u_fogcoef");
	u_alphacolorref = glGetUniformLocation(program, "u_alphacolorref");
	u_alphacolormask = glGetUniformLocation(program, "u_alphacolormask");
	u_stencilReplaceValue = glGetUniformLocation(program, "u_stencilReplaceValue");
	u_testtex = glGetUniformLocation(program, "testtex");

	u_fbotex = glGetUniformLocation(program, "fbotex");
	u_blendFixA = glGetUniformLocation(program, "u_blendFixA");
	u_blendFixB = glGetUniformLocation(program, "u_blendFixB");
	u_fbotexSize = glGetUniformLocation(program, "u_fbotexSize");

	// Transform
	u_view = glGetUniformLocation(program, "u_view");
	u_world = glGetUniformLocation(program, "u_world");
	u_texmtx = glGetUniformLocation(program, "u_texmtx");
	if (vertTypeGetWeightMask(vertType) != GE_VTYPE_WEIGHT_NONE)
		numBones = TranslateNumBones(vertTypeGetNumBoneWeights(vertType));
	else
		numBones = 0;
	u_depthRange = glGetUniformLocation(program, "u_depthRange");

#ifdef USE_BONE_ARRAY
	u_bone = glGetUniformLocation(program, "u_bone");
#else
	for (int i = 0; i < 8; i++) {
		char name[10];
		sprintf(name, "u_bone%i", i);
		u_bone[i] = glGetUniformLocation(program, name);
	}
#endif

	// Lighting, texturing
	u_ambient = glGetUniformLocation(program, "u_ambient");
	u_matambientalpha = glGetUniformLocation(program, "u_matambientalpha");
	u_matdiffuse = glGetUniformLocation(program, "u_matdiffuse");
	u_matspecular = glGetUniformLocation(program, "u_matspecular");
	u_matemissive = glGetUniformLocation(program, "u_matemissive");
	u_uvscaleoffset = glGetUniformLocation(program, "u_uvscaleoffset");
	u_texclamp = glGetUniformLocation(program, "u_texclamp");
	u_texclampoff = glGetUniformLocation(program, "u_texclampoff");

	for (int i = 0; i < 4; i++) {
		char temp[64];
		sprintf(temp, "u_lightpos%i", i);
		u_lightpos[i] = glGetUniformLocation(program, temp);
		sprintf(temp, "u_lightdir%i", i);
		u_lightdir[i] = glGetUniformLocation(program, temp);
		sprintf(temp, "u_lightatt%i", i);
		u_lightatt[i] = glGetUniformLocation(program, temp);
		sprintf(temp, "u_lightangle%i", i);
		u_lightangle[i] = glGetUniformLocation(program, temp);
		sprintf(temp, "u_lightspotCoef%i", i);
		u_lightspotCoef[i] = glGetUniformLocation(program, temp);
		sprintf(temp, "u_lightambient%i", i);
		u_lightambient[i] = glGetUniformLocation(program, temp);
		sprintf(temp, "u_lightdiffuse%i", i);
		u_lightdiffuse[i] = glGetUniformLocation(program, temp);
		sprintf(temp, "u_lightspecular%i", i);
		u_lightspecular[i] = glGetUniformLocation(program, temp);
	}

	attrMask = 0;
	if (-1 != glGetAttribLocation(program, "position")) attrMask |= 1 << ATTR_POSITION;
	if (-1 != glGetAttribLocation(program, "texcoord")) attrMask |= 1 << ATTR_TEXCOORD;
	if (-1 != glGetAttribLocation(program, "normal")) attrMask |= 1 << ATTR_NORMAL;
	if (-1 != glGetAttribLocation(program, "w1")) attrMask |= 1 << ATTR_W1;
	if (-1 != glGetAttribLocation(program, "w2")) attrMask |= 1 << ATTR_W2;
	if (-1 != glGetAttribLocation(program, "color0")) attrMask |= 1 << ATTR_COLOR0;
	if (-1 != glGetAttribLocation(program, "color1")) attrMask |= 1 << ATTR_COLOR1;

	availableUniforms = 0;
	if (u_proj != -1) availableUniforms |= DIRTY_PROJMATRIX;
	if (u_proj_through != -1) availableUniforms |= DIRTY_PROJTHROUGHMATRIX;
	if (u_texenv != -1) availableUniforms |= DIRTY_TEXENV;
	if (u_alphacolorref != -1) availableUniforms |= DIRTY_ALPHACOLORREF;
	if (u_alphacolormask != -1) availableUniforms |= DIRTY_ALPHACOLORMASK;
	if (u_fogcolor != -1) availableUniforms |= DIRTY_FOGCOLOR;
	if (u_fogcoef != -1) availableUniforms |= DIRTY_FOGCOEF;
	if (u_texenv != -1) availableUniforms |= DIRTY_TEXENV;
	if (u_uvscaleoffset != -1) availableUniforms |= DIRTY_UVSCALEOFFSET;
	if (u_texclamp != -1) availableUniforms |= DIRTY_TEXCLAMP;
	if (u_world != -1) availableUniforms |= DIRTY_WORLDMATRIX;
	if (u_view != -1) availableUniforms |= DIRTY_VIEWMATRIX;
	if (u_texmtx != -1) availableUniforms |= DIRTY_TEXMATRIX;
	if (u_stencilReplaceValue != -1) availableUniforms |= DIRTY_STENCILREPLACEVALUE;
	if (u_blendFixA != -1 || u_blendFixB != -1 || u_fbotexSize != -1) availableUniforms |= DIRTY_SHADERBLEND;
	if (u_depthRange != -1)
		availableUniforms |= DIRTY_DEPTHRANGE;

	// Looping up to numBones lets us avoid checking u_bone[i]
#ifdef USE_BONE_ARRAY
	if (u_bone != -1) {
		for (int i = 0; i < numBones; i++) {
			availableUniforms |= DIRTY_BONEMATRIX0 << i;
		}
	}
#else
	for (int i = 0; i < numBones; i++) {
		if (u_bone[i] != -1)
			availableUniforms |= DIRTY_BONEMATRIX0 << i;
	}
#endif
	if (u_ambient != -1) availableUniforms |= DIRTY_AMBIENT;
	if (u_matambientalpha != -1) availableUniforms |= DIRTY_MATAMBIENTALPHA;
	if (u_matdiffuse != -1) availableUniforms |= DIRTY_MATDIFFUSE;
	if (u_matemissive != -1) availableUniforms |= DIRTY_MATEMISSIVE;
	if (u_matspecular != -1) availableUniforms |= DIRTY_MATSPECULAR;
	for (int i = 0; i < 4; i++) {
		if (u_lightdir[i] != -1 ||
				u_lightspecular[i] != -1 ||
				u_lightpos[i] != -1)
			availableUniforms |= DIRTY_LIGHT0 << i;
	}

	glUseProgram(program);

	// Default uniform values
	glUniform1i(u_tex, 0);
	glUniform1i(u_fbotex, 1);
	glUniform1i(u_testtex, 2);
	// The rest, use the "dirty" mechanism.
	dirtyUniforms = DIRTY_ALL;
	bFrameChanged = true;
	use(vertType, previous);
}

LinkedShader::~LinkedShader() {
	// Shaders are automatically detached by glDeleteProgram.
	glDeleteProgram(program);
}

// Utility
static void SetColorUniform3(int uniform, u32 color) {
	const float col[3] = {
		((color & 0xFF)) / 255.0f,
		((color & 0xFF00) >> 8) / 255.0f,
		((color & 0xFF0000) >> 16) / 255.0f
	};
	glUniform3fv(uniform, 1, col);
}

static void SetColorUniform3Alpha(int uniform, u32 color, u8 alpha) {
	const float col[4] = {
		((color & 0xFF)) / 255.0f,
		((color & 0xFF00) >> 8) / 255.0f,
		((color & 0xFF0000) >> 16) / 255.0f,
		alpha/255.0f
	};
	glUniform4fv(uniform, 1, col);
}

// This passes colors unscaled (e.g. 0 - 255 not 0 - 1.)
static void SetColorUniform3Alpha255(int uniform, u32 color, u8 alpha) {
	if (gl_extensions.gpuVendor == GPU_VENDOR_POWERVR) {
		const float col[4] = {
			(float)((color & 0xFF) >> 0) * (1.0f / 255.0f),
			(float)((color & 0xFF00) >> 8) * (1.0f / 255.0f),
			(float)((color & 0xFF0000) >> 16) * (1.0f / 255.0f),
			(float)alpha * (1.0f / 255.0f)
		};
		glUniform4fv(uniform, 1, col);
	} else {
		const float col[4] = {
			(float)((color & 0xFF) >> 0),
			(float)((color & 0xFF00) >> 8),
			(float)((color & 0xFF0000) >> 16),
			(float)alpha 
		};
		glUniform4fv(uniform, 1, col);
	}
}

static void SetColorUniform3iAlpha(int uniform, u32 color, u8 alpha) {
	const int col[4] = {
		(int)((color & 0xFF) >> 0),
		(int)((color & 0xFF00) >> 8),
		(int)((color & 0xFF0000) >> 16),
		(int)alpha,
	};
	glUniform4iv(uniform, 1, col);
}

static void SetColorUniform3ExtraFloat(int uniform, u32 color, float extra) {
	const float col[4] = {
		((color & 0xFF)) / 255.0f,
		((color & 0xFF00) >> 8) / 255.0f,
		((color & 0xFF0000) >> 16) / 255.0f,
		extra
	};
	glUniform4fv(uniform, 1, col);
}

static void SetFloat24Uniform3(int uniform, const u32 data[3]) {
	const u32 col[3] = {
		data[0] << 8, data[1] << 8, data[2] << 8
	};
	glUniform3fv(uniform, 1, (const GLfloat *)&col[0]);
}

static void SetFloatUniform4(int uniform, float data[4]) {
	glUniform4fv(uniform, 1, data);
}

static void SetMatrix4x3(int uniform, const float *m4x3) {
	float m4x4[16];
	ConvertMatrix4x3To4x4(m4x4, m4x3);
	glUniformMatrix4fv(uniform, 1, GL_FALSE, m4x4);
}

static inline void ScaleProjMatrix(Matrix4x4 &in) {
	const Vec3 trans(gstate_c.vpXOffset, gstate_c.vpYOffset, 0.0f);
	const Vec3 scale(gstate_c.vpWidthScale, gstate_c.vpHeightScale, 1.0);
	in.translateAndScale(trans, scale);
}

void LinkedShader::use(u32 vertType, LinkedShader *previous) {
	glUseProgram(program);
	UpdateUniforms(vertType);
	int enable, disable;
	if (previous) {
		enable = attrMask & ~previous->attrMask;
		disable = (~attrMask) & previous->attrMask;
	} else {
		enable = attrMask;
		disable = ~attrMask;
	}
	for (int i = 0; i < ATTR_COUNT; i++) {
		if (enable & (1 << i))
			glEnableVertexAttribArray(i);
		else if (disable & (1 << i))
			glDisableVertexAttribArray(i);
	}
}

void LinkedShader::stop() {
	for (int i = 0; i < ATTR_COUNT; i++) {
		if (attrMask & (1 << i))
			glDisableVertexAttribArray(i);
	}
}

void LinkedShader::UpdateUniforms(u32 vertType) {
	u32 dirty = dirtyUniforms & availableUniforms;
	dirtyUniforms = 0;
	if (!dirty)
		return;

	static bool temp_skybox = false;
	bool position_changed = false, skybox_changed = false;

	// Update any dirty uniforms before we draw
	if (dirty & DIRTY_TEXENV) {
		SetColorUniform3(u_texenv, gstate.texenvcolor);
	}
	if (dirty & DIRTY_ALPHACOLORREF) {
		SetColorUniform3Alpha255(u_alphacolorref, gstate.getColorTestRef(), gstate.getAlphaTestRef() & gstate.getAlphaTestMask());
	}
	if (dirty & DIRTY_ALPHACOLORMASK) {
		SetColorUniform3iAlpha(u_alphacolormask, gstate.colortestmask, gstate.getAlphaTestMask());
	}
	if (dirty & DIRTY_FOGCOLOR) {
		SetColorUniform3(u_fogcolor, gstate.fogcolor);
	}
	if (dirty & DIRTY_FOGCOEF) {
		float fogcoef[2] = {
			getFloat24(gstate.fog1),
			getFloat24(gstate.fog2),
		};
		if (my_isinf(fogcoef[1])) {
			// not really sure what a sensible value might be.
			fogcoef[1] = fogcoef[1] < 0.0f ? -10000.0f : 10000.0f;
		} else if (my_isnan(fogcoef[1])) {
			// Workaround for https://github.com/hrydgard/ppsspp/issues/5384#issuecomment-38365988
			// Just put the fog far away at a large finite distance.
			// Infinities and NaNs are rather unpredictable in shaders on many GPUs
			// so it's best to just make it a sane calculation.
			fogcoef[0] = 100000.0f;
			fogcoef[1] = 1.0f;
		}
#ifndef MOBILE_DEVICE
		else if (my_isnanorinf(fogcoef[1]) || my_isnanorinf(fogcoef[0])) {
			ERROR_LOG_REPORT_ONCE(fognan, G3D, "Unhandled fog NaN/INF combo: %f %f", fogcoef[0], fogcoef[1]);
		}
#endif
		glUniform2fv(u_fogcoef, 1, fogcoef);
	}

	// Texturing

	// If this dirty check is changed to true, Frontier Gate Boost works in texcoord speedhack mode.
	// This means that it's not a flushing issue.
	// It uses GE_TEXMAP_TEXTURE_MATRIX with GE_PROJMAP_UV a lot.
	// Can't figure out why it doesn't dirty at the right points though...
	if (dirty & DIRTY_UVSCALEOFFSET) {
		const float invW = 1.0f / (float)gstate_c.curTextureWidth;
		const float invH = 1.0f / (float)gstate_c.curTextureHeight;
		const int w = gstate.getTextureWidth(0);
		const int h = gstate.getTextureHeight(0);
		const float widthFactor = (float)w * invW;
		const float heightFactor = (float)h * invH;

		static const float rescale[4] = {1.0f, 2*127.5f/128.f, 2*32767.5f/32768.f, 1.0f};
		const float factor = rescale[(vertType & GE_VTYPE_TC_MASK) >> GE_VTYPE_TC_SHIFT];

		float uvscaleoff[4];

		switch (gstate.getUVGenMode()) {
		case GE_TEXMAP_TEXTURE_COORDS:
			// Not sure what GE_TEXMAP_UNKNOWN is, but seen in Riviera.  Treating the same as GE_TEXMAP_TEXTURE_COORDS works.
		case GE_TEXMAP_UNKNOWN:
			if (g_Config.bPrescaleUV) {
				// Shouldn't even get here as we won't use the uniform in the shader.
				// We are here but are prescaling UV in the decoder? Let's do the same as in the other case
				// except consider *Scale and *Off to be 1 and 0.
				uvscaleoff[0] = widthFactor;
				uvscaleoff[1] = heightFactor;
				uvscaleoff[2] = 0.0f;
				uvscaleoff[3] = 0.0f;
			} else {
				uvscaleoff[0] = gstate_c.uv.uScale * factor * widthFactor;
				uvscaleoff[1] = gstate_c.uv.vScale * factor * heightFactor;
				uvscaleoff[2] = gstate_c.uv.uOff * widthFactor;
				uvscaleoff[3] = gstate_c.uv.vOff * heightFactor;
			}
			break;

		// These two work the same whether or not we prescale UV.

		case GE_TEXMAP_TEXTURE_MATRIX:
			// We cannot bake the UV coord scale factor in here, as we apply a matrix multiplication
			// before this is applied, and the matrix multiplication may contain translation. In this case
			// the translation will be scaled which breaks faces in Hexyz Force for example.
			// So I've gone back to applying the scale factor in the shader.
			uvscaleoff[0] = widthFactor;
			uvscaleoff[1] = heightFactor;
			uvscaleoff[2] = 0.0f;
			uvscaleoff[3] = 0.0f;
			break;

		case GE_TEXMAP_ENVIRONMENT_MAP:
			// In this mode we only use uvscaleoff to scale to the texture size.
			uvscaleoff[0] = widthFactor;
			uvscaleoff[1] = heightFactor;
			uvscaleoff[2] = 0.0f;
			uvscaleoff[3] = 0.0f;
			break;

		default:
			ERROR_LOG_REPORT(G3D, "Unexpected UV gen mode: %d", gstate.getUVGenMode());
		}
		glUniform4fv(u_uvscaleoffset, 1, uvscaleoff);
	}

	if ((dirty & DIRTY_TEXCLAMP) && u_texclamp != -1) {
		const float invW = 1.0f / (float)gstate_c.curTextureWidth;
		const float invH = 1.0f / (float)gstate_c.curTextureHeight;
		const int w = gstate.getTextureWidth(0);
		const int h = gstate.getTextureHeight(0);
		const float widthFactor = (float)w * invW;
		const float heightFactor = (float)h * invH;

		// First wrap xy, then half texel xy (for clamp.)
		const float texclamp[4] = {
			widthFactor,
			heightFactor,
			invW * 0.5f,
			invH * 0.5f,
		};
		const float texclampoff[2] = {
			gstate_c.curTextureXOffset * invW,
			gstate_c.curTextureYOffset * invH,
		};
		glUniform4fv(u_texclamp, 1, texclamp);
		if (u_texclampoff != -1) {
			glUniform2fv(u_texclampoff, 1, texclampoff);
		}
	}

	// Transform
	if (dirty & DIRTY_WORLDMATRIX) {
		SetMatrix4x3(u_world, gstate.worldMatrix);
		position_changed = true;
	}
	if (dirty & DIRTY_VIEWMATRIX) {
		SetMatrix4x3(u_view, gstate.viewMatrix);
	}
	if (dirty & DIRTY_TEXMATRIX) {
		SetMatrix4x3(u_texmtx, gstate.tgenMatrix);
	}
	if ((dirty & DIRTY_DEPTHRANGE) && u_depthRange != -1) {
		float viewZScale = gstate.getViewportZScale();
		float viewZCenter = gstate.getViewportZCenter();
		float viewZInvScale;
		if (viewZScale != 0.0) {
			viewZInvScale = 1.0f / viewZScale;
		} else {
			viewZInvScale = 0.0;
		}
		float data[4] = { viewZScale, viewZCenter, viewZCenter, viewZInvScale };
		SetFloatUniform4(u_depthRange, data);
	}

	if (position_changed && temp_skybox)
	{
		g_is_skybox = false;
		temp_skybox = false;
		skybox_changed = true;
	}
	if (bViewportChanged)
	{
		bViewportChanged = false;
		// VR, Check whether it is a skybox, fullscreen, letterboxed, splitscreen multiplayer, hud element, or offscreen
		//SetViewportType(xfmem.viewport);
		//LogViewport(xfmem.viewport);

		//SetViewportConstants();

		// Update projection if the viewport isn't 1:1 useable
		//if (!g_ActiveConfig.backend_info.bSupportsOversizedViewports)
		//{
		//	ViewportCorrectionMatrix(s_viewportCorrection);
		//	skybox_changed = true;
		//}
		// VR adjust the projection matrix for the new kind of viewport
		//else if (g_viewport_type != g_old_viewport_type)
		//{
		//	skybox_changed = true;
		//}
	}
	if (position_changed && g_Config.bDetectSkybox && !g_is_skybox)
	{
		//CheckSkybox();
		if (g_is_skybox)
		{
			temp_skybox = true;
			skybox_changed = true;
		}
	}
	if (dirty & DIRTY_PROJMATRIX || (bFrameChanged && g_Config.bEnableVR && g_has_hmd)) {
		if (g_Config.bEnableVR && g_has_hmd) {
			bFrameChanged = false;
			SetProjectionConstants(dirty & DIRTY_PROJMATRIX);
			//bProjectionChanged = false;
		}
		else {
			if (g_has_hmd)
				UpdateHeadTrackingIfNeeded();
			Matrix4x4 flippedMatrix;
			memcpy(&flippedMatrix, gstate.projMatrix, 16 * sizeof(float));

			const bool invertedY = gstate_c.vpHeight < 0;
			if (invertedY) {
				flippedMatrix[5] = -flippedMatrix[5];
				flippedMatrix[13] = -flippedMatrix[13];
			}
			const bool invertedX = gstate_c.vpWidth < 0;
			if (invertedX) {
				flippedMatrix[0] = -flippedMatrix[0];
				flippedMatrix[12] = -flippedMatrix[12];
			}

			// In Phantasy Star Portable 2, depth range sometimes goes negative and is clamped by glDepthRange to 0,
			// causing graphics clipping glitch (issue #1788). This hack modifies the projection matrix to work around it.
			if (g_Config.bDepthRangeHack) {
				float zScale = gstate.getViewportZScale() / 65535.0f;
				float zCenter = gstate.getViewportZCenter() / 65535.0f;

				// if far depth range < 0
				if (zCenter + zScale < 0.0f) {
					// if perspective projection
					if (flippedMatrix[11] < 0.0f) {
						float depthMax = gstate.getDepthRangeMax() / 65535.0f;
						float depthMin = gstate.getDepthRangeMin() / 65535.0f;

						float a = flippedMatrix[10];
						float b = flippedMatrix[14];

						float n = b / (a - 1.0f);
						float f = b / (a + 1.0f);

						f = (n * f) / (n + ((zCenter + zScale) * (n - f) / (depthMax - depthMin)));

						a = (n + f) / (n - f);
						b = (2.0f * n * f) / (n - f);

						if (!my_isnan(a) && !my_isnan(b)) {
							flippedMatrix[10] = a;
							flippedMatrix[14] = b;
						}
					}
				}
			}

			ScaleProjMatrix(flippedMatrix);

			glUniformMatrix4fv(u_proj, 1, GL_FALSE, flippedMatrix.m);
		}
	} 
	else if (skybox_changed && g_Config.bEnableVR && g_has_hmd)
	{
		SetProjectionConstants(false);
	}
	if (dirty & DIRTY_PROJTHROUGHMATRIX)
	{
		Matrix4x4 proj_through;
		proj_through.setOrtho(0.0f, gstate_c.curRTWidth, gstate_c.curRTHeight, 0, 0.0f, 1.0f);
		glUniformMatrix4fv(u_proj_through, 1, GL_FALSE, proj_through.getReadPtr());
	}
	//if (g_Config.iMotionSicknessSkybox == 2 && g_is_skybox)
	//	LockSkybox();

	if (dirty & DIRTY_STENCILREPLACEVALUE) {
		glUniform1f(u_stencilReplaceValue, (float)gstate.getStencilTestRef() * (1.0f / 255.0f));
	}
	// TODO: Could even set all bones in one go if they're all dirty.
#ifdef USE_BONE_ARRAY
	if (u_bone != -1) {
		float allBones[8 * 16];

		bool allDirty = true;
		for (int i = 0; i < numBones; i++) {
			if (dirty & (DIRTY_BONEMATRIX0 << i)) {
				ConvertMatrix4x3To4x4(allBones + 16 * i, gstate.boneMatrix + 12 * i);
			} else {
				allDirty = false;
			}
		}
		if (allDirty) {
			// Set them all with one call
			glUniformMatrix4fv(u_bone, numBones, GL_FALSE, allBones);
		} else {
			// Set them one by one. Could try to coalesce two in a row etc but too lazy.
			for (int i = 0; i < numBones; i++) {
				if (dirty & (DIRTY_BONEMATRIX0 << i)) {
					glUniformMatrix4fv(u_bone + i, 1, GL_FALSE, allBones + 16 * i);
				}
			}
		}
	}
#else
	float bonetemp[16];
	for (int i = 0; i < numBones; i++) {
		if (dirty & (DIRTY_BONEMATRIX0 << i)) {
			ConvertMatrix4x3To4x4(bonetemp, gstate.boneMatrix + 12 * i);
			glUniformMatrix4fv(u_bone[i], 1, GL_FALSE, bonetemp);
		}
	}
#endif

	if (dirty & DIRTY_SHADERBLEND) {
		if (u_blendFixA != -1) {
			SetColorUniform3(u_blendFixA, gstate.getFixA());
		}
		if (u_blendFixB != -1) {
			SetColorUniform3(u_blendFixB, gstate.getFixB());
		}

		const float fbotexSize[2] = {
			1.0f / (float)gstate_c.curRTRenderWidth,
			1.0f / (float)gstate_c.curRTRenderHeight,
		};
		if (u_fbotexSize != -1) {
			glUniform2fv(u_fbotexSize, 1, fbotexSize);
		}
	}

	// Lighting
	if (dirty & DIRTY_AMBIENT) {
		SetColorUniform3Alpha(u_ambient, gstate.ambientcolor, gstate.getAmbientA());
	}
	if (dirty & DIRTY_MATAMBIENTALPHA) {
		SetColorUniform3Alpha(u_matambientalpha, gstate.materialambient, gstate.getMaterialAmbientA());
	}
	if (dirty & DIRTY_MATDIFFUSE) {
		SetColorUniform3(u_matdiffuse, gstate.materialdiffuse);
	}
	if (dirty & DIRTY_MATEMISSIVE) {
		SetColorUniform3(u_matemissive, gstate.materialemissive);
	}
	if (dirty & DIRTY_MATSPECULAR) {
		SetColorUniform3ExtraFloat(u_matspecular, gstate.materialspecular, getFloat24(gstate.materialspecularcoef));
	}

	for (int i = 0; i < 4; i++) {
		if (dirty & (DIRTY_LIGHT0 << i)) {
			if (gstate.isDirectionalLight(i)) {
				// Prenormalize
				float x = getFloat24(gstate.lpos[i * 3 + 0]);
				float y = getFloat24(gstate.lpos[i * 3 + 1]);
				float z = getFloat24(gstate.lpos[i * 3 + 2]);
				float len = sqrtf(x*x + y*y + z*z);
				if (len == 0.0f)
					len = 1.0f;
				else
					len = 1.0f / len;
				float vec[3] = { x * len, y * len, z * len };
				glUniform3fv(u_lightpos[i], 1, vec);
			} else {
				SetFloat24Uniform3(u_lightpos[i], &gstate.lpos[i * 3]);
			}
			if (u_lightdir[i] != -1) SetFloat24Uniform3(u_lightdir[i], &gstate.ldir[i * 3]);
			if (u_lightatt[i] != -1) SetFloat24Uniform3(u_lightatt[i], &gstate.latt[i * 3]);
			if (u_lightangle[i] != -1) glUniform1f(u_lightangle[i], getFloat24(gstate.lcutoff[i]));
			if (u_lightspotCoef[i] != -1) glUniform1f(u_lightspotCoef[i], getFloat24(gstate.lconv[i]));
			if (u_lightambient[i] != -1) SetColorUniform3(u_lightambient[i], gstate.lcolor[i * 3]);
			if (u_lightdiffuse[i] != -1) SetColorUniform3(u_lightdiffuse[i], gstate.lcolor[i * 3 + 1]);
			if (u_lightspecular[i] != -1) SetColorUniform3(u_lightspecular[i], gstate.lcolor[i * 3 + 2]);
		}
	}
}

void LinkedShader::SetProjectionConstants(bool shouldLog) {
	Matrix4x4 flippedMatrix;
	memcpy(&flippedMatrix, gstate.projMatrix, 16 * sizeof(float));

	bool isPerspective = flippedMatrix.zw == -1;

	const bool invertedY = gstate_c.vpHeight < 0;
	if (invertedY) {
		flippedMatrix[5] = -flippedMatrix[5];
		flippedMatrix[13] = -flippedMatrix[13];
	}
	const bool invertedX = gstate_c.vpWidth < 0;
	if (invertedX) {
		flippedMatrix[0] = -flippedMatrix[0];
		flippedMatrix[12] = -flippedMatrix[12];
	}

	// In Phantasy Star Portable 2, depth range sometimes goes negative and is clamped by glDepthRange to 0,
	// causing graphics clipping glitch (issue #1788). This hack modifies the projection matrix to work around it.
	if (g_Config.bDepthRangeHack) {
		float zScale = gstate.getViewportZScale() / 65535.0f;
		float zCenter = gstate.getViewportZCenter() / 65535.0f;

		// if far depth range < 0
		if (zCenter + zScale < 0.0f) {
			// if perspective projection
			if (flippedMatrix[11] < 0.0f) {
				float depthMax = gstate.getDepthRangeMax() / 65535.0f;
				float depthMin = gstate.getDepthRangeMin() / 65535.0f;

				float a = flippedMatrix[10];
				float b = flippedMatrix[14];

				float n = b / (a - 1.0f);
				float f = b / (a + 1.0f);

				f = (n * f) / (n + ((zCenter + zScale) * (n - f) / (depthMax - depthMin)));

				a = (n + f) / (n - f);
				b = (2.0f * n * f) / (n - f);

				if (!my_isnan(a) && !my_isnan(b)) {
					flippedMatrix[10] = a;
					flippedMatrix[14] = b;
				}
			}
		}
	}
	if (shouldLog)
		LogProj(flippedMatrix);

	///////////////////////////////////////////////////////
	// First, identify any special layers and hacks

	m_layer_on_top = false;
	bool bFullscreenLayer = g_Config.bHudFullscreen && !isPerspective;
	bool bFlashing = (debug_projNum - 1) == g_Config.iSelectedLayer;
	bool bStuckToHead = false, bHide = false;
	int flipped_x = 1, flipped_y = 1, iTelescopeHack = -1;
	float fScaleHack = 1, fWidthHack = 1, fHeightHack = 1, fUpHack = 0, fRightHack = 0;

	if (g_Config.iMetroidPrime)
	{
		//GetMetroidPrimeValues(&bStuckToHead, &bFullscreenLayer, &bHide, &bFlashing,
		//	&fScaleHack, &fWidthHack, &fHeightHack, &fUpHack, &fRightHack, &iTelescopeHack);
	}

	// VR: in split-screen, only draw VR player TODO: fix offscreen to render to a separate texture in VR 
	bHide = bHide || (g_has_hmd && (g_viewport_type == VIEW_OFFSCREEN || (g_viewport_type >= VIEW_PLAYER_1 && g_viewport_type <= VIEW_PLAYER_4 && g_Config.iVRPlayer != g_viewport_type - VIEW_PLAYER_1)));
	// flash selected layer for debugging
	bHide = bHide || (bFlashing && g_Config.iFlashState > 5);
	// hide skybox or everything to reduce motion sickness
	bHide = bHide || (g_is_skybox && g_Config.iMotionSicknessSkybox == 1) || g_vr_black_screen;

	// Split WidthHack and HeightHack into left and right versions for telescopes
	float fLeftWidthHack = fWidthHack, fRightWidthHack = fWidthHack;
	float fLeftHeightHack = fHeightHack, fRightHeightHack = fHeightHack;
	bool bHideLeft = bHide, bHideRight = bHide, bTelescopeHUD = false, bNoForward = false;
	if (iTelescopeHack < 0 && g_Config.iTelescopeEye && vr_widest_3d_VFOV <= g_Config.fTelescopeMaxFOV && vr_widest_3d_VFOV > 1
		&& (g_Config.fTelescopeMaxFOV <= g_Config.fMinFOV || (g_Config.fTelescopeMaxFOV > g_Config.fMinFOV && vr_widest_3d_VFOV > g_Config.fMinFOV)))
		iTelescopeHack = g_Config.iTelescopeEye;
	if (g_has_hmd && iTelescopeHack > 0)
	{
		bNoForward = true;
		// Calculate telescope scale
		float hmd_halftan, telescope_scale;
		VR_GetProjectionHalfTan(hmd_halftan);
		telescope_scale = fabs(hmd_halftan / tan(DEGREES_TO_RADIANS(vr_widest_3d_VFOV) / 2));
		if (iTelescopeHack & 1)
		{
			fLeftWidthHack *= telescope_scale;
			fLeftHeightHack *= telescope_scale;
			bHideLeft = false;
		}
		if (iTelescopeHack & 2)
		{
			fRightWidthHack *= telescope_scale;
			fRightHeightHack *= telescope_scale;
			bHideRight = false;
		}
	}

	///////////////////////////////////////////////////////
	// What happens last depends on what kind of rendering we are doing for this layer
	// Hide: don't render anything
	// Render to texture: render in 2D exactly the same as the real console would, for projection shadows etc.
	// Free Look
	// Normal emulation
	// VR Fullscreen layer: render EFB copies or screenspace effects so they fill the full screen they were copied from
	// VR: Render everything as part of a virtual world, there are a few separate alternatives here:
	//     2D HUD as thick pane of glass floating in 3D space
	//     3D HUD element as a 3D object attached to that pane of glass
	//     3D world

	float UnitsPerMetre = g_Config.fUnitsPerMetre * fScaleHack / g_Config.fScale;

	bHide = bHide && (bFlashing || (g_has_hmd && g_Config.bEnableVR));

	if (bHide)
	{
		// If we are supposed to hide the layer, zero out the projection matrix
		Matrix4x4 final_matrix;
		final_matrix.empty();
		glUniformMatrix4fv(u_proj, 1, GL_FALSE, final_matrix.m);
		return;
	}
	// don't do anything fancy for rendering to a texture
	// render exactly as we are told, and in mono
	else if (g_viewport_type == VIEW_RENDER_TO_TEXTURE)
	{
		// we aren't applying viewport correction, because Render To Texture never has a viewport larger than the framebufffer

		glUniformMatrix4fv(u_proj, 1, GL_FALSE, flippedMatrix.m);
		return;
	}
	// This was already copied from the fullscreen EFB.
	// Which makes it already correct for the HMD's FOV.
	// But we still need to correct it for the difference between the requested and rendered viewport.
	// Don't add any stereoscopy because that was already done when copied.
	else if (bFullscreenLayer)
	{
		ScaleProjMatrix(flippedMatrix);

		glUniformMatrix4fv(u_proj, 1, GL_FALSE, flippedMatrix.m);
		return;
	}

	// VR HMD 3D projection matrix, needs to include head-tracking

	// near clipping plane in game units
	float zfar, znear, zNear3D, hfov, vfov;

	// if the camera is zoomed in so much that the action only fills a tiny part of your FOV,
	// we need to move the camera forwards until objects at AimDistance fill the minimum FOV.
	float zoom_forward = 0.0f;
	if (vr_widest_3d_HFOV <= g_Config.fMinFOV && vr_widest_3d_HFOV > 0 && iTelescopeHack <= 0)
	{
		zoom_forward = g_Config.fAimDistance * tanf(DEGREES_TO_RADIANS(g_Config.fMinFOV) / 2) / tanf(DEGREES_TO_RADIANS(vr_widest_3d_HFOV) / 2);
		zoom_forward -= g_Config.fAimDistance;
	}

	// Real 3D scene
	if (isPerspective && g_viewport_type != VIEW_HUD_ELEMENT && g_viewport_type != VIEW_OFFSCREEN)
	{
		float p5 = flippedMatrix.wz;
		float p4 = flippedMatrix.zz;
		zfar = flippedMatrix.wz / flippedMatrix.zz;
		znear = (1 + flippedMatrix.zz * zfar) / flippedMatrix.zz;
		float zn2 = p5 / (p4 - 1);
		float zf2 = p5 / (p4 + 1);
		hfov = 2 * atan(1.0f / flippedMatrix.xx)*180.0f / 3.1415926535f;
		vfov = 2 * atan(1.0f / flippedMatrix.yy)*180.0f / 3.1415926535f;
		if (debug_newScene)
			INFO_LOG(VR, "Real 3D scene: hfov=%8.4f    vfov=%8.4f      znear=%8.4f or %8.4f   zfar=%8.4f or %8.4f", hfov, vfov, znear, zn2, zfar, zf2);

		// Find the game's camera angle and position by looking at the view/model matrix of the first real 3D object drawn.
		// This won't work for all games.
		if (!g_vr_had_3D_already) {
			//CheckOrientationConstants();
			g_vr_had_3D_already = true;
		}

		//NOTICE_LOG(VR, "G [%8.4f %8.4f %8.4f   %8.4f]", flippedMatrix.data[0 * 4 + 0], flippedMatrix.data[0 * 4 + 1], flippedMatrix.data[0 * 4 + 2], flippedMatrix.data[0 * 4 + 3]);
		//NOTICE_LOG(VR, "G [%8.4f %8.4f %8.4f   %8.4f]", flippedMatrix.data[1 * 4 + 0], flippedMatrix.data[1 * 4 + 1], flippedMatrix.data[1 * 4 + 2], flippedMatrix.data[1 * 4 + 3]);
		//NOTICE_LOG(VR, "G [%8.4f %8.4f %8.4f   %8.4f]", flippedMatrix.data[2 * 4 + 0], flippedMatrix.data[2 * 4 + 1], flippedMatrix.data[2 * 4 + 2], flippedMatrix.data[2 * 4 + 3]);
		//NOTICE_LOG(VR, "G {%8.4f %8.4f %8.4f   %8.4f}", flippedMatrix.data[3 * 4 + 0], flippedMatrix.data[3 * 4 + 1], flippedMatrix.data[3 * 4 + 2], flippedMatrix.data[3 * 4 + 3]);
		//WARN_LOG(VR, "---");
	}
	// 2D layer we will turn into a 3D scene
	// or 3D HUD element that we will treat like a part of the 2D HUD 
	else
	{
		m_layer_on_top = g_Config.bHudOnTop;
		if (vr_widest_3d_HFOV > 0)
		{
			znear = vr_widest_3d_zNear;
			zfar = vr_widest_3d_zFar;
			if (zoom_forward != 0)
			{
				hfov = g_Config.fMinFOV;
				vfov = g_Config.fMinFOV * vr_widest_3d_VFOV / vr_widest_3d_HFOV;
			}
			else
			{
				hfov = vr_widest_3d_HFOV;
				vfov = vr_widest_3d_VFOV;
			}
			if (debug_newScene)
				INFO_LOG(VR, "2D to fit 3D world: hfov=%8.4f    vfov=%8.4f      znear=%8.4f   zfar=%8.4f", hfov, vfov, znear, zfar);
		}
		else
		{
			// default, if no 3D in scene
			znear = 0.2f*UnitsPerMetre * 20; // 50cm
			zfar = 40 * UnitsPerMetre; // 40m
			hfov = 70; // 70 degrees
			vfov = 180.0f / 3.14159f * 2 * atanf(tanf((hfov*3.14159f / 180.0f) / 2)* 9.0f / 16.0f); // 2D screen is always meant to be 16:9 aspect ratio
			// TODO: fix aspect ratio in portrait mode
			if (debug_newScene)
				DEBUG_LOG(VR, "Only 2D Projecting: %g x %g, n=%fm f=%fm", hfov, vfov, znear, zfar);
		}
		zNear3D = znear;
		znear /= 40.0f;
		if (debug_newScene)
			DEBUG_LOG(VR, "2D: zNear3D = %f, znear = %f, zFar = %f", zNear3D, znear, zfar);
		//ERROR_LOG(VR, "2D Matrix!");
		//ERROR_LOG(VR, "G [%8.4f %8.4f %8.4f   %8.4f]", flippedMatrix.data[0 * 4 + 0], flippedMatrix.data[0 * 4 + 1], flippedMatrix.data[0 * 4 + 2], flippedMatrix.data[0 * 4 + 3]);
		//ERROR_LOG(VR, "G [%8.4f %8.4f %8.4f   %8.4f]", flippedMatrix.data[1 * 4 + 0], flippedMatrix.data[1 * 4 + 1], flippedMatrix.data[1 * 4 + 2], flippedMatrix.data[1 * 4 + 3]);
		//ERROR_LOG(VR, "G [%8.4f %8.4f %8.4f   %8.4f]", flippedMatrix.data[2 * 4 + 0], flippedMatrix.data[2 * 4 + 1], flippedMatrix.data[2 * 4 + 2], flippedMatrix.data[2 * 4 + 3]);
		//ERROR_LOG(VR, "G {%8.4f %8.4f %8.4f   %8.4f}", flippedMatrix.data[3 * 4 + 0], flippedMatrix.data[3 * 4 + 1], flippedMatrix.data[3 * 4 + 2], flippedMatrix.data[3 * 4 + 3]);
		//WARN_LOG(VR, "---");
	}

	Matrix44 proj_left, proj_right, hmd_left, hmd_right, temp;
	Matrix44::Set(proj_left, flippedMatrix.data);
	Matrix44::Set(proj_right, flippedMatrix.data);
	VR_GetProjectionMatrices(temp, hmd_right, znear, zfar);
	hmd_left = temp.transpose();
	temp = hmd_right;
	hmd_right = temp.transpose();
	proj_left.xx = hmd_left.xx;
	proj_left.yy = hmd_left.yy;
	float hfov2 = 2 * atan(1.0f / hmd_left.data[0 * 4 + 0])*180.0f / 3.1415926535f;
	float vfov2 = 2 * atan(1.0f / hmd_left.data[1 * 4 + 1])*180.0f / 3.1415926535f;
	float zfar2 = hmd_left.wz / hmd_left.zz;
	float znear2 = (1 + hmd_left.zz * zfar) / hmd_left.zz;
	if (debug_newScene)
	{
		// yellow = HMD's suggestion
		DEBUG_LOG(VR, "O hfov=%8.4f    vfov=%8.4f      znear=%8.4f   zfar=%8.4f", hfov2, vfov2, znear2, zfar2);
		DEBUG_LOG(VR, "O [%8.4f %8.4f %8.4f   %8.4f]", hmd_left.data[0 * 4 + 0], hmd_left.data[0 * 4 + 1], hmd_left.data[0 * 4 + 2], hmd_left.data[0 * 4 + 3]);
		DEBUG_LOG(VR, "O [%8.4f %8.4f %8.4f   %8.4f]", hmd_left.data[1 * 4 + 0], hmd_left.data[1 * 4 + 1], hmd_left.data[1 * 4 + 2], hmd_left.data[1 * 4 + 3]);
		DEBUG_LOG(VR, "O [%8.4f %8.4f %8.4f   %8.4f]", hmd_left.data[2 * 4 + 0], hmd_left.data[2 * 4 + 1], hmd_left.data[2 * 4 + 2], hmd_left.data[2 * 4 + 3]);
		DEBUG_LOG(VR, "O {%8.4f %8.4f %8.4f   %8.4f}", hmd_left.data[3 * 4 + 0], hmd_left.data[3 * 4 + 1], hmd_left.data[3 * 4 + 2], hmd_left.data[3 * 4 + 3]);
		// green = Game's suggestion
		INFO_LOG(VR, "G [%8.4f %8.4f %8.4f   %8.4f]", proj_left.data[0 * 4 + 0], proj_left.data[0 * 4 + 1], proj_left.data[0 * 4 + 2], proj_left.data[0 * 4 + 3]);
		INFO_LOG(VR, "G [%8.4f %8.4f %8.4f   %8.4f]", proj_left.data[1 * 4 + 0], proj_left.data[1 * 4 + 1], proj_left.data[1 * 4 + 2], proj_left.data[1 * 4 + 3]);
		INFO_LOG(VR, "G [%8.4f %8.4f %8.4f   %8.4f]", proj_left.data[2 * 4 + 0], proj_left.data[2 * 4 + 1], proj_left.data[2 * 4 + 2], proj_left.data[2 * 4 + 3]);
		INFO_LOG(VR, "G {%8.4f %8.4f %8.4f   %8.4f}", proj_left.data[3 * 4 + 0], proj_left.data[3 * 4 + 1], proj_left.data[3 * 4 + 2], proj_left.data[3 * 4 + 3]);
	}
	// red = my combination
	proj_left.xx = hmd_left.xx * SignOf(proj_left.xx) * fLeftWidthHack; // h fov
	proj_left.yy = hmd_left.yy * SignOf(proj_left.yy) * fLeftHeightHack; // v fov
	proj_left.zx = hmd_left.zx * SignOf(proj_left.xx) - fRightHack; // h off-axis
	proj_left.zy = hmd_left.zy * SignOf(proj_left.yy) - fUpHack; // v off-axis
	proj_right.xx = hmd_right.xx * SignOf(proj_right.xx) * fLeftWidthHack; // h fov
	proj_right.yy = hmd_right.yy * SignOf(proj_right.yy) * fLeftHeightHack; // v fov
	proj_right.zx = hmd_right.zx * SignOf(proj_right.xx) - fRightHack; // h off-axis
	proj_right.zy = hmd_right.zy * SignOf(proj_right.yy) - fUpHack; // v off-axis

	//if (g_ActiveConfig.backend_info.bSupportsGeometryShaders)
	{
		proj_left.zx = 0;
	}

	if (debug_newScene)
	{
		DEBUG_LOG(VR, "VR [%8.4f %8.4f %8.4f   %8.4f]", proj_left.data[0 * 4 + 0], proj_left.data[0 * 4 + 1], proj_left.data[0 * 4 + 2], proj_left.data[0 * 4 + 3]);
		DEBUG_LOG(VR, "VR [%8.4f %8.4f %8.4f   %8.4f]", proj_left.data[1 * 4 + 0], proj_left.data[1 * 4 + 1], proj_left.data[1 * 4 + 2], proj_left.data[1 * 4 + 3]);
		DEBUG_LOG(VR, "VR [%8.4f %8.4f %8.4f   %8.4f]", proj_left.data[2 * 4 + 0], proj_left.data[2 * 4 + 1], proj_left.data[2 * 4 + 2], proj_left.data[2 * 4 + 3]);
		DEBUG_LOG(VR, "VR {%8.4f %8.4f %8.4f   %8.4f}", proj_left.data[3 * 4 + 0], proj_left.data[3 * 4 + 1], proj_left.data[3 * 4 + 2], proj_left.data[3 * 4 + 3]);
	}

	//VR Headtracking and leaning back compensation
	Matrix44 rotation_matrix;
	Matrix44 lean_back_matrix;
	Matrix44 camera_pitch_matrix;
	if (bStuckToHead || !isPerspective)
	{
		Matrix44::LoadIdentity(rotation_matrix);
		Matrix44::LoadIdentity(lean_back_matrix);
		Matrix44::LoadIdentity(camera_pitch_matrix);
	}
	else
	{
		// head tracking
		if (g_Config.bOrientationTracking)
		{
			UpdateHeadTrackingIfNeeded();
			rotation_matrix = g_head_tracking_matrix.transpose();
		}
		else
		{
			rotation_matrix.setIdentity();
		}

		// leaning back
		float extra_pitch = -g_Config.fLeanBackAngle;
		lean_back_matrix.setRotationX(-DEGREES_TO_RADIANS(extra_pitch));
		// camera pitch
		if ((g_Config.bStabilizePitch || g_Config.bStabilizeRoll || g_Config.bStabilizeYaw) && g_Config.bCanReadCameraAngles && (g_Config.iMotionSicknessSkybox != 2 || !g_is_skybox))
		{
			if (!g_Config.bStabilizePitch)
			{
				Matrix44 user_pitch44;
				Matrix44 roll_and_yaw_matrix;

				if (isPerspective || vr_widest_3d_HFOV > 0)
					extra_pitch = g_Config.fCameraPitch;
				else
					extra_pitch = g_Config.fScreenPitch;
				user_pitch44.setRotationX(-DEGREES_TO_RADIANS(extra_pitch));
				Matrix44::Set(roll_and_yaw_matrix, g_game_camera_rotmat.data);
				camera_pitch_matrix = roll_and_yaw_matrix * user_pitch44; // or vice versa?
			}
			else
			{
				Matrix44::Set(camera_pitch_matrix, g_game_camera_rotmat.data);
			}
		}
		else
		{
			if (isPerspective || vr_widest_3d_HFOV > 0)
				extra_pitch = g_Config.fCameraPitch;
			else
				extra_pitch = g_Config.fScreenPitch;
			camera_pitch_matrix.setRotationX(-DEGREES_TO_RADIANS(extra_pitch));
		}
	}

	//VR sometimes yaw needs to be inverted for games that use a flipped x axis
	// (ActionGirlz even uses flipped matrices and non-flipped matrices in the same frame)
	if (isPerspective)
	{
		if (flippedMatrix.xx<0)
		{
			if (debug_newScene)
				INFO_LOG(VR, "flipped X");
			// flip all the x axis values, except x squared (data[0])
			//Needed for Action Girlz Racing, Backyard Baseball
			//rotation_matrix.data[1] *= -1;
			//rotation_matrix.data[2] *= -1;
			//rotation_matrix.data[3] *= -1;
			//rotation_matrix.data[4] *= -1;
			//rotation_matrix.data[8] *= -1;
			//rotation_matrix.data[12] *= -1;
			flipped_x = -1;
		}
		if (flippedMatrix.yy<0)
		{
			if (debug_newScene)
				INFO_LOG(VR, "flipped Y");
			// flip all the y axis values, except y squared (data[5])
			// Needed for ABBA
			//rotation_matrix.data[1] *= -1;
			//rotation_matrix.data[4] *= -1;
			//rotation_matrix.data[6] *= -1;
			//rotation_matrix.data[7] *= -1;
			//rotation_matrix.data[9] *= -1;
			//rotation_matrix.data[13] *= -1;
			flipped_y = -1;
		}
	}

	// Position matrices
	Matrix44 head_position_matrix, free_look_matrix, camera_forward_matrix, camera_position_matrix;
	if (bStuckToHead || g_is_skybox)
	{
		Matrix44::LoadIdentity(head_position_matrix);
		Matrix44::LoadIdentity(free_look_matrix);
		Matrix44::LoadIdentity(camera_position_matrix);
	}
	else
	{
		Vec3 pos;
		// head tracking
		if (g_Config.bPositionTracking)
		{
			for (int i = 0; i < 3; ++i)
				pos[i] = g_head_tracking_position[i] * UnitsPerMetre;
			head_position_matrix.setTranslation(pos);
		}
		else
		{
			head_position_matrix.setIdentity();
		}

		// freelook camera position
		for (int i = 0; i < 3; ++i)
			pos[i] = s_fViewTranslationVector[i] * UnitsPerMetre;
		free_look_matrix.setTranslation(pos);

		// camera position stabilisation
		if (g_Config.bStabilizeX || g_Config.bStabilizeY || g_Config.bStabilizeZ)
		{
			for (int i = 0; i < 3; ++i)
				pos[i] = -g_game_camera_pos[i] * UnitsPerMetre;
			camera_position_matrix.setTranslation(pos);
		}
		else
		{
			camera_position_matrix.setIdentity();
		}
	}

	Matrix44 look_matrix;
	if (isPerspective && g_viewport_type != VIEW_HUD_ELEMENT && g_viewport_type != VIEW_OFFSCREEN)
	{
		// Transformations must be applied in the following order for VR:
		// camera position stabilisation
		// camera forward
		// camera pitch
		// free look
		// leaning back
		// head position tracking
		// head rotation tracking
		if (bNoForward || g_is_skybox || bStuckToHead)
		{
			camera_forward_matrix.setIdentity();
		}
		else
		{
			Vec3 pos;
			pos[0] = 0;
			pos[1] = 0;
			pos[2] = (g_Config.fCameraForward + zoom_forward) * UnitsPerMetre;
			camera_forward_matrix.setTranslation(pos);
		}

		look_matrix = camera_forward_matrix * camera_position_matrix * camera_pitch_matrix * free_look_matrix * lean_back_matrix * head_position_matrix * rotation_matrix;
	}
	else
		//if (xfmem.projection.type != GX_PERSPECTIVE || g_viewport_type == VIEW_HUD_ELEMENT || g_viewport_type == VIEW_OFFSCREEN)
	{
		if (debug_newScene)
			INFO_LOG(VR, "2D: hacky test");

		float HudWidth, HudHeight, HudThickness, HudDistance, HudUp, CameraForward, AimDistance;

		proj_left[15] = 0.0f;
		proj_right[15] = 0.0f;

		// 2D Screen
		if (vr_widest_3d_HFOV <= 0)
		{
			HudThickness = g_Config.fScreenThickness * UnitsPerMetre;
			HudDistance = g_Config.fScreenDistance * UnitsPerMetre;
			HudHeight = g_Config.fScreenHeight * UnitsPerMetre;
			HudHeight = g_Config.fScreenHeight * UnitsPerMetre;
			HudWidth = HudHeight * (float)16 / 9;
			CameraForward = 0;
			HudUp = g_Config.fScreenUp * UnitsPerMetre;
			AimDistance = HudDistance;
		}
		else
			// HUD over 3D world
		{
			// Give the 2D layer a 3D effect if different parts of the 2D layer are rendered at different z coordinates
			HudThickness = g_Config.fHudThickness * UnitsPerMetre;  // the 2D layer is actually a 3D box this many game units thick
			HudDistance = g_Config.fHudDistance * UnitsPerMetre;   // depth 0 on the HUD should be this far away
			HudUp = 0;
			if (bNoForward)
				CameraForward = 0;
			else
				CameraForward = (g_Config.fCameraForward + zoom_forward) * UnitsPerMetre;
			// When moving the camera forward, correct the size of the HUD so that aiming is correct at AimDistance
			AimDistance = g_Config.fAimDistance * UnitsPerMetre;
			if (AimDistance <= 0)
				AimDistance = HudDistance;
			// Now that we know how far away the box is, and what FOV it should fill, we can work out the width and height in game units
			// Note: the HUD won't line up exactly (except at AimDistance) if CameraForward is non-zero 
			//float HudWidth = 2.0f * tanf(hfov / 2.0f * 3.14159f / 180.0f) * (HudDistance) * Correction;
			//float HudHeight = 2.0f * tanf(vfov / 2.0f * 3.14159f / 180.0f) * (HudDistance) * Correction;
			HudWidth = 2.0f * tanf(DEGREES_TO_RADIANS(hfov / 2.0f)) * HudDistance * (AimDistance + CameraForward) / AimDistance;
			HudHeight = 2.0f * tanf(DEGREES_TO_RADIANS(vfov / 2.0f)) * HudDistance * (AimDistance + CameraForward) / AimDistance;
		}

		Vec3 scale; // width, height, and depth of box in game units divided by 2D width, height, and depth 
		Vec3 position; // position of front of box relative to the camera, in game units 

		float viewport_scale[2];
		float viewport_offset[2]; // offset as a fraction of the viewport's width
		//if (g_viewport_type != VIEW_HUD_ELEMENT && g_viewport_type != VIEW_OFFSCREEN)
		{
			viewport_scale[0] = 1.0f;
			viewport_scale[1] = 1.0f;
			viewport_offset[0] = 0.0f;
			viewport_offset[1] = 0.0f;
		}
		//else
		//{
		//	Viewport &v = xfmem.viewport;
		//	float left, top, width, height;
		//	left = v.xOrig - v.wd - 342;
		//	top = v.yOrig + v.ht - 342;
		//	width = 2 * v.wd;
		//	height = -2 * v.ht;
		//	float screen_width = (float)g_final_screen_region.GetWidth();
		//	float screen_height = (float)g_final_screen_region.GetHeight();
		//	viewport_scale[0] = width / screen_width;
		//	viewport_scale[1] = height / screen_height;
		//	viewport_offset[0] = ((left + (width / 2)) - (0 + (screen_width / 2))) / screen_width;
		//	viewport_offset[1] = -((top + (height / 2)) - (0 + (screen_height / 2))) / screen_height;
		//}

		// 3D HUD elements (may be part of 2D screen or HUD)
		if (isPerspective)
		{
			// these are the edges of the near clipping plane in game coordinates
			float left2D = -(flippedMatrix.zx + 1) / flippedMatrix.xx;
			float right2D = left2D + 2 / flippedMatrix.xx;
			float bottom2D = -(flippedMatrix.zy + 1) / flippedMatrix.yy;
			float top2D = bottom2D + 2 / flippedMatrix.yy;
			float zFar2D = flippedMatrix.wz / flippedMatrix.zz;
			float zNear2D = zFar2D*flippedMatrix.zz / (flippedMatrix.zz - 1);
			float zObj = zNear2D + (zFar2D - zNear2D) * g_Config.fHud3DCloser;

			left2D *= zObj;
			right2D *= zObj;
			bottom2D *= zObj;
			top2D *= zObj;

			// Scale the width and height to fit the HUD in metres
			if (flippedMatrix.xx == 0 || right2D == left2D) {
				scale[0] = 0;
			}
			else {
				scale[0] = viewport_scale[0] * HudWidth / (right2D - left2D);
			}
			if (flippedMatrix.yy == 0 || top2D == bottom2D) {
				scale[1] = 0;
			}
			else {
				scale[1] = viewport_scale[1] * HudHeight / (top2D - bottom2D); // note that positive means up in 3D
			}
			// Keep depth the same scale as width, so it looks like a real object
			if (flippedMatrix.zz == 0 || zFar2D == zNear2D) {
				scale[2] = scale[0];
			}
			else {
				scale[2] = scale[0];
			}
			// Adjust the position for off-axis projection matrices, and shifting the 2D screen
			position[0] = scale[0] * (-(right2D + left2D) / 2.0f) + viewport_offset[0] * HudWidth; // shift it right into the centre of the view
			position[1] = scale[1] * (-(top2D + bottom2D) / 2.0f) + viewport_offset[1] * HudHeight + HudUp; // shift it up into the centre of the view;
			// Shift it from the old near clipping plane to the HUD distance, and shift the camera forward
			if (vr_widest_3d_HFOV <= 0)
				position[2] = scale[2] * zObj - HudDistance;
			else
				position[2] = scale[2] * zObj - HudDistance; // - CameraForward;
		}
		// 2D layer, or 2D viewport (may be part of 2D screen or HUD)
		else
		{
			float left2D = -(flippedMatrix.wx + 1) / flippedMatrix.xx;
			float right2D = left2D + 2 / flippedMatrix.xx;
			float bottom2D = -(flippedMatrix.wy + 1) / flippedMatrix.yy;
			float top2D = bottom2D + 2 / flippedMatrix.yy;
			float zFar2D, zNear2D;
			zFar2D = flippedMatrix.wz / flippedMatrix.zz;
			zNear2D = (1 + flippedMatrix.zz * zFar2D) / flippedMatrix.zz;

			// for 2D, work out the fraction of the HUD we should fill, and multiply the scale by that
			// also work out what fraction of the height we should shift it up, and what fraction of the width we should shift it left
			// only multiply by the extra scale after adjusting the position?

			if (flippedMatrix.xx == 0 || right2D == left2D) {
				scale[0] = 0;
			}
			else {
				scale[0] = viewport_scale[0] * HudWidth / (right2D - left2D);
			}
			if (flippedMatrix.yy == 0 || top2D == bottom2D) {
				scale[1] = 0;
			}
			else {
				scale[1] = viewport_scale[1] * HudHeight / (top2D - bottom2D); // note that positive means up in 3D
			}
			if (flippedMatrix.zz == 0 || zFar2D == zNear2D) {
				scale[2] = 0; // The 2D layer was flat, so we make it flat instead of a box to avoid dividing by zero
			}
			else {
				scale[2] = HudThickness / (zFar2D - zNear2D); // Scale 2D z values into 3D game units so it is the right thickness
			}
			position[0] = scale[0] * (-(right2D + left2D) / 2.0f) + viewport_offset[0] * HudWidth; // shift it right into the centre of the view
			position[1] = scale[1] * (-(top2D + bottom2D) / 2.0f) + viewport_offset[1] * HudHeight + HudUp; // shift it up into the centre of the view;
			// Shift it from the zero plane to the HUD distance, and shift the camera forward
			if (vr_widest_3d_HFOV <= 0)
				position[2] = -HudDistance;
			else
				position[2] = -HudDistance; // - CameraForward;
		}

		Matrix44 A, B, scale_matrix, position_matrix, box_matrix;
		scale_matrix.setScaling(scale);
		position_matrix.setTranslation(position);

		// order: scale, position
		look_matrix = scale_matrix * position_matrix * camera_position_matrix * camera_pitch_matrix * free_look_matrix * lean_back_matrix * head_position_matrix * rotation_matrix;

	}

	Matrix44 eye_pos_matrix_left, eye_pos_matrix_right;
	float posLeft[3] = { 0, 0, 0 };
	float posRight[3] = { 0, 0, 0 };
	if (!g_is_skybox)
	{
		VR_GetEyePos(posLeft, posRight);
		for (int i = 0; i < 3; ++i)
		{
			posLeft[i] *= UnitsPerMetre;
			posRight[i] *= UnitsPerMetre;
		}
	}

	Matrix44 view_matrix_left, view_matrix_right;
	//if (g_Config.backend_info.bSupportsGeometryShaders)
	{
		Matrix44::Set(view_matrix_left, look_matrix.data);
		Matrix44::Set(view_matrix_right, view_matrix_left.data);
	}
	//else
	//{
	//	Matrix44::Translate(eye_pos_matrix_left, posLeft);
	//	Matrix44::Translate(eye_pos_matrix_right, posRight);
	//	Matrix44::Multiply(eye_pos_matrix_left, look_matrix, view_matrix_left);
	//	Matrix44::Multiply(eye_pos_matrix_right, look_matrix, view_matrix_right);
	//}
	Matrix44 final_matrix_left, final_matrix_right;
	//Matrix44::Multiply(proj_left, view_matrix_left, final_matrix_left);
	//Matrix44::Multiply(proj_right, view_matrix_right, final_matrix_right);
	final_matrix_left = view_matrix_left * proj_left;

	if (flipped_x < 0)
	{
		// flip all the x axis values, except x squared (data[0])
		//Needed for Action Girlz Racing, Backyard Baseball
		final_matrix_left.data[1] *= -1;
		final_matrix_left.data[2] *= -1;
		final_matrix_left.data[3] *= -1;
		//GeometryShaderManager::constants.stereoparams[2] *= -1;
		final_matrix_left.data[4] *= -1;
		final_matrix_left.data[8] *= -1;
		final_matrix_left.data[12] *= -1;
		final_matrix_right.data[1] *= -1;
		final_matrix_right.data[2] *= -1;
		final_matrix_right.data[3] *= -1;
		//GeometryShaderManager::constants.stereoparams[3] *= -1;
		final_matrix_right.data[4] *= -1;
		final_matrix_right.data[8] *= -1;
		final_matrix_right.data[12] *= -1;
		//GeometryShaderManager::constants.stereoparams[0] *= -1;
		//GeometryShaderManager::constants.stereoparams[1] *= -1;
	}
	if (flipped_y < 0)
	{
		final_matrix_left.data[1] *= -1;
		final_matrix_left.data[4] *= -1;
		final_matrix_left.data[6] *= -1;
		final_matrix_left.data[7] *= -1;
		final_matrix_left.data[9] *= -1;
		final_matrix_left.data[13] *= -1;
		final_matrix_right.data[1] *= -1;
		final_matrix_right.data[4] *= -1;
		final_matrix_right.data[6] *= -1;
		final_matrix_right.data[7] *= -1;
		final_matrix_right.data[9] *= -1;
		final_matrix_right.data[13] *= -1;
	}

	//Matrix4x4 final_matrix = rotation_matrix * proj_left;

	ScaleProjMatrix(final_matrix_left);

	glUniformMatrix4fv(u_proj, 1, GL_FALSE, final_matrix_left.m);
}

ShaderManager::ShaderManager() : lastShader_(NULL), globalDirty_(0xFFFFFFFF), shaderSwitchDirty_(0) {
	codeBuffer_ = new char[16384];
}

ShaderManager::~ShaderManager() {
	delete [] codeBuffer_;
}

void ShaderManager::Clear() {
	DirtyLastShader();
	for (auto iter = linkedShaderCache_.begin(); iter != linkedShaderCache_.end(); ++iter) {
		delete iter->ls;
	}
	for (auto iter = fsCache_.begin(); iter != fsCache_.end(); ++iter)	{
		delete iter->second;
	}
	for (auto iter = vsCache_.begin(); iter != vsCache_.end(); ++iter)	{
		delete iter->second;
	}
	linkedShaderCache_.clear();
	fsCache_.clear();
	vsCache_.clear();
	globalDirty_ = 0xFFFFFFFF;
	lastFSID_.clear();
	lastVSID_.clear();
	DirtyShader();
	bFrameChanged = true;
}

void ShaderManager::ClearCache(bool deleteThem) {
	Clear();
}

void ShaderManager::DirtyShader() {
	// Forget the last shader ID
	lastFSID_.clear();
	lastVSID_.clear();
	DirtyLastShader();
	globalDirty_ = 0xFFFFFFFF;
	shaderSwitchDirty_ = 0;
}

void ShaderManager::DirtyLastShader() { // disables vertex arrays
	if (lastShader_)
		lastShader_->stop();
	lastShader_ = 0;
	lastVShaderSame_ = false;
}

// This is to be used when debugging why incompatible shaders are being linked, like is
// happening as I write this in Tactics Ogre
bool ShaderManager::DebugAreShadersCompatibleForLinking(Shader *vs, Shader *fs) {
	// Check clear mode flag just for starters.
	ShaderID vsid = vs->ID();
	ShaderID fsid = fs->ID();

	// TODO: Make the flag fields more similar?
	// Check DoTexture
	if (((vsid.d[0] >> 4) & 1) != ((fsid.d[0] >> 1) & 1)) {
		ERROR_LOG(G3D, "Texture enable flag mismatch!");
		return false;
	}

	return true;
}

Shader *ShaderManager::ApplyVertexShader(int prim, u32 vertType) {
	// This doesn't work - we miss some events that really do need to dirty the prescale.
	// like changing the texmapmode.
	// if (g_Config.bPrescaleUV)
	//	 globalDirty_ &= ~DIRTY_UVSCALEOFFSET;

	if (globalDirty_) {
		if (lastShader_)
			lastShader_->dirtyUniforms |= globalDirty_;
		shaderSwitchDirty_ |= globalDirty_;
		globalDirty_ = 0;
	}

	bool useHWTransform = CanUseHardwareTransform(prim);

	ShaderID VSID;
	ComputeVertexShaderID(&VSID, vertType, useHWTransform);

	// Just update uniforms if this is the same shader as last time.
	if (lastShader_ != 0 && VSID == lastVSID_) {
		lastVShaderSame_ = true;
		return lastShader_->vs_;  	// Already all set.
	} else {
		lastVShaderSame_ = false;
	}

	lastVSID_ = VSID;

	VSCache::iterator vsIter = vsCache_.find(VSID);
	Shader *vs;
	if (vsIter == vsCache_.end())	{
		// Vertex shader not in cache. Let's compile it.
		GenerateVertexShader(prim, vertType, codeBuffer_, useHWTransform);
		vs = new Shader(codeBuffer_, GL_VERTEX_SHADER, useHWTransform, VSID);

		if (vs->Failed()) {
			I18NCategory *gr = GetI18NCategory("Graphics");
			ERROR_LOG(G3D, "Shader compilation failed, falling back to software transform");
			osm.Show(gr->T("hardware transform error - falling back to software"), 2.5f, 0xFF3030FF, -1, true);
			delete vs;

			// TODO: Look for existing shader with the appropriate ID, use that instead of generating a new one - however, need to make sure
			// that that shader ID is not used when computing the linked shader ID below, because then IDs won't match
			// next time and we'll do this over and over...

			// Can still work with software transform.
			GenerateVertexShader(prim, vertType, codeBuffer_, false);
			vs = new Shader(codeBuffer_, GL_VERTEX_SHADER, false, VSID);
		}

		vsCache_[VSID] = vs;
	} else {
		vs = vsIter->second;
	}
	return vs;
}

LinkedShader *ShaderManager::ApplyFragmentShader(Shader *vs, int prim, u32 vertType) {
	ShaderID FSID;
	ComputeFragmentShaderID(&FSID);
	if (lastVShaderSame_ && FSID == lastFSID_) {
		lastShader_->UpdateUniforms(vertType);
		return lastShader_;
	}

	lastFSID_ = FSID;

	FSCache::iterator fsIter = fsCache_.find(FSID);
	Shader *fs;
	if (fsIter == fsCache_.end())	{
		// Fragment shader not in cache. Let's compile it.
		GenerateFragmentShader(codeBuffer_);
		fs = new Shader(codeBuffer_, GL_FRAGMENT_SHADER, vs->UseHWTransform(), FSID);
		fsCache_[FSID] = fs;
	} else {
		fs = fsIter->second;
	}

	// Okay, we have both shaders. Let's see if there's a linked one.
	LinkedShader *ls = NULL;

	u32 switchDirty = shaderSwitchDirty_;
	for (auto iter = linkedShaderCache_.begin(); iter != linkedShaderCache_.end(); ++iter) {
		// Deferred dirtying! Let's see if we can make this even more clever later.
		iter->ls->dirtyUniforms |= switchDirty;

		if (iter->vs == vs && iter->fs == fs) {
			ls = iter->ls;
		}
	}
	shaderSwitchDirty_ = 0;

	if (ls == NULL) {
		// Check if we can link these.
#ifdef _DEBUG
		if (!DebugAreShadersCompatibleForLinking(vs, fs)) {
			return NULL;
		}
#endif
		ls = new LinkedShader(vs, fs, vertType, vs->UseHWTransform(), lastShader_);  // This does "use" automatically
		const LinkedShaderCacheEntry entry(vs, fs, ls);
		linkedShaderCache_.push_back(entry);
	} else {
		ls->use(vertType, lastShader_);
	}

	lastShader_ = ls;
	return ls;
}
