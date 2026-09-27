/*
 * main.cpp, part of the OmniAI VCMI plugin
 *
 * License: GNU General Public License v2.0 or later
 *
 * VCMI dynamic-loader entry points. The engine calls GetGlobalAiVersion to
 * check ABI compatibility, GetAiName for display, and GetNewAI to obtain the
 * adventure-map agent instance.
 */
#include "StdInc.h"

#include "OmniAI.h"

#ifdef __GNUC__
#define strcpy_s(a, b, c) strncpy(a, c, b)
#endif

static const char * const g_cszAiName = "OmniAI 0.1";

extern "C" DLL_EXPORT int GetGlobalAiVersion()
{
	return AI_INTERFACE_VER;
}

extern "C" DLL_EXPORT void GetAiName(char* name)
{
	strcpy_s(name, strlen(g_cszAiName) + 1, g_cszAiName);
}

extern "C" DLL_EXPORT void GetNewAI(std::shared_ptr<CGlobalAI> &out)
{
	out = std::make_shared<OmniAI>();
}
