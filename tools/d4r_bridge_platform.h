/* What differs between the two builds of the nvcuda bridge (tools/wine_nvcuda_bridge.c):
 *
 * - the Wine builtin (winegcc, scripts/build_wine_nvcuda_bridge.sh): Windows exports on the Unix side of a
 *   Proton game, forwarding to ZLUDA's Linux libcuda.so and ROCm, whose functions use the System V ABI;
 * - D4R_NATIVE_WINDOWS (MinGW-w64, scripts/build_windows_nvcuda.sh): a native Windows nvcuda.dll for
 *   Windows games, forwarding to ZLUDA's Windows build and the HIP SDK's amdhip64 in the same process,
 *   all with the Windows x64 ABI.
 *
 * Everything the bridge needs from the operating system beyond the C library goes through this header. */
#pragma once

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef D4R_NATIVE_WINDOWS

#include <direct.h>
#include <io.h>
#include <process.h>

/* ZLUDA and HIP are Windows DLLs here: no ABI switch. */
#define D4R_UNIX_ABI

#define RTLD_NOW 0
#define RTLD_LOCAL 0
#define RTLD_NOLOAD 4

static char d4r_dl_error[512];

/* LoadLibrary with dlopen's interface: a path with a directory loads that file (its dependencies are
   searched in its directory first), RTLD_NOLOAD only finds an already-loaded module. */
static inline void* dlopen(const char* path, int flags)
{
    HMODULE module = (flags & RTLD_NOLOAD) != 0
        ? GetModuleHandleA(path)
        : LoadLibraryExA(path, NULL, strpbrk(path, "\\/") != NULL ? LOAD_WITH_ALTERED_SEARCH_PATH : 0);
    if (module == NULL)
        snprintf(d4r_dl_error, sizeof(d4r_dl_error), "%s: Windows error %lu", path, GetLastError());
    return (void*)module;
}

static inline void* dlsym(void* module, const char* name)
{
    return module != NULL ? (void*)GetProcAddress((HMODULE)module, name) : NULL;
}

static inline const char* dlerror(void)
{
    return d4r_dl_error[0] != '\0' ? d4r_dl_error : NULL;
}

/* The C runtime keeps its own copy of the environment: update both it (getenv) and the process
   environment (GetEnvironmentVariable, and Rust's std::env in ZLUDA). */
static inline int setenv(const char* name, const char* value, int overwrite)
{
    if (!overwrite && getenv(name) != NULL)
        return 0;
    if (_putenv_s(name, value) != 0)
        return -1;
    return SetEnvironmentVariableA(name, value) ? 0 : -1;
}

static inline int unsetenv(const char* name)
{
    _putenv_s(name, "");
    return SetEnvironmentVariableA(name, NULL) || GetLastError() == ERROR_ENVVAR_NOT_FOUND ? 0 : -1;
}

static inline int d4r_mkdir(const char* path)
{
    return _mkdir(path);
}

static inline int d4r_is_separator(char c)
{
    return c == '/' || c == '\\';
}

/* Native kernels are served from a per-process directory: a hard link where the volume allows it, else
   a copy (the code objects are small). */
static inline int d4r_link_file(const char* target, const char* link)
{
    if (CreateHardLinkA(link, target, NULL) || CopyFileA(target, link, TRUE))
        return 0;
    return GetLastError() == ERROR_ALREADY_EXISTS || GetLastError() == ERROR_FILE_EXISTS ? 0 : -1;
}

static inline int d4r_process_exited(long pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (process == NULL)
        return GetLastError() == ERROR_INVALID_PARAMETER;
    DWORD code = 0;
    const int exited = GetExitCodeProcess(process, &code) && code != STILL_ACTIVE;
    CloseHandle(process);
    return exited;
}

static inline long d4r_process_id(void)
{
    return (long)GetCurrentProcessId();
}

/* Bytes readable from address to the end of its memory region, 0 when it is not readable. */
static inline size_t d4r_readable_span(const void* address)
{
    MEMORY_BASIC_INFORMATION info;
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
        return 0;
    const uintptr_t end = (uintptr_t)info.BaseAddress + info.RegionSize;
    return (size_t)(end - (uintptr_t)address);
}

/* The per-user base for d4r's caches: %LOCALAPPDATA%, where ZLUDA keeps its own cache too. */
static inline const char* d4r_user_cache_base(char* out, size_t size)
{
    const char* local = getenv("LOCALAPPDATA");
    const char* profile = getenv("USERPROFILE");
    if (local != NULL && local[0] != '\0')
        snprintf(out, size, "%s", local);
    else if (profile != NULL && profile[0] != '\0')
        snprintf(out, size, "%s/AppData/Local", profile);
    else
        snprintf(out, size, "C:/Windows/Temp");
    return out;
}

static inline const char* d4r_home(void)
{
    const char* profile = getenv("USERPROFILE");
    return profile != NULL ? profile : getenv("HOME");
}

#else /* the Wine builtin */

#include <dlfcn.h>
#include <signal.h>

/* ZLUDA's libcuda.so and ROCm are Linux libraries. */
#define D4R_UNIX_ABI __attribute__((sysv_abi))

static inline int d4r_mkdir(const char* path)
{
    return mkdir(path, 0755);
}

static inline int d4r_is_separator(char c)
{
    return c == '/';
}

static inline int d4r_link_file(const char* target, const char* link)
{
    return symlink(target, link) == 0 || errno == EEXIST ? 0 : -1;
}

static inline int d4r_process_exited(long pid)
{
    return kill((pid_t)pid, 0) != 0 && errno == ESRCH;
}

static inline long d4r_process_id(void)
{
    return (long)getpid();
}

static inline size_t d4r_readable_span(const void* address)
{
    FILE* maps = fopen("/proc/self/maps", "r");
    if (maps == NULL)
        return 0;
    const uintptr_t target = (uintptr_t)address;
    char line[512];
    size_t span = 0;
    while (fgets(line, sizeof(line), maps) != NULL)
    {
        unsigned long long begin = 0, end = 0;
        char permissions[5] = {0};
        if (sscanf(line, "%llx-%llx %4s", &begin, &end, permissions) == 3 &&
            permissions[0] == 'r' && target >= begin && target < end)
        {
            span = (size_t)(end - target);
            break;
        }
    }
    fclose(maps);
    return span;
}

static inline const char* d4r_user_cache_base(char* out, size_t size)
{
    const char* xdg = getenv("XDG_CACHE_HOME");
    const char* home = getenv("HOME");
    if (xdg != NULL && xdg[0] != '\0')
        snprintf(out, size, "%s", xdg);
    else
        snprintf(out, size, "%s/.cache", home != NULL ? home : "/tmp");
    return out;
}

static inline const char* d4r_home(void)
{
    return getenv("HOME");
}

#endif
