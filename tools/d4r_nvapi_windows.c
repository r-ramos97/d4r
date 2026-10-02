/* nvapi64.dll for native Windows installs (docs/windows.md): the NVIDIA identity d4r needs on an AMD GPU.
 *
 * Under Proton, dxvk-nvapi with DXVK_NVAPI_GPU_ARCH=AD100 reports an Ada GPU: OptiScaler then offers its DLSS
 * backend, and NVIDIA's NGX core picks the network weights that match the sm_89 PTX ZLUDA compiles. A Windows PC
 * with an AMD GPU has no NVAPI at all. This DLL goes in the game folder (where OptiScaler and NGX load nvapi64.dll
 * from) and answers the GPU identity queries: one physical and logical GPU with the architecture, driver version
 * and name below, and the LUID of the system's main GPU so NGX matches it with the D3D12 device and ZLUDA's CUDA
 * device. Every other interface is left unimplemented (NULL), as on a GPU without the feature, unless
 * D4R_NVAPI_CHAIN names another nvapi64.dll (for example fakenvapi) to forward it to.
 *
 * Environment (read once):
 *   D4R_NVAPI_GPU_ARCH        AD100 (default), GA100 or TU100: the architecture reported
 *   D4R_NVAPI_DRIVER_VERSION  driver version reported (default 596.36, the driver whose NGX core d4r tested)
 *   D4R_NVAPI_LUID            adapter LUID as HIGH:LOW hex (default: the non-software DXGI adapter with the most
 *                             dedicated memory)
 *   D4R_NVAPI_CHAIN           another nvapi64.dll whose interfaces this one does not implement are forwarded to
 *   D4R_NVAPI_LOG             log file (default d4r_nvapi.log next to this DLL); 0 turns logging off
 *
 * The log names every interface asked for once, so a game or NGX version that needs more can be seen. */
#include <windows.h>
#include <dxgi.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d4r_nvapi_names.h"

typedef int NvAPI_Status;
enum
{
    NVAPI_OK = 0,
    NVAPI_ERROR = -1,
    NVAPI_INVALID_ARGUMENT = -5,
    NVAPI_NVIDIA_DEVICE_NOT_FOUND = -6,
    NVAPI_INCOMPATIBLE_STRUCT_VERSION = -9,
    NVAPI_EXPECTED_PHYSICAL_GPU_HANDLE = -101,
    NVAPI_EXPECTED_LOGICAL_GPU_HANDLE = -100,
};

#define NVAPI_SHORT_STRING_MAX 64
#define NVAPI_MAX_PHYSICAL_GPUS 64
#define NVAPI_MAX_LOGICAL_GPUS 64
#define NVAPI_VERSION(size, version) ((unsigned int)((size) | ((version) << 16)))

/* the one GPU: opaque handles callers only compare and pass back */
static char physical_gpu, logical_gpu;
#define PHYSICAL_HANDLE ((void*)&physical_gpu)
#define LOGICAL_HANDLE ((void*)&logical_gpu)
enum { GPU_ID = 0x100 };

typedef void* (*QueryInterfaceFn)(unsigned int);

static struct
{
    INIT_ONCE once;
    unsigned int architecture, implementation, revision;
    unsigned int driver_version; /* e.g. 58142 for 581.42 */
    char name[NVAPI_SHORT_STRING_MAX];
    char log_path[MAX_PATH];
    QueryInterfaceFn chain;
    CRITICAL_SECTION lock;
    unsigned int logged[1024];
    unsigned int logged_count;
} state = {.once = INIT_ONCE_STATIC_INIT};

static HMODULE self_module(void)
{
    HMODULE module = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)(void*)&self_module, &module);
    return module;
}

static void nvapi_log(const char* format, ...)
{
    if (state.log_path[0] == '\0')
        return;
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    FILE* file = fopen(state.log_path, "a");
    if (file != NULL)
    {
        fprintf(file, "%s\n", line);
        fclose(file);
    }
}

static BOOL CALLBACK initialize(PINIT_ONCE once, PVOID parameter, PVOID* context)
{
    (void)once, (void)parameter, (void)context;
    InitializeCriticalSection(&state.lock);
    char value[MAX_PATH] = {0};

    /* the architecture NGX selects its weights by; AD100 matches the sm_89 PTX ZLUDA compiles */
    state.architecture = 0x190, state.implementation = 0x02, state.revision = 0x11; /* AD100, AD102, A01 */
    snprintf(state.name, sizeof(state.name), "NVIDIA GeForce RTX 4090");
    if (GetEnvironmentVariableA("D4R_NVAPI_GPU_ARCH", value, sizeof(value)) > 0)
    {
        if (_stricmp(value, "GA100") == 0)
            state.architecture = 0x170, state.implementation = 0x02, snprintf(state.name, sizeof(state.name),
                                                                               "NVIDIA GeForce RTX 3090");
        else if (_stricmp(value, "TU100") == 0)
            state.architecture = 0x160, state.implementation = 0x02, snprintf(state.name, sizeof(state.name),
                                                                               "NVIDIA GeForce RTX 2080 Ti");
    }
    state.driver_version = 59636;
    if (GetEnvironmentVariableA("D4R_NVAPI_DRIVER_VERSION", value, sizeof(value)) > 0)
    {
        unsigned int major = 0, minor = 0;
        if (sscanf(value, "%u.%u", &major, &minor) == 2 && major < 10000 && minor < 100)
            state.driver_version = major * 100 + minor;
    }

    const DWORD length = GetEnvironmentVariableA("D4R_NVAPI_LOG", value, sizeof(value));
    if (length > 0 && length < sizeof(value))
    {
        if (strcmp(value, "0") != 0)
            snprintf(state.log_path, sizeof(state.log_path), "%s", value);
    }
    else if (GetModuleFileNameA(self_module(), state.log_path, sizeof(state.log_path)) > 0)
    {
        char* slash = strrchr(state.log_path, '\\');
        snprintf(slash != NULL ? slash + 1 : state.log_path,
                 sizeof(state.log_path) - (size_t)(slash != NULL ? slash + 1 - state.log_path : 0), "d4r_nvapi.log");
        DeleteFileA(state.log_path); /* one launch per log */
    }

    if (GetEnvironmentVariableA("D4R_NVAPI_CHAIN", value, sizeof(value)) > 0)
    {
        HMODULE chained = LoadLibraryA(value);
        state.chain = chained != NULL ? (QueryInterfaceFn)(void*)GetProcAddress(chained, "nvapi_QueryInterface") : NULL;
        nvapi_log("chaining unimplemented interfaces to %s: %s", value, state.chain != NULL ? "loaded" : "failed to load");
    }
    nvapi_log("d4r NVAPI: %s, architecture 0x%x, driver %u.%02u", state.name, state.architecture,
         state.driver_version / 100, state.driver_version % 100);
    return TRUE;
}

static void ensure_initialized(void)
{
    InitOnceExecuteOnce(&state.once, initialize, NULL, NULL);
}

/* The adapter LUID: D4R_NVAPI_LUID, else the hardware DXGI adapter with the most dedicated memory (the discrete
   GPU on a desktop with an enabled iGPU). DXGI is created lazily: never under the loader lock. */
static int adapter_luid(LUID* luid)
{
    char value[64] = {0};
    if (GetEnvironmentVariableA("D4R_NVAPI_LUID", value, sizeof(value)) > 0)
    {
        unsigned long high = 0, low = 0;
        if (sscanf(value, "%lx:%lx", &high, &low) == 2)
        {
            luid->HighPart = (LONG)high, luid->LowPart = (DWORD)low;
            return 1;
        }
    }
    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    typedef HRESULT(WINAPI * CreateFactoryFn)(REFIID, void**);
    CreateFactoryFn create = dxgi != NULL ? (CreateFactoryFn)(void*)GetProcAddress(dxgi, "CreateDXGIFactory1") : NULL;
    static const GUID factory_iid = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
    IDXGIFactory1* factory = NULL;
    if (create == NULL || FAILED(create(&factory_iid, (void**)&factory)))
        return 0;
    SIZE_T best_memory = 0;
    int found = 0;
    IDXGIAdapter1* adapter = NULL;
    for (UINT index = 0; factory->lpVtbl->EnumAdapters1(factory, index, &adapter) == S_OK; ++index)
    {
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(adapter->lpVtbl->GetDesc1(adapter, &desc)) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
            (!found || desc.DedicatedVideoMemory > best_memory))
        {
            best_memory = desc.DedicatedVideoMemory;
            *luid = desc.AdapterLuid;
            found = 1;
        }
        adapter->lpVtbl->Release(adapter);
    }
    factory->lpVtbl->Release(factory);
    return found;
}

static void copy_short_string(char* out, const char* text)
{
    snprintf(out, NVAPI_SHORT_STRING_MAX, "%s", text);
}

/* --- the interfaces -------------------------------------------------------------------------------------- */

static NvAPI_Status __cdecl Initialize(void)
{
    return NVAPI_OK;
}

static NvAPI_Status __cdecl Unload(void)
{
    return NVAPI_OK;
}

static NvAPI_Status __cdecl GetErrorMessage(NvAPI_Status status, char* text)
{
    if (text == NULL)
        return NVAPI_INVALID_ARGUMENT;
    snprintf(text, NVAPI_SHORT_STRING_MAX, "NVAPI status %d", status);
    return NVAPI_OK;
}

/* OptiScaler's NVIDIA check reads this; a string mentioning DXVK would send it down its non-NVIDIA path */
static NvAPI_Status __cdecl GetInterfaceVersionString(char* text)
{
    if (text == NULL)
        return NVAPI_INVALID_ARGUMENT;
    copy_short_string(text, "NVIDIA NVAPI (d4r for AMD GPUs)");
    return NVAPI_OK;
}

static NvAPI_Status __cdecl SYS_GetDriverAndBranchVersion(unsigned int* version, char* branch)
{
    if (version == NULL || branch == NULL)
        return NVAPI_INVALID_ARGUMENT;
    *version = state.driver_version;
    char text[NVAPI_SHORT_STRING_MAX];
    snprintf(text, sizeof(text), "r%u_00", state.driver_version / 100);
    copy_short_string(branch, text);
    return NVAPI_OK;
}

typedef struct
{
    unsigned int version;
    unsigned int drvVersion;
    unsigned int bldChangeListNum;
    char szBuildBranchString[NVAPI_SHORT_STRING_MAX];
    char szAdapterString[NVAPI_SHORT_STRING_MAX];
} NV_DISPLAY_DRIVER_VERSION;

static NvAPI_Status __cdecl GetDisplayDriverVersion(void* display, NV_DISPLAY_DRIVER_VERSION* info)
{
    (void)display;
    if (info == NULL)
        return NVAPI_INVALID_ARGUMENT;
    if (info->version != NVAPI_VERSION(sizeof(NV_DISPLAY_DRIVER_VERSION), 1))
        return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
    info->drvVersion = state.driver_version;
    info->bldChangeListNum = 0;
    char text[NVAPI_SHORT_STRING_MAX];
    snprintf(text, sizeof(text), "r%u_00", state.driver_version / 100);
    copy_short_string(info->szBuildBranchString, text);
    copy_short_string(info->szAdapterString, state.name);
    return NVAPI_OK;
}

static NvAPI_Status __cdecl EnumPhysicalGPUs(void* handles[NVAPI_MAX_PHYSICAL_GPUS], unsigned int* count)
{
    if (handles == NULL || count == NULL)
        return NVAPI_INVALID_ARGUMENT;
    handles[0] = PHYSICAL_HANDLE;
    *count = 1;
    return NVAPI_OK;
}

static NvAPI_Status __cdecl EnumLogicalGPUs(void* handles[NVAPI_MAX_LOGICAL_GPUS], unsigned int* count)
{
    if (handles == NULL || count == NULL)
        return NVAPI_INVALID_ARGUMENT;
    handles[0] = LOGICAL_HANDLE;
    *count = 1;
    return NVAPI_OK;
}

static NvAPI_Status __cdecl GetLogicalGPUFromPhysicalGPU(void* physical, void** logical)
{
    if (physical != PHYSICAL_HANDLE)
        return NVAPI_EXPECTED_PHYSICAL_GPU_HANDLE;
    if (logical == NULL)
        return NVAPI_INVALID_ARGUMENT;
    *logical = LOGICAL_HANDLE;
    return NVAPI_OK;
}

static NvAPI_Status __cdecl GetPhysicalGPUsFromLogicalGPU(void* logical, void* handles[NVAPI_MAX_PHYSICAL_GPUS],
                                                          unsigned int* count)
{
    if (logical != LOGICAL_HANDLE)
        return NVAPI_EXPECTED_LOGICAL_GPU_HANDLE;
    if (handles == NULL || count == NULL)
        return NVAPI_INVALID_ARGUMENT;
    handles[0] = PHYSICAL_HANDLE;
    *count = 1;
    return NVAPI_OK;
}

static NvAPI_Status __cdecl GetGPUIDfromPhysicalGPU(void* physical, unsigned int* id)
{
    if (physical != PHYSICAL_HANDLE)
        return NVAPI_EXPECTED_PHYSICAL_GPU_HANDLE;
    if (id == NULL)
        return NVAPI_INVALID_ARGUMENT;
    *id = GPU_ID;
    return NVAPI_OK;
}

static NvAPI_Status __cdecl GetPhysicalGPUFromGPUID(unsigned int id, void** physical)
{
    if (physical == NULL)
        return NVAPI_INVALID_ARGUMENT;
    if (id != GPU_ID)
        return NVAPI_NVIDIA_DEVICE_NOT_FOUND;
    *physical = PHYSICAL_HANDLE;
    return NVAPI_OK;
}

typedef struct
{
    unsigned int version;
    unsigned int architecture;
    unsigned int implementation;
    unsigned int revision;
} NV_GPU_ARCH_INFO;

static NvAPI_Status __cdecl GPU_GetArchInfo(void* physical, NV_GPU_ARCH_INFO* info)
{
    if (physical != PHYSICAL_HANDLE)
        return NVAPI_EXPECTED_PHYSICAL_GPU_HANDLE;
    if (info == NULL)
        return NVAPI_INVALID_ARGUMENT;
    if (info->version != NVAPI_VERSION(sizeof(NV_GPU_ARCH_INFO), 1) &&
        info->version != NVAPI_VERSION(sizeof(NV_GPU_ARCH_INFO), 2))
        return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
    info->architecture = state.architecture;
    info->implementation = state.implementation;
    info->revision = state.revision;
    return NVAPI_OK;
}

static NvAPI_Status __cdecl GPU_GetFullName(void* physical, char* name)
{
    if (physical != PHYSICAL_HANDLE)
        return NVAPI_EXPECTED_PHYSICAL_GPU_HANDLE;
    if (name == NULL)
        return NVAPI_INVALID_ARGUMENT;
    copy_short_string(name, state.name);
    return NVAPI_OK;
}

/* an AD102 board (RTX 4090), consistent with the architecture */
static NvAPI_Status __cdecl GPU_GetPCIIdentifiers(void* physical, unsigned int* device, unsigned int* subsystem,
                                                  unsigned int* revision, unsigned int* external)
{
    if (physical != PHYSICAL_HANDLE)
        return NVAPI_EXPECTED_PHYSICAL_GPU_HANDLE;
    if (device == NULL || subsystem == NULL || revision == NULL || external == NULL)
        return NVAPI_INVALID_ARGUMENT;
    *device = 0x268410de;
    *subsystem = 0;
    *revision = 0xa1;
    *external = 0x2684;
    return NVAPI_OK;
}

static NvAPI_Status __cdecl GPU_GetGPUType(void* physical, unsigned int* type)
{
    if (physical != PHYSICAL_HANDLE)
        return NVAPI_EXPECTED_PHYSICAL_GPU_HANDLE;
    if (type == NULL)
        return NVAPI_INVALID_ARGUMENT;
    *type = 2; /* NV_SYSTEM_TYPE_DGPU */
    return NVAPI_OK;
}

static NvAPI_Status __cdecl GPU_GetAdapterIdFromPhysicalGpu(void* physical, void* adapter)
{
    if (physical != PHYSICAL_HANDLE)
        return NVAPI_EXPECTED_PHYSICAL_GPU_HANDLE;
    if (adapter == NULL)
        return NVAPI_INVALID_ARGUMENT;
    LUID luid;
    if (!adapter_luid(&luid))
        return NVAPI_NVIDIA_DEVICE_NOT_FOUND;
    memcpy(adapter, &luid, sizeof(luid));
    return NVAPI_OK;
}

typedef struct
{
    unsigned int version;
    void* pOSAdapterId;
    unsigned int physicalGpuCount;
    void* physicalGpuHandles[NVAPI_MAX_PHYSICAL_GPUS];
    unsigned int reserved[8];
} NV_LOGICAL_GPU_DATA;

static NvAPI_Status __cdecl GPU_GetLogicalGpuInfo(void* logical, NV_LOGICAL_GPU_DATA* data)
{
    if (logical != LOGICAL_HANDLE)
        return NVAPI_EXPECTED_LOGICAL_GPU_HANDLE;
    if (data == NULL || data->pOSAdapterId == NULL)
        return NVAPI_INVALID_ARGUMENT;
    if (data->version != NVAPI_VERSION(sizeof(NV_LOGICAL_GPU_DATA), 1))
        return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
    LUID luid;
    if (!adapter_luid(&luid))
        return NVAPI_NVIDIA_DEVICE_NOT_FOUND;
    memcpy(data->pOSAdapterId, &luid, sizeof(luid));
    data->physicalGpuCount = 1;
    data->physicalGpuHandles[0] = PHYSICAL_HANDLE;
    return NVAPI_OK;
}

static const struct
{
    unsigned int id;
    void* function;
} implemented[] = {
    {0x0150e828, (void*)&Initialize},
    {0xd22bdd7e, (void*)&Unload},
    {0x6c2d048c, (void*)&GetErrorMessage},
    {0x01053fa5, (void*)&GetInterfaceVersionString},
    {0x2926aaad, (void*)&SYS_GetDriverAndBranchVersion},
    {0xf951a4d1, (void*)&GetDisplayDriverVersion},
    {0xe5ac921f, (void*)&EnumPhysicalGPUs},
    {0x48b3ea59, (void*)&EnumLogicalGPUs},
    {0xadd604d1, (void*)&GetLogicalGPUFromPhysicalGPU},
    {0xaea3fa32, (void*)&GetPhysicalGPUsFromLogicalGPU},
    {0x6533ea3e, (void*)&GetGPUIDfromPhysicalGPU},
    {0x5380ad1a, (void*)&GetPhysicalGPUFromGPUID},
    {0xd8265d24, (void*)&GPU_GetArchInfo},
    {0xceee8e9f, (void*)&GPU_GetFullName},
    {0x2ddfb66e, (void*)&GPU_GetPCIIdentifiers},
    {0xc33baeb1, (void*)&GPU_GetGPUType},
    {0x0ff07fde, (void*)&GPU_GetAdapterIdFromPhysicalGpu},
    {0x842b066e, (void*)&GPU_GetLogicalGpuInfo},
};

/* fakenvapi's own interfaces: OptiScaler takes an NVAPI that answers them for fakenvapi, that is, for no NVIDIA
   GPU, so they are never forwarded to a chained fakenvapi */
static int fakenvapi_private(unsigned int id)
{
    return id == 0x21372137 || id == 0x21382138 || id == 0x21392139 || id == 0x21402140 || id == 0x21412141 ||
           id == 0x21422142;
}

__declspec(dllexport) void* __cdecl nvapi_QueryInterface(unsigned int id)
{
    ensure_initialized();
    void* function = NULL;
    for (size_t i = 0; i < sizeof(implemented) / sizeof(implemented[0]) && function == NULL; ++i)
        if (implemented[i].id == id)
            function = implemented[i].function;
    const char* source = function != NULL ? "d4r" : "unimplemented";
    if (function == NULL && state.chain != NULL && !fakenvapi_private(id))
    {
        function = state.chain(id);
        source = function != NULL ? "chained" : "unimplemented";
    }
    /* each interface once */
    EnterCriticalSection(&state.lock);
    int seen = 0;
    for (unsigned int i = 0; i < state.logged_count && !seen; ++i)
        seen = state.logged[i] == id;
    if (!seen && state.logged_count < sizeof(state.logged) / sizeof(state.logged[0]))
        state.logged[state.logged_count++] = id;
    LeaveCriticalSection(&state.lock);
    if (!seen)
        nvapi_log("0x%08x %s: %s", id, d4r_nvapi_name(id), source);
    return function;
}
