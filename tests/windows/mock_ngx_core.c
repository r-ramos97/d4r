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

#ifdef MOCK_NGX_PARAMETERS
/* Parameter objects with the NGX core's interface (the shim's own implementation, tools/d4r_ngx_param_msvc.cpp),
   a feature, and an "evaluation" that upscales the colour input into the output by nearest neighbour, through the
   mock ZLUDA's arrays: enough for the shim's whole D3D12 path to run (tests/test_windows_native.py). */
void* d4r_ngx_parameters_create(void);
void d4r_ngx_parameters_destroy(void* parameters);
void d4r_ngx_set_int(void* parameters, const char* name, int value);
unsigned int d4r_ngx_get_void(void* parameters, const char* name, void** value);

static int evaluations;

EXPORT NgxResult NVSDK_NGX_CUDA_GetParameters(void** p) { *p = d4r_ngx_parameters_create(); return 1; }
EXPORT NgxResult NVSDK_NGX_CUDA_AllocateParameters(void** p) { *p = d4r_ngx_parameters_create(); return 1; }
EXPORT NgxResult NVSDK_NGX_CUDA_GetCapabilityParameters(void** p)
{
    *p = d4r_ngx_parameters_create();
    d4r_ngx_set_int(*p, "SuperSampling.Available", 1);
    d4r_ngx_set_int(*p, "SuperSampling.FeatureInitResult", 1);
    d4r_ngx_set_int(*p, "SuperSampling.NeedsUpdatedDriver", 0);
    return 1;
}
EXPORT NgxResult NVSDK_NGX_CUDA_DestroyParameters(void* p) { d4r_ngx_parameters_destroy(p); return 1; }
EXPORT NgxResult NVSDK_NGX_CUDA_CreateFeature(unsigned int f, void* p, void** h)
{
    (void)p;
    if (f != 1 || h == NULL)
        return 0xBAD00001;
    *h = calloc(1, 64);
    return 1;
}
EXPORT NgxResult NVSDK_NGX_CUDA_ReleaseFeature(void* h) { free(h); return 1; }

typedef int (*ObjectFn)(uint64_t, unsigned char**, size_t*, size_t*, size_t*, unsigned int*);

EXPORT NgxResult NVSDK_NGX_CUDA_EvaluateFeature(const void* h, void* p, void* c)
{
    (void)h, (void)c;
    ObjectFn object = (ObjectFn)(void*)GetProcAddress(GetModuleHandleA("zluda_nvcuda.dll"), "mock_zluda_object");
    void *colorRef = NULL, *outputRef = NULL;
    if (object == NULL || d4r_ngx_get_void(p, "Color", &colorRef) != 1 || d4r_ngx_get_void(p, "Output", &outputRef) != 1 ||
        colorRef == NULL || outputRef == NULL)
        return 0xBAD00005;
    unsigned char *in, *out;
    size_t inWidth, inHeight, inPitch, outWidth, outHeight, outPitch;
    unsigned int inBytes, outBytes;
    if (!object(*(const uint64_t*)colorRef, &in, &inWidth, &inHeight, &inPitch, &inBytes) ||
        !object(*(const uint64_t*)outputRef, &out, &outWidth, &outHeight, &outPitch, &outBytes) || inBytes != outBytes)
        return 0xBAD00005;
    for (size_t y = 0; y < outHeight; ++y)
        for (size_t x = 0; x < outWidth; ++x)
            memcpy(out + y * outPitch + x * outBytes, in + (y * inHeight / outHeight) * inPitch + (x * inWidth / outWidth) * inBytes,
                   outBytes);
    ++evaluations;
    return 1;
}

EXPORT int mock_ngx_evaluations(void) { return evaluations; }
#else
EXPORT NgxResult NVSDK_NGX_CUDA_GetParameters(void** p) { *p = NULL; return 1; }
EXPORT NgxResult NVSDK_NGX_CUDA_AllocateParameters(void** p) { *p = NULL; return 1; }
EXPORT NgxResult NVSDK_NGX_CUDA_GetCapabilityParameters(void** p) { *p = NULL; return 0xBAD00000; }
EXPORT NgxResult NVSDK_NGX_CUDA_DestroyParameters(void* p) { (void)p; return 1; }
EXPORT NgxResult NVSDK_NGX_CUDA_CreateFeature(unsigned int f, void* p, void** h) { (void)f, (void)p, (void)h; return 0xBAD00001; }
EXPORT NgxResult NVSDK_NGX_CUDA_ReleaseFeature(void* h) { (void)h; return 1; }
EXPORT NgxResult NVSDK_NGX_CUDA_EvaluateFeature(const void* h, void* p, void* c) { (void)h, (void)p, (void)c; return 0xBAD00001; }
#endif

EXPORT void mock_ngx_report(const char** cuda, int* init, int* module)
{
    *cuda = nvcuda_path, *init = init_result, *module = module_result;
}
