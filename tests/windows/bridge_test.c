/* Loads the native Windows nvcuda bridge the way the NGX shim and NVIDIA's NGX core do, against a mock
   ZLUDA (zluda/zluda_nvcuda.dll) and a mock HIP SDK (HIP_PATH\bin\amdhip64_7.dll), and checks what the
   bridge adds: the GPU and native kernel set it picks, PTX-hash verification of native kernels, NGX sync
   elision, cuGetProcAddress, the d4rSetEnv hand-over, the single CUDA device NGX sees and the D3D12 interop
   exports. tests/test_windows_native.py prepares the folder.

   usage: bridge_test.exe D4R_DIR CACHE_DIR MODULE.ptx */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int(WINAPI* SetEnvFn)(const char*, const char*, int);
typedef int(WINAPI* InitFn)(unsigned int);
typedef int(WINAPI* ModuleLoadDataFn)(void**, const void*);
typedef int(WINAPI* SyncFn)(void);
typedef int(WINAPI* GetProcFn)(const char*, void**, int, uint64_t);
typedef const char*(WINAPI* LoadErrorFn)(void);
typedef void (*StatsFn)(int*, int*, int*, int*, const char**, const char**);
typedef int(WINAPI* DeviceCountFn)(int*);
typedef int(WINAPI* DeviceGetFn)(int*, int);
typedef int(WINAPI* ImportMemoryFn)(void*, uint32_t, uint64_t, uint64_t*, void**);
typedef int(WINAPI* ImportSemaphoreFn)(void*, uint32_t, void**);
typedef int(WINAPI* SemaphoreValueFn)(void*, uint64_t);
typedef int(WINAPI* ReleaseFn)(void*);
typedef int (*ContextDeviceFn)(void);
typedef void (*InteropFn)(int*, void**, uint64_t*, unsigned int*, uint64_t*, int*, int*, void**, uint64_t*, uint64_t*,
                          void**, int*);

static int failures;

static void check(int condition, const char* name, const char* detail)
{
    printf("%s %s%s%s\n", condition ? "PASS" : "FAIL", name, condition ? "" : ": ", condition ? "" : detail);
    failures += !condition;
}

static int file_exists(const char* path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* a CUDA fatbin v1 container with one uncompressed PTX entry, as NGX passes its modules */
static unsigned char* make_fatbin(const char* ptx, size_t ptx_size)
{
    const size_t entry_header = 64, header = 16;
    unsigned char* image = calloc(1, header + entry_header + ptx_size + 16);
    const uint32_t magic = 0xba55ed50u;
    const uint16_t version = 1, header_size = (uint16_t)header;
    const uint64_t files_size = entry_header + ptx_size;
    memcpy(image, &magic, 4);
    memcpy(image + 4, &version, 2);
    memcpy(image + 6, &header_size, 2);
    memcpy(image + 8, &files_size, 8);
    unsigned char* entry = image + header;
    const uint16_t kind = 1; /* PTX */
    const uint32_t entry_header_size = (uint32_t)entry_header;
    const uint64_t entry_size = ptx_size;
    memcpy(entry, &kind, 2);
    memcpy(entry + 4, &entry_header_size, 4);
    memcpy(entry + 8, &entry_size, 8);
    memcpy(entry + entry_header, ptx, ptx_size);
    return image;
}

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        fprintf(stderr, "usage: bridge_test.exe D4R_DIR CACHE_DIR MODULE.ptx\n");
        return 2;
    }
    const char* d4r = argv[1];
    char path[MAX_PATH], kernels[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\nvcuda.dll", d4r);
    snprintf(kernels, sizeof(kernels), "%s\\kernels", d4r);

    HMODULE bridge = LoadLibraryA(path);
    check(bridge != NULL, "bridge loads", path);
    if (bridge == NULL)
        return 1;
    SetEnvFn setEnv = (SetEnvFn)(void*)GetProcAddress(bridge, "d4rSetEnv");
    InitFn init = (InitFn)(void*)GetProcAddress(bridge, "cuInit");
    ModuleLoadDataFn loadData = (ModuleLoadDataFn)(void*)GetProcAddress(bridge, "cuModuleLoadData");
    SyncFn ctxSync = (SyncFn)(void*)GetProcAddress(bridge, "cuCtxSynchronize");
    SyncFn ownSync = (SyncFn)(void*)GetProcAddress(bridge, "d4rCtxSynchronize");
    GetProcFn getProc = (GetProcFn)(void*)GetProcAddress(bridge, "cuGetProcAddress");
    LoadErrorFn loadError = (LoadErrorFn)(void*)GetProcAddress(bridge, "d4rLoadError");
    check(setEnv && init && loadData && ctxSync && ownSync && getProc && loadError, "bridge exports", "missing");

    /* the shim's hand-over (d4r.ini settings), before the first CUDA call */
    setEnv("D4R_ZLUDA_NATIVE_DIR", kernels, 1);
    setEnv("D4R_ZLUDA_WMMA_FP8_NATIVE", "1", 1);
    setEnv("D4R_ZLUDA_CACHE_HOME", argv[2], 1);
    setEnv("D4R_ELIDE_NGX_SYNC", "1", 1);
    setEnv("D4R_PREFER_ACCURACY", "0", 1);
    setEnv("D4R_CUDA_CAPTURE", "0", 1);
    /* the game's D3D12 adapter, which the shim names before NGX starts: the RX 9070 XT, not the larger GPU */
    setEnv("D4R_CUDA_LUID_LOW", "0x00002000", 1);
    setEnv("D4R_CUDA_LUID_HIGH", "0x00000000", 1);
    char value[64] = {0};
    GetEnvironmentVariableA("D4R_ELIDE_NGX_SYNC", value, sizeof(value));
    check(strcmp(value, "1") == 0 && getenv("D4R_ELIDE_NGX_SYNC") != NULL, "d4rSetEnv reaches the process", value);
    check(setEnv("D4R_ELIDE_NGX_SYNC", "0", 0) == 1 && strcmp(getenv("D4R_ELIDE_NGX_SYNC"), "1") == 0,
          "d4rSetEnv keeps an existing value without overwrite", getenv("D4R_ELIDE_NGX_SYNC"));

    check(init(0) == 0, "cuInit through the bridge", loadError());
    HMODULE zluda = GetModuleHandleA("zluda_nvcuda.dll");
    check(zluda != NULL, "ZLUDA loaded from the zluda folder", loadError());
    HMODULE hip = GetModuleHandleA("amdhip64_7.dll");
    check(hip != NULL, "HIP preloaded from HIP_PATH", loadError());
    StatsFn stats = zluda != NULL ? (StatsFn)(void*)GetProcAddress(zluda, "mock_zluda_stats") : NULL;
    if (stats == NULL)
        return 1;

    int inits, syncs, loads, launches;
    const char *served, *fp8;
    stats(&inits, &syncs, &loads, &launches, &served, &fp8);
    char expected_prefix[MAX_PATH];
    snprintf(expected_prefix, sizeof(expected_prefix), "%s/d4r-native/", argv[2]);
    check(strncmp(served, expected_prefix, strlen(expected_prefix)) == 0,
          "ZLUDA is pointed at a per-process native directory", served);
    check(strcmp(fp8, "1") == 0, "ZLUDA sees the hand-over environment", fp8);

    /* NGX loads its modules: enc0's PTX matches the manifest, dec0's does not */
    FILE* file = fopen(argv[3], "rb");
    check(file != NULL, "module PTX readable", argv[3]);
    if (file == NULL)
        return 1;
    static char ptx[1 << 16];
    const size_t ptx_size = fread(ptx, 1, sizeof(ptx) - 1, file);
    fclose(file);
    unsigned char* image = make_fatbin(ptx, ptx_size);
    void* module = NULL;
    check(loadData(&module, image) == 0 && module != NULL, "cuModuleLoadData forwards", "");
    free(image);
    char enc0[MAX_PATH], dec0[MAX_PATH];
    snprintf(enc0, sizeof(enc0), "%s/dltss_pwin_enc0_layer.hsaco", served);
    snprintf(dec0, sizeof(dec0), "%s/dltss_pwin_dec0_layer.hsaco", served);
    check(file_exists(enc0), "matching native kernel served (gfx1201-fp8 set)", enc0);
    check(!file_exists(dec0), "native kernel with a different PTX hash not served", dec0);
    char source[MAX_PATH];
    snprintf(source, sizeof(source), "%s\\gfx1201-fp8\\dltss_pwin_enc0_layer.hsaco", kernels);
    FILE* served_file = fopen(enc0, "rb");
    char served_bytes[32] = {0};
    if (served_file != NULL)
    {
        fread(served_bytes, 1, sizeof(served_bytes) - 1, served_file);
        fclose(served_file);
    }
    check(strcmp(served_bytes, "gfx1201-fp8 enc0") == 0, "served kernel comes from the discrete GPU's FP8 set",
          served_bytes);

    /* NGX's synchronisations are elided; the shim's own wait is not */
    ctxSync();
    stats(&inits, &syncs, &loads, &launches, &served, &fp8);
    check(syncs == 0, "cuCtxSynchronize elided with D4R_ELIDE_NGX_SYNC=1", "");
    ownSync();
    stats(&inits, &syncs, &loads, &launches, &served, &fp8);
    check(syncs == 1, "d4rCtxSynchronize still waits", "");

    /* NGX resolves the driver API through cuGetProcAddress and LoadLibrary("nvcuda.dll") */
    void* launch = NULL;
    check(getProc("cuLaunchKernel", &launch, 12080, 0) == 0 && launch == (void*)GetProcAddress(bridge, "cuLaunchKernel"),
          "cuGetProcAddress returns the bridge's own exports", "");
    check(LoadLibraryA("nvcuda.dll") == bridge, "LoadLibrary(\"nvcuda.dll\") finds the bridge, not ZLUDA", "");
    /* a loader that wants System32's CUDA driver (as NVIDIA's own components may) gets the loaded bridge too */
    check(LoadLibraryExA("nvcuda.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32) == bridge,
          "LoadLibraryEx(\"nvcuda.dll\", SEARCH_SYSTEM32) finds the loaded bridge", "");

    /* NGX sees one CUDA device: the HIP GPU of the game's D3D12 adapter */
    DeviceCountFn deviceCount = (DeviceCountFn)(void*)GetProcAddress(bridge, "cuDeviceGetCount");
    DeviceGetFn deviceGet = (DeviceGetFn)(void*)GetProcAddress(bridge, "cuDeviceGet");
    int count = 0, device = -1;
    check(deviceCount != NULL && deviceCount(&count) == 0 && count == 1, "NGX sees one CUDA device of three GPUs", "");
    check(deviceGet != NULL && deviceGet(&device, 0) == 0 && device == 1,
          "CUDA device 0 is the HIP GPU whose LUID is the D3D12 adapter's", "");
    check(deviceGet != NULL && deviceGet(&device, 1) != 0, "CUDA device 1 does not exist", "");
    ContextDeviceFn contextDevice = (ContextDeviceFn)(void*)GetProcAddress(zluda, "mock_zluda_context_device");

    /* D3D12 interop: shared NT handles reach HIP with HIP's descriptor layout */
    ImportMemoryFn importMemory = (ImportMemoryFn)(void*)GetProcAddress(bridge, "d4rImportWin32Memory");
    ImportSemaphoreFn importSemaphore = (ImportSemaphoreFn)(void*)GetProcAddress(bridge, "d4rImportWin32Semaphore");
    SemaphoreValueFn waitSemaphore = (SemaphoreValueFn)(void*)GetProcAddress(bridge, "d4rWaitSemaphore");
    SemaphoreValueFn signalSemaphore = (SemaphoreValueFn)(void*)GetProcAddress(bridge, "d4rSignalSemaphore");
    ReleaseFn releaseMemory = (ReleaseFn)(void*)GetProcAddress(bridge, "d4rReleaseVulkanMemory");
    ReleaseFn releaseSemaphore = (ReleaseFn)(void*)GetProcAddress(bridge, "d4rReleaseSemaphore");
    InteropFn interop = (InteropFn)(void*)GetProcAddress(hip, "mock_hip_interop");
    check(importMemory && importSemaphore && waitSemaphore && signalSemaphore && releaseMemory && releaseSemaphore &&
              interop && contextDevice,
          "interop exports", "missing");
    if (!(importMemory && importSemaphore && waitSemaphore && signalSemaphore && releaseMemory && releaseSemaphore &&
          interop && contextDevice))
        return 1;
    uint64_t pointer = 0;
    void *memory = NULL, *semaphore = NULL;
    check(importMemory((void*)(uintptr_t)0x1234, 5, 1 << 20, &pointer, &memory) == 0 && pointer != 0 && memory != NULL,
          "d4rImportWin32Memory maps a D3D12 resource", "");
    check(contextDevice() == 1, "the bridge's own context (created by its first helper) is on that GPU too", "");
    check(importSemaphore((void*)(uintptr_t)0x5678, 4, &semaphore) == 0 && semaphore != NULL,
          "d4rImportWin32Semaphore imports a D3D12 fence", "");
    check(waitSemaphore(semaphore, 7) == 0 && signalSemaphore(semaphore, 8) == 0, "fence wait and signal queue", "");
    check(releaseMemory(memory) == 0 && releaseSemaphore(semaphore) == 0, "imports release", "");
    int memoryType, memoryDestroyed, semaphoreType, semaphoreDestroyed;
    void *memoryHandle, *semaphoreHandle, *waitStream;
    uint64_t memorySize, mappedSize, waited, signalled;
    unsigned int memoryFlags;
    interop(&memoryType, &memoryHandle, &memorySize, &memoryFlags, &mappedSize, &memoryDestroyed, &semaphoreType,
            &semaphoreHandle, &waited, &signalled, &waitStream, &semaphoreDestroyed);
    check(memoryType == 5 && memoryHandle == (void*)(uintptr_t)0x1234 && memorySize == 1 << 20 && memoryFlags == 1,
          "HIP gets a dedicated D3D12 resource import (type 5, handle, size, flags 1)", "");
    check(mappedSize == 1 << 20, "the whole resource is mapped", "");
    check(semaphoreType == 4 && semaphoreHandle == (void*)(uintptr_t)0x5678, "HIP gets a D3D12 fence (type 4)", "");
    check(waited == 7 && signalled == 8 && waitStream == NULL, "fence values on the null stream, where NGX runs", "");
    check(memoryDestroyed && semaphoreDestroyed, "HIP releases both imports", "");

    printf("%d failure(s)\n", failures);
    return failures != 0;
}
