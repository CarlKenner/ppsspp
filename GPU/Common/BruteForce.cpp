// Copyright (c) 2015- PPSSPP Project.

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

#include <string>

#ifdef _WIN32
#include "Windows/W32Util/Misc.h"
#endif

#include "Common/Common.h"
#include "Core/Config.h"
#include "Core/CoreParameter.h"
#include "Core/Debugger/SymbolMap.h"
#include "Core/MemMap.h"
#include "Core/MIPS/JitCommon/NativeJit.h"
#include "Core/SaveState.h"
#include "Core/System.h"
#include "GPU/Common/BruteForce.h"
#include "GPU/Common/VR.h"
#include "GPU/GPU.h"

extern bool bFreeLookChanged;

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
	PSP_CoreParameter().fpsLimit = 1;
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
	PSP_CoreParameter().fpsLimit = 0;
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