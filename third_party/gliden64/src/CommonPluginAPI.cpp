#ifdef OS_WINDOWS
# include <windows.h>
#else
# include "winlnxdefs.h"
#endif // OS_WINDOWS

#include "PluginAPI.h"

#ifdef __ANDROID__
#include <android/log.h>
#define QRD_G64_API_LOG(...) __android_log_print(ANDROID_LOG_INFO, "GLideN64Hook", __VA_ARGS__)
#else
#define QRD_G64_API_LOG(...) ((void)0)
#endif

extern "C" {

EXPORT BOOL CALL InitiateGFX (GFX_INFO Gfx_Info)
{
	return api().InitiateGFX(Gfx_Info);
}

EXPORT void CALL MoveScreen (int xpos, int ypos)
{
	api().MoveScreen(xpos, ypos);
}

EXPORT void CALL ProcessDList(void)
{
	static unsigned int calls = 0;
	++calls;
	if (calls <= 12 || (calls % 500) == 0)
		QRD_G64_API_LOG("ProcessDList entry=%u", calls);
	api().ProcessDList();
}

EXPORT void CALL ProcessRDPList(void)
{
	api().ProcessRDPList();
}

EXPORT void CALL RomClosed (void)
{
	api().RomClosed();
}

EXPORT void CALL ShowCFB (void)
{
	api().ShowCFB();
}

EXPORT void CALL UpdateScreen (void)
{
	static unsigned int calls = 0;
	++calls;
	if (calls <= 12 || (calls % 120) == 0)
		QRD_G64_API_LOG("UpdateScreen entry=%u", calls);
	api().UpdateScreen();
}

EXPORT void CALL ViStatusChanged (void)
{
	api().ViStatusChanged();
}

EXPORT void CALL ViWidthChanged (void)
{
	api().ViWidthChanged();
}

EXPORT void CALL ChangeWindow(void)
{
	api().ChangeWindow();
}

EXPORT void CALL FBWrite(unsigned int addr, unsigned int size)
{
	api().FBWrite(addr, size);
}

EXPORT void CALL FBRead(unsigned int addr)
{
	api().FBRead(addr);
}

EXPORT void CALL FBGetFrameBufferInfo(void *pinfo)
{
	api().FBGetFrameBufferInfo(pinfo);
}

#ifndef MUPENPLUSAPI
EXPORT void CALL FBWList(FrameBufferModifyEntry *plist, unsigned int size)
{
	api().FBWList(plist, size);
}
#endif
}
