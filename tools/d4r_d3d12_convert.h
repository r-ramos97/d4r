// Format conversion on the GPU for native Windows' VRAM interop (docs/windows.md, "VRAM interop").
//
// The shim's D3D12 VRAM path copies the game's DLSS resources with CopyTextureRegion when they already are in
// DLSS's canonical layouts (RGBA16F colour and output, R32F depth and exposure, RG16F motion). For the other
// formats the host path accepts, compute shaders on the game's command list convert instead:
//   - input_main reads the game's texture through a typed view and stores the canonical layout into the shared
//     buffer (colour as RGBA16F, depth and exposure as R32F, motion as RG16F);
//   - output_main reads the canonical RGBA16F result and stores it through a typed view of the game's output,
//     whose format the hardware converts to.
// They follow the host path's conversions (convert_row_in, convert_row_out): typeless 8-bit colour is read as
// UNORM, D24 and D16 depth as UNORM, exposure takes the first component and motion the first two; up to rounding
// of the last bit, the results match.
//
// The views live in a shader-visible descriptor heap of the shim's, which the conversions bind on the game's
// command list (with their root signature and pipeline state), as a DLSS evaluation may do; applications restore
// their heaps after DLSS. HLSL compiled at runtime by Windows' d3dcompiler_47.dll, root signatures in the HLSL.
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace d4r_convert
{
// what the input shader stores per texel
enum class Kind : uint32_t
{
    Color = 0,  // RGBA16F, 8 bytes
    Scalar = 1, // R32F (depth, exposure), 4 bytes
    Motion = 2, // RG16F, 4 bytes
};

inline uint32_t texel_bytes(Kind kind)
{
    return kind == Kind::Color ? 8 : 4;
}

constexpr char kSource[] = R"hlsl(
#define INPUT_RS "RootConstants(num32BitConstants=4, b0), DescriptorTable(SRV(t0)), UAV(u0)"
#define OUTPUT_RS "RootConstants(num32BitConstants=4, b0), SRV(t0), DescriptorTable(UAV(u0))"
cbuffer Params : register(b0)
{
    uint width;
    uint height;
    uint pitch; // bytes per row of the buffer
    uint kind;  // input_main: 0 colour (RGBA16F), 1 scalar (R32F), 2 motion (RG16F)
};

Texture2D<float4> source : register(t0);
RWByteAddressBuffer canonical : register(u0);

[RootSignature(INPUT_RS)]
[numthreads(8, 8, 1)]
void input_main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    const float4 value = source.Load(int3(id.xy, 0));
    const uint row = id.y * pitch;
    if (kind == 0)
        canonical.Store2(row + id.x * 8, uint2(f32tof16(value.r) | (f32tof16(value.g) << 16),
                                               f32tof16(value.b) | (f32tof16(value.a) << 16)));
    else if (kind == 1)
        canonical.Store(row + id.x * 4, asuint(value.r));
    else
        canonical.Store(row + id.x * 4, f32tof16(value.r) | (f32tof16(value.g) << 16));
}
)hlsl";

constexpr char kOutputSource[] = R"hlsl(
#define OUTPUT_RS "RootConstants(num32BitConstants=4, b0), SRV(t0), DescriptorTable(UAV(u0))"
cbuffer Params : register(b0)
{
    uint width;
    uint height;
    uint pitch;
    uint unused;
};

ByteAddressBuffer result : register(t0);
RWTexture2D<float4> destination : register(u0);

[RootSignature(OUTPUT_RS)]
[numthreads(8, 8, 1)]
void output_main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    const uint2 texel = result.Load2(id.y * pitch + id.x * 8);
    destination[id.xy] = float4(f16tof32(texel.x), f16tof32(texel.x >> 16), f16tof32(texel.y), f16tof32(texel.y >> 16));
}
)hlsl";

// The typed view input_main reads a game resource of `format` through, for a plane of `kind`; UNKNOWN if the
// conversion does not handle it. `exposure` selects the exposure plane's formats among the scalar ones.
inline DXGI_FORMAT input_view(Kind kind, bool exposure, DXGI_FORMAT format)
{
    switch (kind)
    {
    case Kind::Color:
        switch (format)
        {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R11G11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        // a TYPELESS resource is read as UNORM, as the host path reads sRGB data; a fully typed sRGB resource
        // cannot be viewed as UNORM, so it is not converted here (the host path takes it)
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        default: return DXGI_FORMAT_UNKNOWN;
        }
    case Kind::Scalar:
        if (exposure)
            switch (format)
            {
            case DXGI_FORMAT_R16_FLOAT:
            case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
            case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            case DXGI_FORMAT_R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
            default: return DXGI_FORMAT_UNKNOWN;
            }
        switch (format)
        {
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
        default: return DXGI_FORMAT_UNKNOWN;
        }
    case Kind::Motion:
        switch (format)
        {
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        default: return DXGI_FORMAT_UNKNOWN;
        }
    }
    return DXGI_FORMAT_UNKNOWN;
}

// The typed view output_main stores through for a game output of `format`; UNKNOWN if not handled.
inline DXGI_FORMAT output_view(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R11G11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

class Converter
{
public:
    // Compiles the shaders and creates the pipelines and the descriptor heap; on failure `error` says why.
    bool init(ID3D12Device* device, std::string& error)
    {
        device_ = device;
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
        if (!build(compile, kSource, sizeof(kSource) - 1, "input_main", &inputRoot_, &input_, error) ||
            !build(compile, kOutputSource, sizeof(kOutputSource) - 1, "output_main", &outputRoot_, &output_, error))
            return false;
        D3D12_DESCRIPTOR_HEAP_DESC heap = {};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = kDescriptors;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        const HRESULT hr = device->CreateDescriptorHeap(&heap, __uuidof(ID3D12DescriptorHeap),
                                                        reinterpret_cast<void**>(&heap_));
        if (FAILED(hr))
        {
            char text[96];
            std::snprintf(text, sizeof(text), "CreateDescriptorHeap failed: 0x%08lx", static_cast<unsigned long>(hr));
            error = text;
            return false;
        }
        increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        heap_->GetCPUDescriptorHandleForHeapStart(&cpu_);
        heap_->GetGPUDescriptorHandleForHeapStart(&gpu_);
        return true;
    }

    // Whether the device can read `view` in a shader / store it through a typed UAV.
    bool can_read(DXGI_FORMAT view) const
    {
        return supports(view, D3D12_FORMAT_SUPPORT1_SHADER_LOAD, D3D12_FORMAT_SUPPORT2_NONE);
    }
    bool can_store(DXGI_FORMAT view) const
    {
        return supports(view, D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW, D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
    }

    // Converts `texture` (subresource 0, `width` x `height`, read through `view`) into `buffer` (`pitch` bytes per
    // row). The texture is in COPY_SOURCE state before and after, the buffer in COMMON.
    void record_input(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, DXGI_FORMAT view, Kind kind,
                      ID3D12Resource* buffer, uint32_t pitch, uint32_t width, uint32_t height)
    {
        const UINT slot = next_slot();
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
        desc.Format = view;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(texture, &desc, cpu_at(slot));

        D3D12_RESOURCE_BARRIER barriers[2] = {
            transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            transition(buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        list->ResourceBarrier(2, barriers);
        const uint32_t constants[4] = {width, height, pitch, static_cast<uint32_t>(kind)};
        list->SetDescriptorHeaps(1, &heap_);
        list->SetComputeRootSignature(inputRoot_);
        list->SetComputeRoot32BitConstants(0, 4, constants, 0);
        list->SetComputeRootDescriptorTable(1, gpu_at(slot));
        list->SetComputeRootUnorderedAccessView(2, buffer->GetGPUVirtualAddress());
        list->SetPipelineState(input_);
        list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        barriers[0] = transition(texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barriers[1] = transition(buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        list->ResourceBarrier(2, barriers);
    }

    // Converts the RGBA16F rows of `buffer` into `texture` (subresource 0, stored through `view`). The texture is in
    // COPY_DEST state before and after; the buffer goes from `bufferBefore` to `bufferAfter`.
    void record_output(ID3D12GraphicsCommandList* list, ID3D12Resource* buffer, D3D12_RESOURCE_STATES bufferBefore,
                       D3D12_RESOURCE_STATES bufferAfter, uint32_t pitch, ID3D12Resource* texture, DXGI_FORMAT view,
                       uint32_t width, uint32_t height)
    {
        const UINT slot = next_slot();
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
        desc.Format = view;
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(texture, nullptr, &desc, cpu_at(slot));

        D3D12_RESOURCE_BARRIER barriers[2] = {
            transition(buffer, bufferBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            transition(texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        list->ResourceBarrier(2, barriers);
        const uint32_t constants[4] = {width, height, pitch, 0};
        list->SetDescriptorHeaps(1, &heap_);
        list->SetComputeRootSignature(outputRoot_);
        list->SetComputeRoot32BitConstants(0, 4, constants, 0);
        list->SetComputeRootShaderResourceView(1, buffer->GetGPUVirtualAddress());
        list->SetComputeRootDescriptorTable(2, gpu_at(slot));
        list->SetPipelineState(output_);
        list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        barriers[0] = transition(buffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, bufferAfter);
        barriers[1] = transition(texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(2, barriers);
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
    // A ring of views: a slot is reused 1024 conversions later, long after the command list that used it ran.
    static constexpr UINT kDescriptors = 1024;

    bool build(pD3DCompile compile, const char* source, size_t length, const char* entry, ID3D12RootSignature** root,
               ID3D12PipelineState** pipeline, std::string& error)
    {
        ID3DBlob *blob = nullptr, *messages = nullptr;
        HRESULT hr = compile(source, length, "d4r_convert.hlsl", nullptr, nullptr, entry, "cs_5_1",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &messages);
        if (FAILED(hr))
        {
            error = std::string("D3DCompile ") + entry + " failed";
            if (messages != nullptr)
                error += std::string(": ") + std::string(static_cast<const char*>(messages->GetBufferPointer()),
                                                         messages->GetBufferSize());
        }
        if (messages != nullptr)
            messages->Release();
        if (FAILED(hr))
            return false;
        hr = device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(root));
        if (SUCCEEDED(hr))
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
            desc.pRootSignature = *root;
            desc.CS.pShaderBytecode = blob->GetBufferPointer();
            desc.CS.BytecodeLength = blob->GetBufferSize();
            hr = device_->CreateComputePipelineState(&desc, __uuidof(ID3D12PipelineState),
                                                     reinterpret_cast<void**>(pipeline));
        }
        blob->Release();
        if (FAILED(hr))
        {
            char text[128];
            std::snprintf(text, sizeof(text), "%s: pipeline creation failed: 0x%08lx", entry,
                          static_cast<unsigned long>(hr));
            error = text;
            return false;
        }
        return true;
    }

    bool supports(DXGI_FORMAT format, D3D12_FORMAT_SUPPORT1 one, D3D12_FORMAT_SUPPORT2 two) const
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {format, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
        if (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))))
            return false;
        return (support.Support1 & one) == one && (support.Support2 & two) == two;
    }

    UINT next_slot()
    {
        return next_.fetch_add(1, std::memory_order_relaxed) % kDescriptors;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_at(UINT slot) const
    {
        return {cpu_.ptr + static_cast<SIZE_T>(slot) * increment_};
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_at(UINT slot) const
    {
        return {gpu_.ptr + static_cast<UINT64>(slot) * increment_};
    }

    ID3D12Device* device_ = nullptr;
    ID3D12RootSignature* inputRoot_ = nullptr;
    ID3D12RootSignature* outputRoot_ = nullptr;
    ID3D12PipelineState* input_ = nullptr;
    ID3D12PipelineState* output_ = nullptr;
    ID3D12DescriptorHeap* heap_ = nullptr;
    UINT increment_ = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_ = {};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_ = {};
    std::atomic<UINT> next_{0};
};
} // namespace d4r_convert
