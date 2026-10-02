// Same-frame DLSS results on native Windows (docs/windows.md, "Same-frame results"): a bounded GPU-side wait
// inside the game's own D3D12 command list.
//
// Under Proton the d4r vkd3d-proton patch splits the game's command list at the DLSS call and submits the rest
// once DLSS is done. AMD's D3D12 driver cannot split a command list, so on Windows the list waits on the GPU
// instead: after the input copies, two compute dispatches on the game's command list
//   - wait_main (one thread) spins, with atomic loads, until the "released" value in a status buffer that HIP
//     writes after DLSS (d4rStreamWriteValue32) reaches this frame, or a spin limit passes, then picks the output
//     slot holding the newest result not newer than this frame (normally this frame's own);
//   - copy_main copies that slot into a present buffer, which the caller copies into the game's output texture.
// HIP runs on its own hardware queues, so it makes progress while the graphics queue spins; the spin limit keeps
// a lost frame from stalling the GPU for long (and far from Windows' 2-second GPU timeout).
//
// The shaders are HLSL compiled at runtime by d3dcompiler_47.dll (part of Windows), with their root signature in
// the HLSL, so nothing but d3d12.dll's device is needed. Recording them changes the command list's compute root
// signature and pipeline state, which a DLSS evaluation may change too (applications restore them afterwards).
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace d4r_inline
{
constexpr int kSlots = 4;            // output slots, as the shim's kOutputSlots
constexpr uint32_t kNoSlot = 0xffffffffu;

// status buffer, in u32s
constexpr uint32_t kReleased = 0;    // newest frame whose wait is released (HIP, or the CPU for dropped frames)
constexpr uint32_t kProduced = 1;    // kProduced + slot: the frame whose result the slot holds (0: none)
constexpr uint32_t kChosen = kProduced + kSlots; // the slot wait_main picked, kNoSlot if none
constexpr uint32_t kSpins = kChosen + 1;          // how long wait_main spun (diagnostics)
constexpr uint32_t kStatusWords = kSpins + 1;
constexpr uint64_t kStatusBytes = 256;

constexpr char kSource[] = R"hlsl(
#define RS "RootConstants(num32BitConstants=4, b0), UAV(u0), SRV(t0), SRV(t1), SRV(t2), SRV(t3), UAV(u1)"
cbuffer Params : register(b0)
{
    uint frame;    // the frame this command list presents
    uint maxSpins; // wait_main's limit
    uint vectors;  // copy_main: 16-byte vectors to copy
    uint groupsX;  // copy_main: thread groups per row of the dispatch
};
globallycoherent RWByteAddressBuffer status : register(u0);
ByteAddressBuffer slot0 : register(t0);
ByteAddressBuffer slot1 : register(t1);
ByteAddressBuffer slot2 : register(t2);
ByteAddressBuffer slot3 : register(t3);
RWByteAddressBuffer present : register(u1);

[RootSignature(RS)]
[numthreads(1, 1, 1)]
void wait_main()
{
    uint released = 0;
    uint spins = 0;
    [loop] while (spins < maxSpins)
    {
        status.InterlockedOr(0, 0, released); // an atomic: always reads the value HIP last wrote
        if ((int)(released - frame) >= 0)
            break;
        ++spins;
    }
    uint chosen = 0xffffffff;
    uint chosenFrame = 0;
    [unroll] for (uint slot = 0; slot < 4; ++slot)
    {
        uint produced;
        status.InterlockedOr(4 + 4 * slot, 0, produced);
        if (produced != 0 && (int)(frame - produced) >= 0 &&
            (chosen == 0xffffffff || (int)(produced - chosenFrame) > 0))
        {
            chosen = slot;
            chosenFrame = produced;
        }
    }
    status.Store(20, chosen);
    status.Store(24, spins);
}

[RootSignature(RS)]
[numthreads(256, 1, 1)]
void copy_main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint chosen = status.Load(20);
    const uint index = (group.y * groupsX + group.x) * 256 + thread.x;
    if (chosen > 3 || index >= vectors)
        return;
    const uint address = index * 16;
    uint4 data;
    if (chosen == 0)
        data = slot0.Load4(address);
    else if (chosen == 1)
        data = slot1.Load4(address);
    else if (chosen == 2)
        data = slot2.Load4(address);
    else
        data = slot3.Load4(address);
    present.Store4(address, data);
}
)hlsl";

struct Presenter
{
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* wait = nullptr;
    ID3D12PipelineState* copy = nullptr;

    // Compiles the shaders and creates their pipelines; on failure `error` says why.
    bool init(ID3D12Device* device, std::string& error)
    {
        HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (compiler == nullptr)
            compiler = LoadLibraryW(L"d3dcompiler_47.dll");
        auto compile = compiler != nullptr ? reinterpret_cast<pD3DCompile>(
                                                 reinterpret_cast<void*>(GetProcAddress(compiler, "D3DCompile")))
                                           : nullptr;
        if (compile == nullptr)
        {
            error = "d3dcompiler_47.dll unavailable";
            return false;
        }
        ID3DBlob* blobs[2] = {};
        const char* entries[2] = {"wait_main", "copy_main"};
        for (int index = 0; index < 2; ++index)
        {
            ID3DBlob* messages = nullptr;
            const HRESULT hr = compile(kSource, sizeof(kSource) - 1, "d4r_inline.hlsl", nullptr, nullptr,
                                       entries[index], "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blobs[index],
                                       &messages);
            if (FAILED(hr))
            {
                error = std::string("D3DCompile ") + entries[index] + " failed";
                if (messages != nullptr)
                    error += std::string(": ") +
                             std::string(static_cast<const char*>(messages->GetBufferPointer()),
                                         messages->GetBufferSize());
            }
            if (messages != nullptr)
                messages->Release();
            if (FAILED(hr))
            {
                release_blobs(blobs);
                return false;
            }
        }
        HRESULT hr = device->CreateRootSignature(0, blobs[0]->GetBufferPointer(), blobs[0]->GetBufferSize(),
                                                 __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&root));
        ID3D12PipelineState** pipelines[2] = {&wait, &copy};
        for (int index = 0; index < 2 && SUCCEEDED(hr); ++index)
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
            desc.pRootSignature = root;
            desc.CS.pShaderBytecode = blobs[index]->GetBufferPointer();
            desc.CS.BytecodeLength = blobs[index]->GetBufferSize();
            hr = device->CreateComputePipelineState(&desc, __uuidof(ID3D12PipelineState),
                                                    reinterpret_cast<void**>(pipelines[index]));
        }
        release_blobs(blobs);
        if (FAILED(hr))
        {
            char text[96];
            std::snprintf(text, sizeof(text), "pipeline creation failed: 0x%08lx", static_cast<unsigned long>(hr));
            error = text;
            release();
            return false;
        }
        return true;
    }

    void release()
    {
        for (IUnknown* object : {static_cast<IUnknown*>(copy), static_cast<IUnknown*>(wait),
                                 static_cast<IUnknown*>(root)})
            if (object != nullptr)
                object->Release();
        copy = wait = nullptr;
        root = nullptr;
    }

    // Records the wait for `frame` and the copy of the chosen slot into `present` (`bytes` long, a multiple of
    // 16). Every buffer is in COMMON state before and after; the caller copies `present` on afterwards (it is left
    // in COPY_SOURCE state for that, and must be returned to COMMON).
    void record(ID3D12GraphicsCommandList* list, ID3D12Resource* status, ID3D12Resource* const slots[kSlots],
                ID3D12Resource* present, uint32_t frame, uint64_t bytes, uint32_t maxSpins) const
    {
        // Every transition comes before the wait: buffers bound as root descriptors count as used by each dispatch
        // that runs with them bound, the wait's included, so the debug layer would otherwise see them promoted
        // implicitly there and reject a later COMMON -> SRV transition.
        D3D12_RESOURCE_BARRIER barriers[kSlots + 2] = {};
        int count = 0;
        barriers[count++] = transition(status, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for (int slot = 0; slot < kSlots; ++slot)
            barriers[count++] = transition(slots[slot], D3D12_RESOURCE_STATE_COMMON,
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers[count++] = transition(present, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(count, barriers);

        const uint32_t vectors = static_cast<uint32_t>(bytes / 16);
        const uint32_t groups = (vectors + 255) / 256;
        const uint32_t groupsX = groups < 65535 ? (groups > 0 ? groups : 1) : 65535;
        const uint32_t groupsY = (groups + groupsX - 1) / groupsX;
        const uint32_t constants[4] = {frame, maxSpins, vectors, groupsX};
        list->SetComputeRootSignature(root);
        list->SetComputeRoot32BitConstants(0, 4, constants, 0);
        list->SetComputeRootUnorderedAccessView(1, status->GetGPUVirtualAddress());
        for (int slot = 0; slot < kSlots; ++slot)
            list->SetComputeRootShaderResourceView(2 + slot, slots[slot]->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(2 + kSlots, present->GetGPUVirtualAddress());
        list->SetPipelineState(wait);
        list->Dispatch(1, 1, 1);

        // The copy starts once the wait is over and its choice is stored; a barrier between the two dispatches also
        // drops what the shader caches hold, so the slots are read as HIP left them.
        D3D12_RESOURCE_BARRIER uav = {};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = nullptr; // every UAV access
        list->ResourceBarrier(1, &uav);
        list->SetPipelineState(copy);
        list->Dispatch(groupsX, groupsY, 1);

        count = 0;
        barriers[count++] = transition(status, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        for (int slot = 0; slot < kSlots; ++slot)
            barriers[count++] = transition(slots[slot], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                           D3D12_RESOURCE_STATE_COMMON);
        barriers[count++] = transition(present, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                       D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(count, barriers);
    }

    static D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                             D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        return barrier;
    }

private:
    static void release_blobs(ID3DBlob* (&blobs)[2])
    {
        for (ID3DBlob*& blob : blobs)
            if (blob != nullptr)
            {
                blob->Release();
                blob = nullptr;
            }
    }
};
} // namespace d4r_inline
