/* A stand-in for ZLUDA's Windows nvcuda.dll (built as zluda_nvcuda.dll) for the native bridge test: host
   memory instead of a GPU, and counters the test reads through mock_zluda_stats. */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define EXPORT __declspec(dllexport)
typedef int CUresult;

static int init_calls, sync_calls, module_loads, launches;
static char native_dir_at_init[1024];
static char wmma_fp8_native_at_init[16];

EXPORT CUresult cuInit(unsigned int flags)
{
    (void)flags;
    ++init_calls;
    /* what ZLUDA (Rust std::env) would see */
    if (GetEnvironmentVariableA("D4R_ZLUDA_NATIVE_DIR", native_dir_at_init, sizeof(native_dir_at_init)) == 0)
        native_dir_at_init[0] = '\0';
    if (GetEnvironmentVariableA("D4R_ZLUDA_WMMA_FP8_NATIVE", wmma_fp8_native_at_init, sizeof(wmma_fp8_native_at_init)) == 0)
        wmma_fp8_native_at_init[0] = '\0';
    return 0;
}
EXPORT CUresult cuDeviceGetCount(int* count) { *count = 1; return 0; }
EXPORT CUresult cuDeviceGet(int* device, int ordinal) { *device = ordinal; return 0; }
EXPORT CUresult cuCtxCreate_v2(void** context, unsigned int flags, int device)
{
    (void)flags, (void)device;
    *context = (void*)0x1234;
    return 0;
}
EXPORT CUresult cuCtxSynchronize(void) { ++sync_calls; return 0; }
EXPORT CUresult cuModuleLoadData(void** module, const void* image)
{
    (void)image;
    ++module_loads;
    *module = (void*)(uintptr_t)module_loads;
    return 0;
}
EXPORT CUresult cuModuleGetFunction(void** function, void* module, const char* name)
{
    (void)module;
    *function = _strdup(name);
    return 0;
}
EXPORT CUresult cuLaunchKernel(void* f, unsigned gx, unsigned gy, unsigned gz, unsigned bx, unsigned by, unsigned bz,
                               unsigned shared, void* stream, void** params, void** extra)
{
    (void)f, (void)gx, (void)gy, (void)gz, (void)bx, (void)by, (void)bz, (void)shared, (void)stream, (void)params, (void)extra;
    ++launches;
    return 0;
}
EXPORT CUresult cuMemAlloc_v2(uint64_t* pointer, size_t bytes)
{
    *pointer = (uint64_t)(uintptr_t)malloc(bytes);
    return *pointer != 0 ? 0 : 2;
}
EXPORT CUresult cuMemFree_v2(uint64_t pointer) { free((void*)(uintptr_t)pointer); return 0; }
EXPORT CUresult cuDeviceGetLuid(char* luid, unsigned int* mask, int device)
{
    (void)device;
    for (int i = 0; i < 8; ++i)
        luid[i] = (char)(i + 1);
    *mask = 1;
    return 0;
}
EXPORT CUresult cuGetErrorString(CUresult code, const char** message)
{
    (void)code;
    *message = "mock error";
    return 0;
}
EXPORT CUresult cuDriverGetVersion(int* version) { *version = 12080; return 0; }

EXPORT void mock_zluda_stats(int* init, int* sync, int* loads, int* launched, const char** native_dir,
                             const char** fp8_native)
{
    *init = init_calls, *sync = sync_calls, *loads = module_loads, *launched = launches;
    *native_dir = native_dir_at_init;
    *fp8_native = wmma_fp8_native_at_init;
}
