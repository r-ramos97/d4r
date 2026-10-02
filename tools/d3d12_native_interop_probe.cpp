// Probe for zero-copy D3D12 <-> HIP sharing on native Windows (docs/windows.md, "VRAM interop").
//
// Under Proton d4r keeps DLSS's inputs and output in VRAM through Vulkan interop and synchronises on the GPU
// (tools/d3d12_hip_interop_probe.cpp). On native Windows the shim copies them through host memory, one frame
// late. Whether AMD's Windows HIP can do better is what this asks the driver, through the d4r nvcuda bridge:
//
//   1. memory: a D3D12 buffer in VRAM, shared (CreateSharedHandle) and imported into HIP
//      (d4rImportWin32Memory: hipImportExternalMemory as a D3D12 resource, else an opaque Win32 handle, else a
//      shared heap);
//   2. both directions: D3D12 texture -> shared buffer -> CUDA, and CUDA -> shared buffer -> D3D12 texture;
//   3. GPU-side ordering without the CPU: a shared D3D12 fence imported as a HIP external semaphore
//      (d4rWaitSemaphore / d4rSignalSemaphore), and a marker that D3D12's WriteBufferImmediate writes into the
//      shared buffer and HIP's stream waits for (d4rStreamWaitValue32, the Proton path's mechanism);
//   4. timings: VRAM copies against today's readback copy;
//   5. same-frame results (FrameAge = 0): the GPU-side wait of tools/d4r_d3d12_inline.h inside one D3D12 command
//      list, released by HIP (d4rStreamWriteValue32) after HIP itself waited for a D3D12 marker, and the cost of
//      one spin of the wait.
//
// usage: d4r-interop-probe.exe NVCUDA_BRIDGE_DLL [WIDTH HEIGHT]   (D4R_PROBE_ADAPTER=warp: software D3D12)
// Exit code 0: memory is shared both ways (the VRAM path can be built); 1: it is not; 2: setup failed.
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "d4r_d3d12_inline.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using CUresult = int;
using CUdeviceptr = unsigned long long;

namespace
{
constexpr CUresult CUDA_ERROR_NOT_READY = 600;

ID3D12Device* g_device;
ID3D12CommandQueue* g_queue;
ID3D12CommandAllocator* g_allocator;
ID3D12GraphicsCommandList* g_list;
ID3D12Fence* g_fence;
HANDLE g_event;
UINT64 g_fenceValue;

struct Bridge
{
    using SetEnvFn = int(WINAPI*)(const char*, const char*, int);
    using LoadErrorFn = const char*(WINAPI*)();
    using InitFn = CUresult(WINAPI*)(unsigned int);
    using DeviceGetFn = CUresult(WINAPI*)(int*, int);
    using DeviceNameFn = CUresult(WINAPI*)(char*, int, int);
    using ImportMemoryFn = CUresult(WINAPI*)(void*, uint32_t, uint64_t, CUdeviceptr*, void**);
    using ReleaseFn = CUresult(WINAPI*)(void*);
    using ImportSemaphoreFn = CUresult(WINAPI*)(void*, uint32_t, void**);
    using SemaphoreValueFn = CUresult(WINAPI*)(void*, uint64_t);
    using WaitValueFn = CUresult(WINAPI*)(CUdeviceptr, uint32_t);
    using DtoHFn = CUresult(WINAPI*)(void*, CUdeviceptr, size_t);
    using HtoDFn = CUresult(WINAPI*)(CUdeviceptr, const void*, size_t);
    using DtoDFn = CUresult(WINAPI*)(CUdeviceptr, CUdeviceptr, size_t);
    using MemsetFn = CUresult(WINAPI*)(CUdeviceptr, unsigned int, size_t);
    using MemsetAsyncFn = CUresult(WINAPI*)(CUdeviceptr, unsigned int, size_t, void*);
    using SyncFn = CUresult(WINAPI*)();
    using QueryFn = CUresult(WINAPI*)(void*);
    using AllocFn = CUresult(WINAPI*)(CUdeviceptr*, size_t);
    using FreeFn = CUresult(WINAPI*)(CUdeviceptr);
    using StreamWriteFn = CUresult(WINAPI*)(CUdeviceptr, uint32_t);

    SetEnvFn setEnv;
    LoadErrorFn loadError;
    InitFn init;
    DeviceGetFn deviceGet;
    DeviceNameFn deviceName;
    ImportMemoryFn importMemory;
    ReleaseFn releaseMemory;
    ImportSemaphoreFn importSemaphore;
    SemaphoreValueFn waitSemaphore;
    SemaphoreValueFn signalSemaphore;
    ReleaseFn releaseSemaphore;
    WaitValueFn waitValue;
    WaitValueFn writeValue;
    DtoHFn dtoh;
    HtoDFn htod;
    DtoDFn dtod;
    MemsetFn memset32;
    MemsetAsyncFn memset32Async;
    SyncFn synchronize;
    QueryFn streamQuery;
    AllocFn alloc;
    FreeFn free;
    StreamWriteFn streamWrite; // optional: newer bridges
};

template <typename T> bool resolve(HMODULE module, const char* name, T& function)
{
    function = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(module, name)));
    if (function == nullptr)
        std::printf("  the bridge has no %s (an older d4r nvcuda.dll?)\n", name);
    return function != nullptr;
}

bool check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
        std::printf("  %s failed: 0x%08lx\n", what, static_cast<unsigned long>(hr));
    return SUCCEEDED(hr);
}

bool submit()
{
    if (!check(g_list->Close(), "Close"))
        return false;
    ID3D12CommandList* lists[] = {g_list};
    g_queue->ExecuteCommandLists(1, lists);
    return true;
}

// waits for everything submitted so far; false after timeoutMs
bool wait_queue(DWORD timeoutMs = INFINITE)
{
    g_queue->Signal(g_fence, ++g_fenceValue);
    g_fence->SetEventOnCompletion(g_fenceValue, g_event);
    return WaitForSingleObject(g_event, timeoutMs) == WAIT_OBJECT_0;
}

bool reset_list()
{
    return check(g_allocator->Reset(), "allocator Reset") && check(g_list->Reset(g_allocator, nullptr), "list Reset");
}

bool flush()
{
    return submit() && wait_queue() && reset_list();
}

double elapsed_ms(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 size)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

ID3D12Resource* create_buffer(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state,
                              D3D12_HEAP_FLAGS flags = D3D12_HEAP_FLAG_NONE,
                              D3D12_RESOURCE_FLAGS resourceFlags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc = buffer_desc(size);
    desc.Flags = resourceFlags;
    ID3D12Resource* resource = nullptr;
    if (!check(g_device->CreateCommittedResource(&heap, flags, &desc, state, nullptr, __uuidof(ID3D12Resource),
                                                 reinterpret_cast<void**>(&resource)),
               "CreateCommittedResource(buffer)"))
        return nullptr;
    return resource;
}

void transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    g_list->ResourceBarrier(1, &barrier);
}

uint64_t pattern(uint64_t i)
{
    // finite halves: copies must not canonicalize NaNs
    return ((i * 0x9E3779B97F4A7C15ull) ^ (i >> 7)) & 0x3BFF3BFF3BFF3BFFull;
}

std::string adapter_name(LUID luid)
{
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    DXGI_ADAPTER_DESC1 desc = {};
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory))) &&
        SUCCEEDED(factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter))))
        adapter->GetDesc1(&desc);
    char name[128] = "?";
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
    char line[256];
    std::snprintf(line, sizeof(line), "%s, %llu MB VRAM", name,
                  static_cast<unsigned long long>(desc.DedicatedVideoMemory >> 20));
    if (adapter != nullptr)
        adapter->Release();
    if (factory != nullptr)
        factory->Release();
    return line;
}

const char* verdict(bool ok)
{
    return ok ? "PASS" : "FAIL";
}

bool memory_shared(bool toCuda, bool toD3D12)
{
    return toCuda && toD3D12;
}

// u64 words of a readback buffer that differ from `expected`; all of them if it cannot be mapped
size_t words_in(ID3D12Resource* readback, UINT64 bytes, uint64_t expected)
{
    void* mapped = nullptr;
    D3D12_RANGE range = {0, static_cast<SIZE_T>(bytes)};
    if (FAILED(readback->Map(0, &range, &mapped)))
        return static_cast<size_t>(bytes / 8);
    size_t wrong = 0;
    const uint64_t* words = static_cast<const uint64_t*>(mapped);
    for (UINT64 i = 0; i < bytes / 8; ++i)
        wrong += words[i] != expected;
    D3D12_RANGE none = {0, 0};
    readback->Unmap(0, &none);
    return wrong;
}

// polls the null stream until its work is done; false after timeoutMs
bool wait_stream(const Bridge& cuda, DWORD timeoutMs)
{
    const auto start = std::chrono::steady_clock::now();
    for (;;)
    {
        const CUresult result = cuda.streamQuery(nullptr);
        if (result != CUDA_ERROR_NOT_READY)
            return result == 0;
        if (elapsed_ms(start) > timeoutMs)
            return false;
        Sleep(1);
    }
}
} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2)
    {
        std::printf("usage: %s NVCUDA_BRIDGE_DLL [WIDTH HEIGHT]\n", argv[0]);
        return 2;
    }
    const UINT width = argc > 3 ? static_cast<UINT>(std::strtoul(argv[2], nullptr, 0)) : 2560;
    const UINT height = argc > 3 ? static_cast<UINT>(std::strtoul(argv[3], nullptr, 0)) : 1440;
    const UINT64 texel = 8; // R16G16B16A16_FLOAT, the DLSS output format
    const UINT64 pitch = width * texel;
    const UINT64 bytes = pitch * height;
    const UINT64 words = bytes / 8;
    const UINT64 markerOffset = bytes;              // a u32 after the image, for the marker test
    const UINT64 sharedBytes = bytes + 65536;
    if (width == 0 || height == 0 || pitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT != 0)
    {
        std::printf("the width must be a multiple of 32\n");
        return 2;
    }

    std::printf("d4r D3D12 <-> HIP interop probe, %ux%u RGBA16F (%.1f MB)\n\n", width, height, bytes / 1048576.0);

    // the game's device: the default adapter, as D3D12CreateDevice(nullptr) picks it (D4R_PROBE_ADAPTER=warp:
    // Windows' software renderer, for tests on machines without a GPU)
    IDXGIAdapter* adapter = nullptr;
    const char* adapterChoice = std::getenv("D4R_PROBE_ADAPTER");
    if (adapterChoice != nullptr && std::strcmp(adapterChoice, "warp") == 0)
    {
        IDXGIFactory4* factory = nullptr;
        if (!check(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory)),
                   "CreateDXGIFactory1") ||
            !check(factory->EnumWarpAdapter(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&adapter)),
                   "EnumWarpAdapter"))
            return 2;
        factory->Release();
    }
    if (!check(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device),
                                 reinterpret_cast<void**>(&g_device)),
               "D3D12CreateDevice"))
        return 2;
    const LUID luid = g_device->GetAdapterLuid();
    std::printf("D3D12 adapter: %s, LUID %08lx:%08lx\n", adapter_name(luid).c_str(),
                static_cast<unsigned long>(luid.HighPart), static_cast<unsigned long>(luid.LowPart));
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!check(g_device->CreateCommandQueue(&queueDesc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&g_queue)),
               "CreateCommandQueue") ||
        !check(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                                reinterpret_cast<void**>(&g_allocator)),
               "CreateCommandAllocator") ||
        !check(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocator, nullptr,
                                           __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&g_list)),
               "CreateCommandList") ||
        !check(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&g_fence)),
               "CreateFence"))
        return 2;
    g_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    // the bridge, on the GPU of this D3D12 device (as the shim sets it up for NGX)
    HMODULE bridge = LoadLibraryExA(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (bridge == nullptr)
    {
        std::printf("LoadLibrary(%s) failed: %lu\n", argv[1], GetLastError());
        return 2;
    }
    Bridge cuda = {};
    bool exports = resolve(bridge, "d4rSetEnv", cuda.setEnv) && resolve(bridge, "d4rLoadError", cuda.loadError) &&
                   resolve(bridge, "cuInit", cuda.init) && resolve(bridge, "cuDeviceGet", cuda.deviceGet) &&
                   resolve(bridge, "cuDeviceGetName", cuda.deviceName) &&
                   resolve(bridge, "d4rImportWin32Memory", cuda.importMemory) &&
                   resolve(bridge, "d4rReleaseVulkanMemory", cuda.releaseMemory) &&
                   resolve(bridge, "d4rImportWin32Semaphore", cuda.importSemaphore) &&
                   resolve(bridge, "d4rWaitSemaphore", cuda.waitSemaphore) &&
                   resolve(bridge, "d4rSignalSemaphore", cuda.signalSemaphore) &&
                   resolve(bridge, "d4rReleaseSemaphore", cuda.releaseSemaphore) &&
                   resolve(bridge, "d4rStreamWaitValue32", cuda.waitValue) &&
                   resolve(bridge, "d4rWriteValue32", cuda.writeValue) &&
                   resolve(bridge, "cuMemcpyDtoH", cuda.dtoh) && resolve(bridge, "cuMemcpyHtoD", cuda.htod) &&
                   resolve(bridge, "cuMemcpyDtoD", cuda.dtod) && resolve(bridge, "cuMemsetD32", cuda.memset32) &&
                   resolve(bridge, "cuMemsetD32Async", cuda.memset32Async) &&
                   resolve(bridge, "d4rCtxSynchronize", cuda.synchronize) &&
                   resolve(bridge, "cuStreamQuery", cuda.streamQuery) && resolve(bridge, "cuMemAlloc", cuda.alloc) &&
                   resolve(bridge, "cuMemFree", cuda.free);
    if (!exports)
        return 2;
    cuda.streamWrite = reinterpret_cast<Bridge::StreamWriteFn>(
        reinterpret_cast<void*>(GetProcAddress(bridge, "d4rStreamWriteValue32")));
    char value[24];
    std::snprintf(value, sizeof(value), "0x%08lx", static_cast<unsigned long>(luid.LowPart));
    cuda.setEnv("D4R_CUDA_LUID_LOW", value, 1);
    std::snprintf(value, sizeof(value), "0x%08lx", static_cast<unsigned long>(luid.HighPart));
    cuda.setEnv("D4R_CUDA_LUID_HIGH", value, 1);
    CUresult result = cuda.init(0);
    if (result != 0)
    {
        std::printf("cuInit through the bridge failed (%d): %s\n", result, cuda.loadError());
        return 2;
    }
    int cudaDevice = -1;
    char cudaName[256] = "?";
    cuda.deviceGet(&cudaDevice, 0);
    cuda.deviceName(cudaName, sizeof(cudaName), cudaDevice);
    std::printf("CUDA (ZLUDA) device: HIP device %d, %s\n\n", cudaDevice, cudaName);

    // 1. memory
    std::printf("1. Sharing a D3D12 buffer in VRAM with HIP\n");
    ID3D12Resource* shared = nullptr;
    ID3D12Heap* sharedHeap = nullptr;
    HANDLE handle = nullptr;
    CUdeviceptr device = 0;
    void* external = nullptr;
    const char* importKind = nullptr;
    {
        shared = create_buffer(D3D12_HEAP_TYPE_DEFAULT, sharedBytes, D3D12_RESOURCE_STATE_COMMON, D3D12_HEAP_FLAG_SHARED);
        if (shared != nullptr && check(g_device->CreateSharedHandle(shared, nullptr, GENERIC_ALL, nullptr, &handle),
                                       "CreateSharedHandle(resource)"))
        {
            const D3D12_RESOURCE_DESC desc = buffer_desc(sharedBytes);
            const UINT64 allocation = g_device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
            result = cuda.importMemory(handle, 5, allocation, &device, &external);
            std::printf("  as a D3D12 resource (hipExternalMemoryHandleTypeD3D12Resource): %s (%d)\n",
                        result == 0 ? "imported" : "failed", result);
            if (result == 0)
                importKind = "D3D12 resource";
            else
            {
                result = cuda.importMemory(handle, 2, allocation, &device, &external);
                std::printf("  as an opaque Win32 handle (hipExternalMemoryHandleTypeOpaqueWin32): %s (%d)\n",
                            result == 0 ? "imported" : "failed", result);
                if (result == 0)
                    importKind = "opaque Win32 handle";
            }
            if (importKind == nullptr)
            {
                CloseHandle(handle);
                handle = nullptr;
                shared->Release();
                shared = nullptr;
            }
        }
        if (importKind == nullptr)
        {
            // a shared heap with the buffer placed in it
            D3D12_HEAP_DESC heapDesc = {};
            heapDesc.SizeInBytes = (sharedBytes + 65535) / 65536 * 65536;
            heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            heapDesc.Flags = static_cast<D3D12_HEAP_FLAGS>(D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS);
            const D3D12_RESOURCE_DESC desc = buffer_desc(sharedBytes);
            if (check(g_device->CreateHeap(&heapDesc, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&sharedHeap)),
                      "CreateHeap(shared)") &&
                check(g_device->CreatePlacedResource(sharedHeap, 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     __uuidof(ID3D12Resource), reinterpret_cast<void**>(&shared)),
                      "CreatePlacedResource") &&
                check(g_device->CreateSharedHandle(sharedHeap, nullptr, GENERIC_ALL, nullptr, &handle),
                      "CreateSharedHandle(heap)"))
            {
                result = cuda.importMemory(handle, 4, heapDesc.SizeInBytes, &device, &external);
                std::printf("  as a D3D12 heap (hipExternalMemoryHandleTypeD3D12Heap): %s (%d)\n",
                            result == 0 ? "imported" : "failed", result);
                if (result == 0)
                    importKind = "D3D12 heap";
            }
        }
    }
    if (importKind == nullptr)
    {
        std::printf("\nRESULT: AMD's HIP on this driver cannot import D3D12 memory; d4r keeps copying through host "
                    "memory (works, one frame late). Please report this output.\n");
        return 1;
    }
    std::printf("  shared buffer at CUDA address 0x%llx (%s)\n\n", device, importKind);

    // a game-like texture, filled through an upload buffer
    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* texture = nullptr;
    ID3D12Resource* upload = create_buffer(D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource* readback = create_buffer(D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!check(g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource),
                                                 reinterpret_cast<void**>(&texture)),
               "CreateCommittedResource(texture)") ||
        upload == nullptr || readback == nullptr)
        return 2;
    uint64_t* mapped = nullptr;
    D3D12_RANGE none = {0, 0};
    if (!check(upload->Map(0, &none, reinterpret_cast<void**>(&mapped)), "Map upload"))
        return 2;
    for (UINT64 i = 0; i < words; ++i)
        mapped[i] = pattern(i);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    footprint.Footprint.Format = textureDesc.Format;
    footprint.Footprint.Width = width;
    footprint.Footprint.Height = height;
    footprint.Footprint.Depth = 1;
    footprint.Footprint.RowPitch = static_cast<UINT>(pitch);
    D3D12_TEXTURE_COPY_LOCATION textureLocation = {};
    textureLocation.pResource = texture;
    textureLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION uploadLocation = {};
    uploadLocation.pResource = upload;
    uploadLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    uploadLocation.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION readbackLocation = uploadLocation;
    readbackLocation.pResource = readback;
    D3D12_TEXTURE_COPY_LOCATION sharedLocation = uploadLocation;
    sharedLocation.pResource = shared;
    g_list->CopyTextureRegion(&textureLocation, 0, 0, 0, &uploadLocation, nullptr);
    transition(texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (!flush())
        return 2;

    // 2. both directions (buffers go from COMMON to the copy states implicitly and back after each submission)
    std::printf("2. Data through the shared buffer\n");
    g_list->CopyTextureRegion(&sharedLocation, 0, 0, 0, &textureLocation, nullptr);
    if (!flush())
        return 2;
    std::vector<uint64_t> host(words);
    result = cuda.dtoh(host.data(), device, bytes);
    size_t mismatches = 0;
    for (UINT64 i = 0; i < words; ++i)
        mismatches += host[i] != pattern(i);
    const bool toCuda = result == 0 && mismatches == 0;
    std::printf("  D3D12 texture -> shared buffer -> CUDA: %s (result %d, %zu of %llu texels differ)\n",
                verdict(toCuda), result, mismatches, static_cast<unsigned long long>(words));

    result = cuda.memset32(device, 0x3C003800u, bytes / 4);
    if (result == 0)
        result = cuda.synchronize();
    transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    g_list->CopyTextureRegion(&textureLocation, 0, 0, 0, &sharedLocation, nullptr);
    transition(texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g_list->CopyTextureRegion(&readbackLocation, 0, 0, 0, &textureLocation, nullptr);
    if (!flush())
        return 2;
    uint64_t* back = nullptr;
    D3D12_RANGE all = {0, static_cast<SIZE_T>(bytes)};
    size_t reverse = words;
    if (SUCCEEDED(readback->Map(0, &all, reinterpret_cast<void**>(&back))))
    {
        reverse = 0;
        for (UINT64 i = 0; i < words; ++i)
            reverse += back[i] != 0x3C0038003C003800ull;
        readback->Unmap(0, &none);
    }
    const bool toD3D12 = result == 0 && reverse == 0;
    std::printf("  CUDA -> shared buffer -> D3D12 texture: %s (result %d, %zu of %llu texels differ)\n\n",
                verdict(toD3D12), result, reverse, static_cast<unsigned long long>(words));

    // 3. GPU-side ordering
    std::printf("3. Ordering D3D12 and HIP work on the GPU, without the CPU\n");
    ID3D12Fence* gate = nullptr;    // holds the D3D12 queue until the CPU has checked that HIP waits
    ID3D12Fence* sharedFence = nullptr;
    HANDLE fenceHandle = nullptr;
    void* semaphore = nullptr;
    bool fenceSync = false, markerSync = false;
    bool hung = false;
    const uint32_t fill = 0x3E003D00u;
    const uint64_t fillWord = (static_cast<uint64_t>(fill) << 32) | fill;
    if (check(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&gate)),
              "CreateFence(gate)") &&
        check(g_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence),
                                    reinterpret_cast<void**>(&sharedFence)),
              "CreateFence(shared)") &&
        check(g_device->CreateSharedHandle(sharedFence, nullptr, GENERIC_ALL, nullptr, &fenceHandle),
              "CreateSharedHandle(fence)"))
    {
        result = cuda.importSemaphore(fenceHandle, 4, &semaphore);
        std::printf("  D3D12 fence as a HIP external semaphore: %s (%d)\n", result == 0 ? "imported" : "failed",
                    result);
    }
    if (semaphore != nullptr)
    {
        // HIP: wait for D3D12's copy (fence 1), overwrite the buffer, signal D3D12 (fence 2)
        CUresult hip = cuda.waitSemaphore(semaphore, 1);
        if (hip == 0)
            hip = cuda.memset32Async(device, fill, bytes / 4, nullptr);
        if (hip == 0)
            hip = cuda.signalSemaphore(semaphore, 2);
        // D3D12: after the gate, copy the pattern into the buffer, signal 1; wait for 2, read the buffer back (a
        // second allocator: the first one's list is still pending behind the gate)
        ID3D12CommandAllocator* allocator2 = nullptr;
        ID3D12GraphicsCommandList* readList = nullptr;
        if (!check(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                                    reinterpret_cast<void**>(&allocator2)),
                   "CreateCommandAllocator") ||
            !check(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator2, nullptr,
                                               __uuidof(ID3D12GraphicsCommandList),
                                               reinterpret_cast<void**>(&readList)),
                   "CreateCommandList"))
            return 2;
        readList->CopyTextureRegion(&readbackLocation, 0, 0, 0, &sharedLocation, nullptr);
        readList->Close();
        g_queue->Wait(gate, 1);
        g_list->CopyTextureRegion(&sharedLocation, 0, 0, 0, &textureLocation, nullptr);
        submit();
        g_queue->Signal(sharedFence, 1);
        g_queue->Wait(sharedFence, 2);
        ID3D12CommandList* readLists[] = {readList};
        g_queue->ExecuteCommandLists(1, readLists);
        Sleep(200);
        const CUresult early = cuda.streamQuery(nullptr);
        const auto start = std::chrono::steady_clock::now();
        gate->Signal(1);
        const bool done = hip == 0 && wait_queue(10000);
        const double roundTrip = elapsed_ms(start);
        if (!done)
        {
            hung = true;
            sharedFence->Signal(2); // release the D3D12 queue
            wait_queue(10000);
        }
        reset_list();
        size_t wrong = words;
        if (done && SUCCEEDED(readback->Map(0, &all, reinterpret_cast<void**>(&back))))
        {
            wrong = 0;
            for (UINT64 i = 0; i < words; ++i)
                wrong += back[i] != fillWord;
            readback->Unmap(0, &none);
        }
        fenceSync = done && early == CUDA_ERROR_NOT_READY && wrong == 0;
        std::printf("  HIP waited for D3D12 before the gate opened: %s (stream query %d)\n",
                    verdict(early == CUDA_ERROR_NOT_READY), early);
        if (!done)
            std::printf("  round trip D3D12 -> HIP -> D3D12: FAIL (no result within 10 s; HIP result %d)\n", hip);
        else
            std::printf("  round trip D3D12 -> HIP -> D3D12: %s (%zu of %llu texels wrong, %.3f ms)\n",
                        verdict(wrong == 0), wrong, static_cast<unsigned long long>(words), roundTrip);
    }
    std::printf("  shared fence sync: %s\n", fenceSync ? "PASS" : semaphore == nullptr ? "UNSUPPORTED" : "FAIL");

    // the marker: D3D12 writes a u32 into the shared buffer, HIP's stream waits for it
    ID3D12GraphicsCommandList2* list2 = nullptr;
    if (!hung && check(g_list->QueryInterface(__uuidof(ID3D12GraphicsCommandList2), reinterpret_cast<void**>(&list2)),
                       "QueryInterface(ID3D12GraphicsCommandList2)"))
    {
        const CUdeviceptr marker = device + markerOffset;
        CUresult hip = cuda.memset32(marker, 0, 1);
        if (hip == 0)
            hip = cuda.synchronize();
        if (hip == 0)
            hip = cuda.waitValue(marker, 5);
        if (hip == 0)
            hip = cuda.memset32Async(device, fill ^ 0x01000100u, bytes / 4, nullptr);
        g_queue->Wait(gate, 2);
        transition(shared, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_WRITEBUFFERIMMEDIATE_PARAMETER parameter = {shared->GetGPUVirtualAddress() + markerOffset, 5};
        D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
        list2->WriteBufferImmediate(1, &parameter, &mode);
        transition(shared, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        submit();
        Sleep(200);
        const CUresult early = cuda.streamQuery(nullptr);
        const auto start = std::chrono::steady_clock::now();
        gate->Signal(2);
        const bool done = hip == 0 && wait_stream(cuda, 10000);
        const double latency = elapsed_ms(start);
        if (!done && hip == 0)
        {
            hung = true;
            cuda.writeValue(marker, 5); // release HIP's stream
            wait_stream(cuda, 10000);
        }
        wait_queue(10000);
        reset_list();
        markerSync = done && early == CUDA_ERROR_NOT_READY;
        std::printf("  HIP stream waited for the marker before D3D12 wrote it: %s (stream query %d)\n",
                    verdict(early == CUDA_ERROR_NOT_READY), early);
        std::printf("  D3D12 WriteBufferImmediate -> HIP stream wait: %s (result %d, %.3f ms)\n", verdict(done), hip,
                    latency);
        list2->Release();
    }
    std::printf("  marker sync: %s\n\n", verdict(markerSync));

    // 4. timings: what the VRAM path would cost against today's copies through host memory
    std::printf("4. Timings, best of 10 (submit and wait included)\n");
    double toShared = 1e9, fromShared = 1e9, toReadback = 1e9, cudaCopy = 1e9, cudaToHost = 1e9;
    for (int i = 0; i < 10 && !hung; ++i)
    {
        auto start = std::chrono::steady_clock::now();
        g_list->CopyTextureRegion(&sharedLocation, 0, 0, 0, &textureLocation, nullptr);
        flush();
        toShared = std::min(toShared, elapsed_ms(start));

        transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        flush();
        start = std::chrono::steady_clock::now();
        g_list->CopyTextureRegion(&textureLocation, 0, 0, 0, &sharedLocation, nullptr);
        flush();
        fromShared = std::min(fromShared, elapsed_ms(start));
        transition(texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        flush();

        start = std::chrono::steady_clock::now();
        g_list->CopyTextureRegion(&readbackLocation, 0, 0, 0, &textureLocation, nullptr);
        flush();
        toReadback = std::min(toReadback, elapsed_ms(start));
    }
    CUdeviceptr local = 0;
    if (!hung && cuda.alloc(&local, bytes) == 0)
    {
        for (int i = 0; i < 10; ++i)
        {
            cuda.synchronize();
            auto start = std::chrono::steady_clock::now();
            cuda.dtod(local, device, bytes);
            cuda.synchronize();
            cudaCopy = std::min(cudaCopy, elapsed_ms(start));
            start = std::chrono::steady_clock::now();
            cuda.dtoh(host.data(), local, bytes);
            cudaToHost = std::min(cudaToHost, elapsed_ms(start));
        }
        cuda.free(local);
    }
    if (!hung)
    {
        std::printf("  D3D12 texture -> shared VRAM buffer:   %8.3f ms\n", toShared);
        std::printf("  shared VRAM buffer -> D3D12 texture:   %8.3f ms\n", fromShared);
        std::printf("  CUDA copy out of the shared buffer:    %8.3f ms\n", cudaCopy);
        std::printf("  D3D12 texture -> readback (today):     %8.3f ms\n", toReadback);
        std::printf("  CUDA device -> host (today):           %8.3f ms\n\n", cudaToHost);
    }

    // 5. same-frame results: one command list waits on the GPU for HIP, as d4r's FrameAge = 0 does
    std::printf("5. Same-frame results (FrameAge = 0): a D3D12 command list waits on the GPU for HIP\n");
    bool sameFrame = false;
    double nsPerSpin = -1.0;
    d4r_inline::Presenter presenter;
    std::string presenterError;
    struct SharedBuffer
    {
        ID3D12Resource* resource = nullptr;
        CUdeviceptr device = 0;
        void* external = nullptr;
    };
    auto make_shared = [&](UINT64 size, SharedBuffer& out) {
        out.resource = create_buffer(D3D12_HEAP_TYPE_DEFAULT, size, D3D12_RESOURCE_STATE_COMMON,
                                     D3D12_HEAP_FLAG_SHARED, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        HANDLE shared = nullptr;
        if (out.resource == nullptr ||
            FAILED(g_device->CreateSharedHandle(out.resource, nullptr, GENERIC_ALL, nullptr, &shared)))
            return false;
        const D3D12_RESOURCE_DESC desc = out.resource->GetDesc();
        const UINT64 allocation = g_device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        const CUresult imported = cuda.importMemory(shared, 5, allocation, &out.device, &out.external);
        CloseHandle(shared);
        return imported == 0;
    };
    SharedBuffer status, slots[d4r_inline::kSlots], present;
    if (hung || !memory_shared(toCuda, toD3D12))
        std::printf("  skipped: needs VRAM sharing\n");
    else if (cuda.streamWrite == nullptr)
        std::printf("  skipped: this nvcuda.dll has no d4rStreamWriteValue32\n");
    else if (!presenter.init(g_device, presenterError))
        std::printf("  the wait's shaders: FAIL (%s)\n", presenterError.c_str());
    else
    {
        bool ok = make_shared(d4r_inline::kStatusBytes, status) && make_shared(bytes, present);
        for (SharedBuffer& slot : slots)
            ok = ok && make_shared(bytes, slot);
        ID3D12Resource* slotResources[d4r_inline::kSlots];
        for (int slot = 0; slot < d4r_inline::kSlots; ++slot)
            slotResources[slot] = slots[slot].resource;
        ID3D12Resource* statusReadback = create_buffer(D3D12_HEAP_TYPE_READBACK, d4r_inline::kStatusBytes,
                                                       D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12GraphicsCommandList2* waitList = nullptr;
        ok = ok && statusReadback != nullptr &&
             SUCCEEDED(g_list->QueryInterface(__uuidof(ID3D12GraphicsCommandList2), reinterpret_cast<void**>(&waitList)));
        CUresult hip = ok ? cuda.memset32(status.device, 0, d4r_inline::kStatusBytes / 4) : -1;
        for (SharedBuffer& slot : slots)
            if (hip == 0)
                hip = cuda.memset32(slot.device, 0, bytes / 4);
        if (hip == 0)
            hip = cuda.synchronize();
        // HIP: wait for D3D12's marker (status word 8), write slot 0, release frame 7
        const uint32_t frame = 7;
        const CUdeviceptr gate = status.device + 32;
        if (hip == 0)
            hip = cuda.waitValue(gate, 1);
        if (hip == 0)
            hip = cuda.memset32Async(slots[0].device, fill, bytes / 4, nullptr);
        if (hip == 0)
            hip = cuda.streamWrite(status.device + 4 * (d4r_inline::kProduced + 0), frame);
        if (hip == 0)
            hip = cuda.streamWrite(status.device + 4 * d4r_inline::kReleased, frame);
        if (ok && hip == 0)
        {
            // D3D12: the marker, then the wait and the copy into the present buffer, as the shim records them
            transition(status.resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_WRITEBUFFERIMMEDIATE_PARAMETER parameter = {status.resource->GetGPUVirtualAddress() + 32, 1};
            D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
            waitList->WriteBufferImmediate(1, &parameter, &mode);
            transition(status.resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            presenter.record(g_list, status.resource, slotResources, present.resource, frame, bytes, 50000000);
            g_list->CopyBufferRegion(readback, 0, present.resource, 0, bytes);
            transition(present.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            g_list->CopyBufferRegion(statusReadback, 0, status.resource, 0, d4r_inline::kStatusBytes);
            const auto start = std::chrono::steady_clock::now();
            const bool done = submit() && wait_queue(10000);
            const double roundTrip = elapsed_ms(start);
            if (!done)
            {
                hung = true;
                cuda.writeValue(gate, 1);
                cuda.writeValue(status.device + 4 * d4r_inline::kReleased, frame);
                wait_queue(10000);
            }
            reset_list();
            uint32_t statusWords[d4r_inline::kStatusWords] = {};
            size_t wrong = words_in(readback, bytes, fillWord);
            void* mappedStatus = nullptr;
            D3D12_RANGE statusRange = {0, static_cast<SIZE_T>(d4r_inline::kStatusBytes)};
            if (SUCCEEDED(statusReadback->Map(0, &statusRange, &mappedStatus)))
            {
                std::memcpy(statusWords, mappedStatus, sizeof(statusWords));
                statusReadback->Unmap(0, &none);
            }
            sameFrame = done && wrong == 0 && statusWords[d4r_inline::kChosen] == 0 && statusWords[d4r_inline::kSpins] < 50000000;
            std::printf("  HIP released the wait, D3D12 copied HIP's result: %s (slot %d, %u spins, %zu of %llu texels "
                        "wrong, %.3f ms)\n", verdict(sameFrame), static_cast<int>(statusWords[d4r_inline::kChosen]),
                        statusWords[d4r_inline::kSpins], wrong, static_cast<unsigned long long>(words), roundTrip);

            // the cost of one spin: a wait for a frame nobody releases, up to a fixed number of spins
            if (!hung)
            {
                const uint32_t spins = 1000000;
                presenter.record(g_list, status.resource, slotResources, present.resource, frame + 1, 16, spins);
                transition(present.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                const auto spinStart = std::chrono::steady_clock::now();
                if (submit() && wait_queue(10000))
                    nsPerSpin = elapsed_ms(spinStart) * 1e6 / spins;
                reset_list();
                if (nsPerSpin > 0)
                    std::printf("  one spin of the wait: %.1f ns; d4r's limit (D4R_SHIM_INLINE_SPINS, 2000000 spins) "
                                "ends a lost frame's wait after %.0f ms\n", nsPerSpin, nsPerSpin * 2000000 / 1e6);
            }
        }
        else
            std::printf("  setting it up failed (D3D12 %s, HIP result %d)\n", ok ? "ok" : "failed", hip);
        if (waitList != nullptr)
            waitList->Release();
    }
    std::printf("  same-frame wait: %s\n\n", verdict(sameFrame));

    const bool memory = memory_shared(toCuda, toD3D12);
    std::printf("RESULT: VRAM sharing %s (%s), GPU sync: shared fence %s, marker %s, same-frame wait %s\n",
                memory ? "works" : "FAILS", importKind, fenceSync ? "works" : "no", markerSync ? "works" : "no",
                sameFrame ? "works" : "no");
    if (memory)
        std::printf("VRAM sharing works on this PC: d4r keeps DLSS's inputs and output in video memory "
                    "(VramInterop in d4r\\d4r.ini).\n");
    else
        std::printf("d4r keeps copying through host memory on this PC.\n");
    if (sameFrame)
        std::printf("Same-frame results work on this PC: FrameAge = 0 in d4r\\d4r.ini shows each frame's own DLSS "
                    "result.\n");
    else
        std::printf("Same-frame results do not work here: keep FrameAge = 1 or more in d4r\\d4r.ini.\n");
    std::printf("Please report this output (docs/windows.md).\n");
    std::fflush(stdout);
    // a wait that never returned may still hold a GPU queue; skip the teardown that would wait for it
    if (hung)
        TerminateProcess(GetCurrentProcess(), memory ? 0 : 1);
    if (semaphore != nullptr)
        cuda.releaseSemaphore(semaphore);
    cuda.releaseMemory(external);
    if (fenceHandle != nullptr)
        CloseHandle(fenceHandle);
    if (handle != nullptr)
        CloseHandle(handle);
    return memory ? 0 : 1;
}
