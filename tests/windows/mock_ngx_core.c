/* A stand-in for NVIDIA's NGX core (_nvngx.dll) CUDA path, for the native shim test: like the real core it
   finds the CUDA driver with LoadLibrary("nvcuda.dll") and its functions with cuGetProcAddress, initialises
   CUDA and loads a DLSS module (a fatbin around MODULE.ptx, from D4R_TEST_MODULE). mock_ngx_report says
   which nvcuda.dll answered. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPORT __declspec(dllexport)
typedef unsigned int NgxResult;
typedef int(WINAPI* GetProcFn)(const char*, void**, int, uint64_t);
typedef int(WINAPI* InitFn)(unsigned int);
typedef int(WINAPI* ModuleLoadDataFn)(void**, const void*);

static char nvcuda_path[MAX_PATH];
static int init_result = -1, module_result = -1;

static void load_dlss_module(ModuleLoadDataFn load)
{
    char path[MAX_PATH];
    if (GetEnvironmentVariableA("D4R_TEST_MODULE", path, sizeof(path)) == 0)
        return;
    FILE* file = fopen(path, "rb");
    if (file == NULL)
        return;
    static char ptx[1 << 16];
    const size_t size = fread(ptx, 1, sizeof(ptx) - 1, file);
    fclose(file);
    unsigned char* image = calloc(1, 16 + 64 + size + 16);
    const uint32_t magic = 0xba55ed50u, entry_header = 64;
    const uint16_t version = 1, header_size = 16, kind = 1;
    const uint64_t files_size = 64 + size, entry_size = size;
    memcpy(image, &magic, 4), memcpy(image + 4, &version, 2), memcpy(image + 6, &header_size, 2);
    memcpy(image + 8, &files_size, 8);
    memcpy(image + 16, &kind, 2), memcpy(image + 20, &entry_header, 4), memcpy(image + 24, &entry_size, 8);
    memcpy(image + 16 + 64, ptx, size);
    void* module = NULL;
    module_result = load(&module, image);
    free(image);
}

EXPORT NgxResult NVSDK_NGX_CUDA_Init(unsigned long long id, const wchar_t* path, const void* info, unsigned int version)
{
    (void)id, (void)path, (void)info, (void)version;
    HMODULE cuda = LoadLibraryA("nvcuda.dll");
    if (cuda == NULL)
        return 0xBAD00002;
    GetModuleFileNameA(cuda, nvcuda_path, sizeof(nvcuda_path));
    GetProcFn getProc = (GetProcFn)(void*)GetProcAddress(cuda, "cuGetProcAddress");
    void *init = NULL, *load = NULL;
    if (getProc == NULL || getProc("cuInit", &init, 12080, 0) != 0 || getProc("cuModuleLoadData", &load, 12080, 0) != 0)
        return 0xBAD00002;
    init_result = ((InitFn)init)(0);
    load_dlss_module((ModuleLoadDataFn)load);
    return init_result == 0 ? 1 : 0xBAD00002;
}
EXPORT NgxResult NVSDK_NGX_CUDA_Shutdown(void) { return 1; }
EXPORT NgxResult NVSDK_NGX_CUDA_GetParameters(void** p) { *p = NULL; return 1; }
EXPORT NgxResult NVSDK_NGX_CUDA_AllocateParameters(void** p) { *p = NULL; return 1; }
EXPORT NgxResult NVSDK_NGX_CUDA_GetCapabilityParameters(void** p) { *p = NULL; return 0xBAD00000; }
EXPORT NgxResult NVSDK_NGX_CUDA_DestroyParameters(void* p) { (void)p; return 1; }
EXPORT NgxResult NVSDK_NGX_CUDA_CreateFeature(unsigned int f, void* p, void** h) { (void)f, (void)p, (void)h; return 0xBAD00001; }
EXPORT NgxResult NVSDK_NGX_CUDA_ReleaseFeature(void* h) { (void)h; return 1; }
EXPORT NgxResult NVSDK_NGX_CUDA_EvaluateFeature(const void* h, void* p, void* c) { (void)h, (void)p, (void)c; return 0xBAD00001; }

EXPORT void mock_ngx_report(const char** cuda, int* init, int* module)
{
    *cuda = nvcuda_path, *init = init_result, *module = module_result;
}
