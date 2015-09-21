// Copyright 2015 PPSSPP Project.
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <cmath>
#include <cstdio>
#include <locale.h>

#include "base/logging.h"
#include "base/stringutil.h"
#include "Core/Config.h"
#include "Core/Reporting.h"
#include "gfx_es2/gpu_features.h"
#include "GPU/ge_constants.h"
#include "GPU/GPUState.h"
#include "GPU/Common/GPUStateUtils.h"
#include "GPU/Common/VertexDecoderCommon.h"
#include "GPU/Common/VR.h"
#include "GPU/GLES/FragmentShaderGenerator.h"
#include "GPU/GLES/Framebuffer.h"
#include "GPU/GLES/ShaderManager.h"
#include "GPU/GLES/GeometryShaderGenerator.h"
#include "GPU/GLES/VertexShaderGenerator.h"
#include "GPU/ge_constants.h"
#include "GPU/GPUState.h"
#if defined(_WIN32) && defined(_DEBUG)
#include "Common/CommonWindows.h"
#endif

#undef WRITE

#define WRITE p+=sprintf

static char text[16384];

static const char* primitives_ogl[] =
{
	"points",
	"lines",
	"triangles"
};

static const char* primitives_ogl_out[] =
{
	"points",
	"line_strip",
	"triangle_strip"
};

int PrimToPrimitiveType(int prim) {
	switch (prim)
	{
	case GE_PRIM_POINTS:
		return PRIMITIVE_POINTS;
	case GE_PRIM_LINES:
	case GE_PRIM_LINE_STRIP:
		return PRIMITIVE_LINES;
	case GE_PRIM_TRIANGLES:
	case GE_PRIM_TRIANGLE_STRIP:
	case GE_PRIM_TRIANGLE_FAN:
		return PRIMITIVE_TRIANGLES;
	case GE_PRIM_RECTANGLES:
	default:
		return PRIMITIVE_TRIANGLES;
	}
}

//u32 stereo : 1;
//u32 numTexGens : 4;
//u32 pixel_lighting : 1;
//u32 primitive_type : 2;
//u32 wireframe : 1;
//u32 vr : 1;
void ComputeGeometryShaderID(ShaderID *id, int primitive_type) {

	int id0 = 0;
	int id1 = 0;

	//bool stereo = true;
	//u8 numTexGens = 0; //?
	//bool pixel_lighting = false; //?
	//bool wireframe = false;
	//bool vr = g_has_hmd && g_Config.bEnableVR;
	//
	//id0 = stereo & 1;
	//id0 |= (numTexGens & 15) << 1;
	//id0 |= (pixel_lighting & 1) << 5;
	//id0 |= (primitive_type & 3) << 6;
	//id0 |= (wireframe & 1) << 8;
	//id0 |= (vr & 1) << 9;

	bool doTexture = gstate.isTextureMapEnabled() && !gstate.isModeClear();
	bool doTextureProjection = gstate.getUVGenMode() == GE_TEXMAP_TEXTURE_MATRIX;
	bool doFlatShading = gstate.getShadeMode() == GE_SHADE_FLAT && !gstate.isModeClear();
	bool enableFog = gstate.isFogEnabled() && !gstate.isModeThrough() && !gstate.isModeClear();
	bool lmode = gstate.isUsingSecondaryColor() && gstate.isLightingEnabled();

	id0 = lmode & 1;
	id0 |= (enableFog & 1) << 1;
	if (doTexture) {
		id0 |= 1 << 2;
		id0 |= (doTextureProjection & 1) << 3;
	}
	id0 |= (doFlatShading & 1) << 4;

	id->d[0] = id0;
	id->d[1] = id1;
}

void GenerateGeometryShader(int prim, char *buffer, bool useHWTransform) {
	char *p = buffer;

	int primitive_type = PrimToPrimitiveType(prim);

	// Geometry shaders require at least this version. But if we want instancing, we need version 400.
	WRITE(p, "#version 150\n");

	// We remove these everywhere - GL4, GL3, Mac-forced-GL2, etc.
	WRITE(p, "#define lowp\n");
	WRITE(p, "#define mediump\n");
	WRITE(p, "#define highp\n");

	const unsigned int vertex_in = primitive_type + 1;
	unsigned int vertex_out = vertex_in;
	WRITE(p, "layout(%s) in;\n", primitives_ogl[primitive_type]);
	WRITE(p, "layout(%s, max_vertices = %d) out;\n", primitives_ogl_out[primitive_type], vertex_out);

	bool lmode = gstate.isUsingSecondaryColor() && gstate.isLightingEnabled() && !gstate.isModeThrough();
	bool doTexture = gstate.isTextureMapEnabled() && !gstate.isModeClear();
	bool enableFog = gstate.isFogEnabled() && !gstate.isModeThrough() && !gstate.isModeClear();

	WRITE(p, "%s VertexData {\n", "in");
	GenerateVSOutputMembers(p);
	WRITE(p, "} vs[%d];\n", vertex_in);

	WRITE(p, "%s VertexData {\n", "out");
	GenerateVSOutputMembers(p);
	WRITE(p, "} ps;\n");

	WRITE(p, "void main() {\n");
	for (unsigned i = 0; i < vertex_in; ++i)
	{
		WRITE(p, "	gl_Position = gl_in[%d].gl_Position;\n", i);
		WRITE(p, "	ps.v_color0 = vs[%d].v_color0;\n", i);
		if (lmode)
			WRITE(p, "	ps.v_color1 = vs[%d].v_color1;\n", i);
		if (doTexture)
			WRITE(p, "	ps.v_texcoord = vs[%d].v_texcoord;\n", i);
		if (enableFog)
			WRITE(p, "	ps.v_fogdepth = vs[%d].v_fogdepth;\n", i);
		WRITE(p, "	EmitVertex();\n");
	}
	WRITE(p, "	EndPrimitive();\n");
	WRITE(p, "}\n");
}