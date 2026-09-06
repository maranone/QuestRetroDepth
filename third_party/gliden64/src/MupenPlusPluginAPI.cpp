#include "PluginAPI.h"
#include "Types.h"
#include "mupenplus/GLideN64_mupenplus.h"
#include "N64.h"

#ifdef __ANDROID__
#include <android/log.h>
#define QRD_G64_API_LOG(...) __android_log_print(ANDROID_LOG_INFO, "GLideN64Hook", __VA_ARGS__)
#else
#define QRD_G64_API_LOG(...) ((void)0)
#endif

extern "C" {

EXPORT int CALL RomOpen(void)
{
	QRD_G64_API_LOG("RomOpen entry");
	if (rdram_size != nullptr)
		RDRAMSize = *rdram_size - 1;
	else
		RDRAMSize = 0;

	int result = api().RomOpen();
	QRD_G64_API_LOG("RomOpen result=%d", result);
	return result;
}

EXPORT m64p_error CALL PluginGetVersion(
	m64p_plugin_type * _PluginType,
	int * _PluginVersion,
	int * _APIVersion,
	const char ** _PluginNamePtr,
	int * _Capabilities
)
{
	return api().PluginGetVersion(_PluginType, _PluginVersion, _APIVersion, _PluginNamePtr, _Capabilities);
}

EXPORT m64p_error CALL PluginStartup(
	m64p_dynlib_handle CoreLibHandle,
	void *Context,
	void (*DebugCallback)(void *, int, const char *)
)
{
	return api().PluginStartup(CoreLibHandle, Context, DebugCallback);
}

#ifdef M64P_GLIDENUI
EXPORT m64p_error CALL PluginConfig(void* parent)
{
	return api().PluginConfig(parent);
}
#endif // M64P_GLIDENUI

EXPORT m64p_error CALL PluginShutdown(void)
{
	return api().PluginShutdown();
}

EXPORT void CALL ReadScreen2(void *dest, int *width, int *height, int front)
{
	api().ReadScreen2(dest, width, height, front);
}

EXPORT void CALL SetRenderingCallback(void (*callback)(int))
{
	api().SetRenderingCallback(callback);
}

EXPORT void CALL ResizeVideoOutput(int width, int height)
{
	api().ResizeVideoOutput(width, height);
}

} // extern "C"
