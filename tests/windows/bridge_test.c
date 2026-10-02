/* Loads the native Windows nvcuda bridge the way the NGX shim and NVIDIA's NGX core do, against a mock
   ZLUDA (zluda/zluda_nvcuda.dll) and a mock HIP SDK (HIP_PATH\bin\amdhip64_7.dll), and checks what the
   bridge adds: the GPU and native kernel set it picks, PTX-hash verification of native kernels, NGX sync
   elision, cuGetProcAddress and the d4rSetEnv hand-over. tests/test_windows_native.py prepares the folder.

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

    printf("%d failure(s)\n", failures);
    return failures != 0;
}
