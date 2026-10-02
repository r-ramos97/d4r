/* Initialises NGX through the d4r shim of a portable install, as OptiScaler does, on native Windows (or
   Windows mode under Wine, D4R_PLATFORM=windows): the shim must read d4r\d4r.ini, hand the Windows-side
   settings to the native nvcuda bridge, and the (mock) NGX core must reach ZLUDA through the bridge.
   tests/test_windows_native.py prepares the folder.

   usage: shim_test.exe D4R_DIR */
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef unsigned int(__cdecl* InitExtFn)(unsigned long long, const wchar_t*, void*, unsigned int, const void*);
typedef void (*ReportFn)(const char**, int*, int*);

static int failures;

static void check(int condition, const char* name, const char* detail)
{
    printf("%s %s%s%s\n", condition ? "PASS" : "FAIL", name, condition ? "" : ": ", condition ? "" : detail);
    failures += !condition;
}

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\nvngx.dll", argv[1]);
    HMODULE shim = LoadLibraryA(path);
    check(shim != NULL, "shim loads", path);
    if (shim == NULL)
        return 1;
    InitExtFn init = (InitExtFn)(void*)GetProcAddress(shim, "NVSDK_NGX_D3D12_Init_Ext");
    check(init != NULL, "NVSDK_NGX_D3D12_Init_Ext exported", "");
    const unsigned int result = init(1234, L".", NULL, 0x15, NULL);
    char detail[64];
    snprintf(detail, sizeof(detail), "0x%08x", result);
    check(result == 1, "NGX initialises through the shim", detail);

    HMODULE core = GetModuleHandleA("_nvngx.dll");
    ReportFn report = core != NULL ? (ReportFn)(void*)GetProcAddress(core, "mock_ngx_report") : NULL;
    check(report != NULL, "the NGX core from d4r\\ngx is loaded", "");
    if (report == NULL)
        return 1;
    const char* cuda;
    int cuInit, module;
    report(&cuda, &cuInit, &module);
    char bridge[MAX_PATH];
    snprintf(bridge, sizeof(bridge), "%s\\nvcuda.dll", argv[1]);
    check(_stricmp(cuda, bridge) == 0, "NGX's nvcuda.dll is the d4r bridge", cuda);
    check(cuInit == 0, "cuInit reaches ZLUDA", "");
    check(module == 0, "DLSS module loads", "");
    check(GetModuleHandleA("zluda_nvcuda.dll") != NULL, "ZLUDA loaded from d4r\\zluda", "");

    char value[MAX_PATH] = {0};
    GetEnvironmentVariableA("D4R_ZLUDA_NATIVE_DIR", value, sizeof(value));
    check(strstr(value, "d4r-native") != NULL, "native kernels served from a verified per-process folder", value);
    printf("%d failure(s)\n", failures);
    return failures != 0;
}
