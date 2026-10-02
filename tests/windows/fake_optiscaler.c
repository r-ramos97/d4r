/* Stands in for OptiScaler.dll installed as dxgi.dll: it imports version.dll, and its DllMain records whether an
   nvapi64.dll was already loaded, which is what OptiScaler 0.9.4's NVIDIA check looks at. */
#include <windows.h>

static int nvapi_loaded_at_start = -1;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance, (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        nvapi_loaded_at_start = GetModuleHandleW(L"nvapi64.dll") != NULL;
    return TRUE;
}

__declspec(dllexport) int fake_optiscaler_saw_nvapi(void)
{
    return nvapi_loaded_at_start;
}

/* a real version.dll call, so the import exists and goes through whichever version.dll was loaded */
__declspec(dllexport) DWORD fake_optiscaler_version_size(const wchar_t* path)
{
    DWORD handle = 0;
    return GetFileVersionInfoSizeW(path, &handle);
}
