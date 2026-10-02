/* The same-frame wait of tools/d4r_d3d12_inline.h on a real D3D12 runtime: Windows' software renderer (WARP),
   with the status values HIP would write set beforehand. It checks which output slot the wait picks, that the
   copy moves that slot's bytes, that an unreleased frame gives up after the spin limit (falling back to the
   newest older result), and that nothing is copied before any result exists.
   usage: inline_test.exe */
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include "d4r_d3d12_inline.h"
#include <dxgi1_4.h>

#include <cstdio>
#include <vector>

using namespace d4r_inline;

static int failures;
static ID3D12Device* device;
static ID3D12CommandQueue* queue;
static ID3D12CommandAllocator* allocator;
static ID3D12GraphicsCommandList* list;
static ID3D12Fence* fence;
static UINT64 fenceValue;

static void check(bool condition, const char* name)
{
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    failures += !condition;
}

static ID3D12Resource* buffer(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (type == D3D12_HEAP_TYPE_DEFAULT)
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* resource = nullptr;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, __uuidof(ID3D12Resource),
                                    reinterpret_cast<void**>(&resource));
    return resource;
}

static bool run()
{
    if (FAILED(list->Close()))
        return false;
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence, ++fenceValue);
    HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(fenceValue, event);
    const bool done = WaitForSingleObject(event, 20000) == WAIT_OBJECT_0;
    CloseHandle(event);
    return done && SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator, nullptr));
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter* warp = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory))) ||
        FAILED(factory->EnumWarpAdapter(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&warp))) ||
        FAILED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device))))
    {
        std::printf("SKIP no D3D12 WARP device\n");
        return 3;
    }
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    device->CreateCommandQueue(&queueDesc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                   reinterpret_cast<void**>(&allocator));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, __uuidof(ID3D12GraphicsCommandList),
                              reinterpret_cast<void**>(&list));
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence));

    Presenter presenter;
    std::string error;
    check(presenter.init(device, error), "shaders compile and pipelines build");
    if (!error.empty())
        std::printf("  %s\n", error.c_str());
    if (presenter.root == nullptr)
        return 1;

    const UINT64 bytes = 4096 * 16; // a 4096-texel RGBA16F row... as 16-byte vectors
    ID3D12Resource* status = buffer(D3D12_HEAP_TYPE_DEFAULT, kStatusBytes, D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource* slots[kSlots];
    for (ID3D12Resource*& slot : slots)
        slot = buffer(D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource* present = buffer(D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COMMON);
    const UINT64 uploadBytes = kStatusBytes + (kSlots + 1) * bytes;
    ID3D12Resource* upload = buffer(D3D12_HEAP_TYPE_UPLOAD, uploadBytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource* readback = buffer(D3D12_HEAP_TYPE_READBACK, kStatusBytes + bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    uint8_t* up = nullptr;
    D3D12_RANGE none = {0, 0};
    upload->Map(0, &none, reinterpret_cast<void**>(&up));

    // every slot (and the present buffer) holds its own byte pattern
    auto fill = [&](int index, uint8_t value) { std::memset(up + kStatusBytes + index * bytes, value, bytes); };
    for (int slot = 0; slot < kSlots; ++slot)
        fill(slot, static_cast<uint8_t>(0x10 + slot));
    fill(kSlots, 0xab);

    struct Case
    {
        const char* name;
        uint32_t frame, released, produced[kSlots], maxSpins;
        uint32_t chosen;   // expected
        bool spunOut;      // expected: the spin limit was reached
    } cases[] = {
        {"released frame: its own slot, no spinning", 10, 10, {7, 8, 9, 10}, 100000, 3, false},
        {"unreleased frame: gives up after the spin limit, newest older result", 10, 9, {7, 8, 9, 0}, 2000, 2, true},
        {"a slot newer than the frame is never shown", 10, 10, {11, 10, 9, 8}, 100000, 1, false},
        {"frame numbers wrap around", 2, 2, {0xfffffffeu, 0xffffffffu, 1, 2}, 100000, 3, false},
        {"no result yet: nothing copied", 1, 0, {0, 0, 0, 0}, 500, kNoSlot, true},
    };
    for (const Case& test : cases)
    {
        uint32_t words[kStatusWords] = {};
        words[kReleased] = test.released;
        for (int slot = 0; slot < kSlots; ++slot)
            words[kProduced + slot] = test.produced[slot];
        std::memset(up, 0, kStatusBytes);
        std::memcpy(up, words, sizeof(words));
        list->CopyBufferRegion(status, 0, upload, 0, kStatusBytes);
        for (int slot = 0; slot < kSlots; ++slot)
            list->CopyBufferRegion(slots[slot], 0, upload, kStatusBytes + slot * bytes, bytes);
        list->CopyBufferRegion(present, 0, upload, kStatusBytes + kSlots * bytes, bytes);
        if (!run())
        {
            check(false, "setup runs");
            return 1;
        }
        presenter.record(list, status, slots, present, test.frame, bytes, test.maxSpins);
        list->CopyBufferRegion(readback, kStatusBytes, present, 0, bytes);
        D3D12_RESOURCE_BARRIER back =
            Presenter::transition(present, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        list->ResourceBarrier(1, &back);
        if (!run())
        {
            check(false, test.name);
            continue;
        }
        list->CopyBufferRegion(readback, 0, status, 0, kStatusBytes);
        run();
        uint8_t* down = nullptr;
        D3D12_RANGE all = {0, static_cast<SIZE_T>(kStatusBytes + bytes)};
        readback->Map(0, &all, reinterpret_cast<void**>(&down));
        uint32_t result[kStatusWords];
        std::memcpy(result, down, sizeof(result));
        const uint8_t expected = test.chosen == kNoSlot ? 0xab : static_cast<uint8_t>(0x10 + test.chosen);
        size_t wrong = 0;
        for (UINT64 i = 0; i < bytes; ++i)
            wrong += down[kStatusBytes + i] != expected;
        readback->Unmap(0, &none);
        const bool spunOut = result[kSpins] == test.maxSpins;
        std::printf("  frame %u: chosen %d, spins %u, %zu bytes differ\n", test.frame, static_cast<int>(result[kChosen]),
                    result[kSpins], wrong);
        check(result[kChosen] == test.chosen && spunOut == test.spunOut && wrong == 0, test.name);
    }
    std::printf("%d failure(s)\n", failures);
    return failures != 0;
}
