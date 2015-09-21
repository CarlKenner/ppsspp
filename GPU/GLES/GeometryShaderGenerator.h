// Copyright 2014 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "Globals.h"

struct ShaderID;

enum PrimitiveType {
	PRIMITIVE_POINTS,
	PRIMITIVE_LINES,
	PRIMITIVE_TRIANGLES,
};
void ComputeGeometryShaderID(ShaderID *id, int prim);
void GenerateGeometryShader(int prim, char *buffer, bool useHWTransform);
