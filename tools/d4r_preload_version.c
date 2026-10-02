/* version.dll for native Windows installs: loads d4r's nvapi64.dll before OptiScaler starts.
 *
 * OptiScaler 0.9.4 enables its DLSS backend only when it finds NVIDIA's NVAPI while it starts (in its DllMain): an
 * nvapi64.dll already loaded in the process, else System32's. An AMD PC has neither, so OptiScaler would never hand
 * DLSS to d4r. OptiScaler.dll imports version.dll, which Windows looks for in the game's folder first, and Windows
 * initialises a DLL's imports before the DLL itself. This version.dll therefore runs before OptiScaler: it loads
 * d4r's nvapi64.dll from its own folder and forwards every version.dll function to System32's, through one jump
 * each, so any signature passes through unchanged. See docs/windows.md. */
#include <windows.h>

#define D4R_VERSION_EXPORTS(X) \
    X(0, GetFileVersionInfoA) \
    X(1, GetFileVersionInfoByHandle) \
    X(2, GetFileVersionInfoExA) \
    X(3, GetFileVersionInfoExW) \
    X(4, GetFileVersionInfoSizeA) \
    X(5, GetFileVersionInfoSizeExA) \
    X(6, GetFileVersionInfoSizeExW) \
    X(7, GetFileVersionInfoSizeW) \
    X(8, GetFileVersionInfoW) \
    X(9, VerFindFileA) \
    X(10, VerFindFileW) \
    X(11, VerInstallFileA) \
    X(12, VerInstallFileW) \
    X(13, VerLanguageNameA) \
    X(14, VerLanguageNameW) \
    X(15, VerQueryValueA) \
    X(16, VerQueryValueW)

enum { EXPORT_COUNT = 17 };

/* System32's functions; one the system lacks fails the call (returns 0) */
void* d4r_version_real[EXPORT_COUNT];

#define NAME(index, name) #name,
static const char* const names[EXPORT_COUNT] = {D4R_VERSION_EXPORTS(NAME)};

#define THUNK(index, name) \
    ".globl " #name "\n" #name ":\n" \
    "    jmp *d4r_version_real+" #index "*8(%rip)\n"
__asm__(".text\n" D4R_VERSION_EXPORTS(THUNK));

static int __stdcall missing(void)
{
    SetLastError(ERROR_PROC_NOT_FOUND);
    return 0;
}

/* lets d4r's setup and tests tell this version.dll from others */
__declspec(dllexport) int d4r_preload_version(void)
{
    return 1;
}

static void folder_of(HMODULE module, wchar_t* path, DWORD size)
{
    const DWORD length = GetModuleFileNameW(module, path, size);
    wchar_t* slash = length > 0 && length < size ? wcsrchr(path, L'\\') : NULL;
    if (slash != NULL)
        slash[1] = L'\0';
    else
        path[0] = L'\0';
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(instance);

    wchar_t path[MAX_PATH];
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    HMODULE system = NULL;
    if (length > 0 && length + 13 < MAX_PATH)
    {
        lstrcatW(path, L"\\version.dll");
        system = LoadLibraryW(path);
    }
    for (int i = 0; i < EXPORT_COUNT; ++i)
    {
        void* function = system != NULL ? (void*)GetProcAddress(system, names[i]) : NULL;
        d4r_version_real[i] = function != NULL ? function : (void*)&missing;
    }

    /* d4r's NVAPI, unless an nvapi64.dll is loaded already */
    if (GetModuleHandleW(L"nvapi64.dll") == NULL)
    {
        folder_of(instance, path, MAX_PATH);
        if (path[0] != L'\0' && lstrlenW(path) + 12 < MAX_PATH)
        {
            lstrcatW(path, L"nvapi64.dll");
            if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
                LoadLibraryW(path);
        }
    }
    return TRUE;
}
