/* d4r's version.dll (tools/d4r_preload_version.c) in a game folder with a stand-in OptiScaler (dxgi.dll): d4r's
   NVAPI must be loaded before OptiScaler's DllMain runs, and version.dll's functions must still work.
   usage: preload_test.exe GAME_DIR (this program sits in GAME_DIR, as a game's .exe does) */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void check(int condition, const char* name)
{
    printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    failures += !condition;
}

typedef int (*SawFn)(void);
typedef DWORD (*SizeFn)(const wchar_t*);

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\dxgi.dll", argv[1]);
    HMODULE opti = LoadLibraryA(path);
    check(opti != NULL, "stand-in OptiScaler loads");
    if (opti == NULL)
        return 1;
    SawFn saw = (SawFn)(void*)GetProcAddress(opti, "fake_optiscaler_saw_nvapi");
    SizeFn versionSize = (SizeFn)(void*)GetProcAddress(opti, "fake_optiscaler_version_size");
    check(saw != NULL && versionSize != NULL, "the game folder's dxgi.dll is the stand-in (Wine: dxgi=n)");
    if (saw == NULL || versionSize == NULL)
        return 1;
    HMODULE version = GetModuleHandleA("version.dll");
    check(version != NULL && GetProcAddress(version, "d4r_preload_version") != NULL,
          "its version.dll import is d4r's, from the game folder");
    check(saw() == 1, "d4r's nvapi64.dll was loaded before OptiScaler's DllMain");
    char nvapi[MAX_PATH] = {0};
    GetModuleFileNameA(GetModuleHandleA("nvapi64.dll"), nvapi, sizeof(nvapi));
    check(_strnicmp(nvapi, argv[1], strlen(argv[1])) == 0, "the NVAPI is the game folder's");

    wchar_t kernel32[MAX_PATH];
    GetSystemDirectoryW(kernel32, MAX_PATH);
    lstrcatW(kernel32, L"\\kernel32.dll");
    const DWORD size = versionSize(kernel32);
    check(size > 0, "GetFileVersionInfoSizeW forwards to System32's version.dll");
    BYTE* data = (BYTE*)malloc(size);
    VS_FIXEDFILEINFO* info = NULL;
    UINT length = 0;
    typedef BOOL(WINAPI * InfoFn)(LPCWSTR, DWORD, DWORD, LPVOID);
    typedef BOOL(WINAPI * QueryFn)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    InfoFn getInfo = (InfoFn)(void*)GetProcAddress(version, "GetFileVersionInfoW");
    QueryFn query = (QueryFn)(void*)GetProcAddress(version, "VerQueryValueW");
    check(getInfo != NULL && query != NULL && getInfo(kernel32, 0, size, data) &&
              query(data, L"\\", (LPVOID*)&info, &length) && info != NULL && info->dwSignature == 0xfeef04bd,
          "GetFileVersionInfoW and VerQueryValueW read kernel32's version");
    free(data);
    printf("%d failure(s)\n", failures);
    return failures != 0;
}
