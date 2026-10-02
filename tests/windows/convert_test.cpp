/* The format conversions of tools/d4r_d3d12_convert.h on a real D3D12 runtime (Windows' software renderer, WARP),
   against the host path's conversions (convert_row_in, convert_row_out in tools/d4r_nvngx_shim.cpp), written out
   again here: each game format into DLSS's canonical layout, and the canonical result into each output format.
   usage: convert_test.exe */
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include "d4r_d3d12_convert.h"
#include <dxgi1_4.h>

#include <cmath>
#include <cstring>
#include <vector>

using namespace d4r_convert;

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
    return done && SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator, nullptr)) &&
           SUCCEEDED(device->GetDeviceRemovedReason());
}

static ID3D12Resource* buffer(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state, bool uav = false)
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
    desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* resource = nullptr;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, __uuidof(ID3D12Resource),
                                    reinterpret_cast<void**>(&resource));
    return resource;
}

static ID3D12Resource* texture(DXGI_FORMAT format, UINT width, UINT height, D3D12_RESOURCE_STATES state, bool uav)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* resource = nullptr;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, __uuidof(ID3D12Resource),
                                    reinterpret_cast<void**>(&resource));
    return resource;
}

/* the host path's scalar conversions */
static float unorm(uint32_t value, int bits)
{
    return static_cast<float>(value) / static_cast<float>((1u << bits) - 1u);
}

static uint16_t float_to_half(float value) // round to nearest even
{
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int exponent = static_cast<int>((bits >> 23) & 0xff) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffffu;
    if (((bits >> 23) & 0xff) == 0xff)
        return static_cast<uint16_t>(sign | 0x7c00u | (mantissa != 0 ? 0x200u : 0));
    if (exponent >= 31)
        return static_cast<uint16_t>(sign | 0x7c00u);
    if (exponent <= 0)
    {
        if (exponent < -10)
            return static_cast<uint16_t>(sign);
        mantissa |= 0x800000u;
        const int shift = 14 - exponent;
        uint32_t half = mantissa >> shift;
        const uint32_t rest = mantissa & ((1u << shift) - 1u), halfway = 1u << (shift - 1);
        if (rest > halfway || (rest == halfway && (half & 1u)))
            ++half;
        return static_cast<uint16_t>(sign | half);
    }
    uint32_t half = (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);
    const uint32_t rest = mantissa & 0x1fffu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u)))
        ++half;
    return static_cast<uint16_t>(sign | half);
}

static float half_to_float(uint16_t half)
{
    const uint32_t sign = (half & 0x8000u) << 16, exponent = (half >> 10) & 0x1f, mantissa = half & 0x3ffu;
    float value;
    if (exponent == 0)
        value = std::ldexp(static_cast<float>(mantissa), -24);
    else if (exponent == 31)
        value = mantissa != 0 ? NAN : INFINITY;
    else
        value = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    bits |= sign;
    std::memcpy(&value, &bits, 4);
    return value;
}

static float small_float(uint32_t bits, int mantissaBits) // R11G11B10's unsigned floats
{
    const uint32_t exponent = bits >> mantissaBits, mantissa = bits & ((1u << mantissaBits) - 1u);
    if (exponent == 0)
        return std::ldexp(static_cast<float>(mantissa), -14 - mantissaBits);
    return std::ldexp(1.0f + std::ldexp(static_cast<float>(mantissa), -mantissaBits), static_cast<int>(exponent) - 15);
}

static bool close_half(uint16_t a, uint16_t b) // within one unit in the last place
{
    return (a & 0x8000u) == (b & 0x8000u) ? (a > b ? a - b : b - a) <= 1 : (a & 0x7fffu) == 0 && (b & 0x7fffu) == 0;
}

static bool close_unsigned(uint32_t a, uint32_t b)
{
    return (a > b ? a - b : b - a) <= 1;
}

constexpr UINT kWidth = 37, kHeight = 5; // not a multiple of the 8x8 thread groups

// Uploads `texels` (texelBytes each) into a new texture of `format`, converts it as `kind`, returns the buffer.
static bool convert_in(Converter& converter, DXGI_FORMAT format, UINT texelBytes, const std::vector<uint8_t>& texels,
                       Kind kind, bool exposure, std::vector<uint8_t>& out, uint32_t& pitch)
{
    const DXGI_FORMAT view = input_view(kind, exposure, format);
    if (view == DXGI_FORMAT_UNKNOWN || !converter.can_read(view))
        return false;
    ID3D12Resource* source = texture(format, kWidth, kHeight, D3D12_RESOURCE_STATE_COPY_DEST, false);
    D3D12_RESOURCE_DESC desc = source->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    ID3D12Resource* upload = buffer(D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* mapped = nullptr;
    D3D12_RANGE none = {0, 0};
    upload->Map(0, &none, reinterpret_cast<void**>(&mapped));
    for (UINT y = 0; y < kHeight; ++y)
        std::memcpy(mapped + y * footprint.Footprint.RowPitch, texels.data() + y * kWidth * texelBytes, kWidth * texelBytes);
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = source;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION from = {};
    from.pResource = upload;
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = footprint;
    list->CopyTextureRegion(&destination, 0, 0, 0, &from, nullptr);
    D3D12_RESOURCE_BARRIER barrier = Converter::transition(source, D3D12_RESOURCE_STATE_COPY_DEST,
                                                           D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &barrier);

    pitch = (kWidth * texel_bytes(kind) + 255) & ~255u;
    ID3D12Resource* canonical = buffer(D3D12_HEAP_TYPE_DEFAULT, pitch * kHeight, D3D12_RESOURCE_STATE_COMMON, true);
    ID3D12Resource* readback = buffer(D3D12_HEAP_TYPE_READBACK, pitch * kHeight, D3D12_RESOURCE_STATE_COPY_DEST);
    converter.record_input(list, source, view, kind, canonical, pitch, kWidth, kHeight);
    list->CopyBufferRegion(readback, 0, canonical, 0, pitch * kHeight);
    if (!run())
        return false;
    uint8_t* down = nullptr;
    D3D12_RANGE all = {0, pitch * kHeight};
    readback->Map(0, &all, reinterpret_cast<void**>(&down));
    out.assign(down, down + pitch * kHeight);
    readback->Unmap(0, &none);
    return true;
}

// Converts canonical RGBA16F `halves` into a new texture of `format`; returns its rows (texelBytes each).
static bool convert_out(Converter& converter, DXGI_FORMAT format, UINT texelBytes, const std::vector<uint16_t>& halves,
                        std::vector<uint8_t>& out)
{
    const DXGI_FORMAT view = output_view(format);
    if (view == DXGI_FORMAT_UNKNOWN || !converter.can_store(view))
        return false;
    const uint32_t pitch = (kWidth * 8 + 255) & ~255u;
    ID3D12Resource* upload = buffer(D3D12_HEAP_TYPE_UPLOAD, pitch * kHeight, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource* canonical = buffer(D3D12_HEAP_TYPE_DEFAULT, pitch * kHeight, D3D12_RESOURCE_STATE_COMMON, true);
    uint8_t* mapped = nullptr;
    D3D12_RANGE none = {0, 0};
    upload->Map(0, &none, reinterpret_cast<void**>(&mapped));
    for (UINT y = 0; y < kHeight; ++y)
        std::memcpy(mapped + y * pitch, halves.data() + y * kWidth * 4, kWidth * 8);
    list->CopyBufferRegion(canonical, 0, upload, 0, pitch * kHeight);
    if (!run())
        return false;

    ID3D12Resource* target = texture(format, kWidth, kHeight, D3D12_RESOURCE_STATE_COPY_DEST, true);
    converter.record_output(list, canonical, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, pitch, target,
                            view, kWidth, kHeight);
    D3D12_RESOURCE_BARRIER barrier = Converter::transition(target, D3D12_RESOURCE_STATE_COPY_DEST,
                                                           D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &barrier);
    D3D12_RESOURCE_DESC desc = target->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    ID3D12Resource* readback = buffer(D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION to = {};
    to.pResource = readback;
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION from = {};
    from.pResource = target;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    if (!run())
        return false;
    uint8_t* down = nullptr;
    D3D12_RANGE all = {0, static_cast<SIZE_T>(total)};
    readback->Map(0, &all, reinterpret_cast<void**>(&down));
    out.resize(kWidth * kHeight * texelBytes);
    for (UINT y = 0; y < kHeight; ++y)
        std::memcpy(out.data() + y * kWidth * texelBytes, down + y * footprint.Footprint.RowPitch, kWidth * texelBytes);
    readback->Unmap(0, &none);
    return true;
}

static uint32_t pseudo_random(uint32_t& state)
{
    state = state * 1664525u + 1013904223u;
    return state >> 8;
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

    Converter converter;
    std::string error;
    const bool ready = converter.init(device, error);
    check(ready, "conversion shaders compile, pipelines and descriptor heap build");
    if (!ready)
    {
        std::printf("  %s\n", error.c_str());
        return 1;
    }
    const UINT count = kWidth * kHeight;
    uint32_t seed = 12345;

    // colour: RGBA8 (typeless too), BGRA8, RGB10A2, R11G11B10, RGBA32F -> RGBA16F
    struct ColorCase
    {
        const char* name;
        DXGI_FORMAT format;
        UINT bytes;
    } colors[] = {
        {"colour RGBA8 -> RGBA16F", DXGI_FORMAT_R8G8B8A8_UNORM, 4},
        {"colour RGBA8 typeless (sRGB data) -> RGBA16F", DXGI_FORMAT_R8G8B8A8_TYPELESS, 4},
        {"colour BGRA8 -> RGBA16F", DXGI_FORMAT_B8G8R8A8_UNORM, 4},
        {"colour RGB10A2 -> RGBA16F", DXGI_FORMAT_R10G10B10A2_UNORM, 4},
        {"colour R11G11B10 -> RGBA16F (exact)", DXGI_FORMAT_R11G11B10_FLOAT, 4},
        {"colour RGBA32F -> RGBA16F", DXGI_FORMAT_R32G32B32A32_FLOAT, 16},
    };
    for (const ColorCase& test : colors)
    {
        std::vector<uint8_t> texels(count * test.bytes);
        std::vector<uint16_t> expected(count * 4);
        for (UINT i = 0; i < count; ++i)
        {
            float rgba[4] = {0, 0, 0, 1};
            uint8_t* texel = texels.data() + i * test.bytes;
            if (test.bytes == 16)
            {
                for (int c = 0; c < 4; ++c)
                    rgba[c] = static_cast<float>(pseudo_random(seed) % 100000) / 997.0f;
                std::memcpy(texel, rgba, 16);
            }
            else
            {
                const uint32_t packed = pseudo_random(seed) ^ (pseudo_random(seed) << 16);
                std::memcpy(texel, &packed, 4);
                if (test.format == DXGI_FORMAT_R11G11B10_FLOAT)
                {
                    // finite values only (exponent 31 is inf/NaN)
                    uint32_t r = packed & 0x7ffu, g = (packed >> 11) & 0x7ffu, b = (packed >> 22) & 0x3ffu;
                    r &= (r >> 6) == 31 ? 0x3bfu : 0x7ffu;
                    g &= (g >> 6) == 31 ? 0x3bfu : 0x7ffu;
                    b &= (b >> 5) == 31 ? 0x1dfu : 0x3ffu;
                    const uint32_t finite = r | (g << 11) | (b << 22);
                    std::memcpy(texel, &finite, 4);
                    rgba[0] = small_float(r, 6), rgba[1] = small_float(g, 6), rgba[2] = small_float(b, 5);
                }
                else if (test.format == DXGI_FORMAT_R10G10B10A2_UNORM)
                {
                    rgba[0] = unorm(packed & 0x3ffu, 10), rgba[1] = unorm((packed >> 10) & 0x3ffu, 10);
                    rgba[2] = unorm((packed >> 20) & 0x3ffu, 10), rgba[3] = unorm(packed >> 30, 2);
                }
                else if (test.format == DXGI_FORMAT_B8G8R8A8_UNORM)
                {
                    rgba[0] = unorm(texel[2], 8), rgba[1] = unorm(texel[1], 8), rgba[2] = unorm(texel[0], 8);
                    rgba[3] = unorm(texel[3], 8);
                }
                else
                    for (int c = 0; c < 4; ++c)
                        rgba[c] = unorm(texel[c], 8);
            }
            for (int c = 0; c < 4; ++c)
                expected[i * 4 + c] = float_to_half(rgba[c]);
        }
        std::vector<uint8_t> out;
        uint32_t pitch = 0;
        if (!convert_in(converter, test.format, test.bytes, texels, Kind::Color, false, out, pitch))
        {
            check(false, test.name);
            continue;
        }
        size_t wrong = 0;
        for (UINT i = 0; i < count; ++i)
            for (int c = 0; c < 4; ++c)
            {
                uint16_t got;
                std::memcpy(&got, out.data() + (i / kWidth) * pitch + (i % kWidth) * 8 + c * 2, 2);
                const bool exact = test.format == DXGI_FORMAT_R11G11B10_FLOAT;
                wrong += exact ? got != expected[i * 4 + c] : !close_half(got, expected[i * 4 + c]);
            }
        if (wrong != 0)
            std::printf("  %zu of %u components differ\n", wrong, count * 4);
        check(wrong == 0, test.name);
    }

    // depth: D24 (as R24G8 typeless) and D16 (as R16 typeless) -> R32F; exposure R16F -> R32F
    {
        std::vector<uint8_t> texels(count * 4), out;
        std::vector<float> expected(count);
        for (UINT i = 0; i < count; ++i)
        {
            const uint32_t packed = pseudo_random(seed) ^ (pseudo_random(seed) << 24);
            std::memcpy(texels.data() + i * 4, &packed, 4);
            expected[i] = unorm(packed & 0xffffffu, 24);
        }
        uint32_t pitch = 0;
        size_t wrong = count;
        if (convert_in(converter, DXGI_FORMAT_R24G8_TYPELESS, 4, texels, Kind::Scalar, false, out, pitch))
        {
            wrong = 0;
            for (UINT i = 0; i < count; ++i)
            {
                float got;
                std::memcpy(&got, out.data() + (i / kWidth) * pitch + (i % kWidth) * 4, 4);
                wrong += std::fabs(got - expected[i]) > 1e-7f;
            }
        }
        check(wrong == 0, "depth D24 -> R32F");
    }
    {
        std::vector<uint8_t> texels(count * 2), out;
        std::vector<float> expected(count);
        for (UINT i = 0; i < count; ++i)
        {
            const uint16_t value = static_cast<uint16_t>(pseudo_random(seed));
            std::memcpy(texels.data() + i * 2, &value, 2);
            expected[i] = unorm(value, 16);
        }
        uint32_t pitch = 0;
        size_t wrong = count;
        if (convert_in(converter, DXGI_FORMAT_R16_TYPELESS, 2, texels, Kind::Scalar, false, out, pitch))
        {
            wrong = 0;
            for (UINT i = 0; i < count; ++i)
            {
                float got;
                std::memcpy(&got, out.data() + (i / kWidth) * pitch + (i % kWidth) * 4, 4);
                wrong += std::fabs(got - expected[i]) > 1e-7f;
            }
        }
        check(wrong == 0, "depth D16 -> R32F");
    }
    {
        std::vector<uint8_t> texels(count * 2), out;
        std::vector<float> expected(count);
        for (UINT i = 0; i < count; ++i)
        {
            const uint16_t value = float_to_half(static_cast<float>(pseudo_random(seed) % 4000) / 37.0f);
            std::memcpy(texels.data() + i * 2, &value, 2);
            expected[i] = half_to_float(value);
        }
        uint32_t pitch = 0;
        size_t wrong = count;
        if (convert_in(converter, DXGI_FORMAT_R16_FLOAT, 2, texels, Kind::Scalar, true, out, pitch))
        {
            wrong = 0;
            for (UINT i = 0; i < count; ++i)
            {
                float got;
                std::memcpy(&got, out.data() + (i / kWidth) * pitch + (i % kWidth) * 4, 4);
                wrong += got != expected[i];
            }
        }
        check(wrong == 0, "exposure R16F -> R32F (exact)");
    }

    // motion: RG32F and RGBA16F -> RG16F
    {
        std::vector<uint8_t> texels(count * 8), out;
        std::vector<uint16_t> expected(count * 2);
        for (UINT i = 0; i < count; ++i)
        {
            const float mv[2] = {(static_cast<float>(pseudo_random(seed) % 20000) - 10000.0f) / 313.0f,
                                 (static_cast<float>(pseudo_random(seed) % 20000) - 10000.0f) / 313.0f};
            std::memcpy(texels.data() + i * 8, mv, 8);
            expected[i * 2] = float_to_half(mv[0]), expected[i * 2 + 1] = float_to_half(mv[1]);
        }
        uint32_t pitch = 0;
        size_t wrong = count;
        if (convert_in(converter, DXGI_FORMAT_R32G32_FLOAT, 8, texels, Kind::Motion, false, out, pitch))
        {
            wrong = 0;
            for (UINT i = 0; i < count; ++i)
                for (int c = 0; c < 2; ++c)
                {
                    uint16_t got;
                    std::memcpy(&got, out.data() + (i / kWidth) * pitch + (i % kWidth) * 4 + c * 2, 2);
                    wrong += !close_half(got, expected[i * 2 + c]);
                }
        }
        check(wrong == 0, "motion RG32F -> RG16F");
    }
    {
        std::vector<uint8_t> texels(count * 8), out;
        std::vector<uint16_t> expected(count * 2);
        for (UINT i = 0; i < count; ++i)
        {
            uint16_t texel[4];
            for (int c = 0; c < 4; ++c)
                texel[c] = float_to_half((static_cast<float>(pseudo_random(seed) % 20000) - 10000.0f) / 313.0f);
            std::memcpy(texels.data() + i * 8, texel, 8);
            expected[i * 2] = texel[0], expected[i * 2 + 1] = texel[1];
        }
        uint32_t pitch = 0;
        size_t wrong = count;
        if (convert_in(converter, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, texels, Kind::Motion, false, out, pitch))
        {
            wrong = 0;
            for (UINT i = 0; i < count; ++i)
                for (int c = 0; c < 2; ++c)
                {
                    uint16_t got;
                    std::memcpy(&got, out.data() + (i / kWidth) * pitch + (i % kWidth) * 4 + c * 2, 2);
                    wrong += got != expected[i * 2 + c];
                }
        }
        check(wrong == 0, "motion RGBA16F -> RG16F (exact)");
    }

    // output: RGBA16F -> RGBA8, BGRA8, RGB10A2, R11G11B10, RGBA32F
    std::vector<uint16_t> halves(count * 4);
    for (UINT i = 0; i < count * 4; ++i)
        halves[i] = float_to_half(static_cast<float>(pseudo_random(seed) % 13000) / 10000.0f - 0.1f); // [-0.1, 1.2)
    auto to_unorm = [](float value, int bits) {
        const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
        return static_cast<uint32_t>(std::lround(clamped * static_cast<float>((1u << bits) - 1u)));
    };
    struct OutputCase
    {
        const char* name;
        DXGI_FORMAT format;
        UINT bytes;
    } outputs[] = {
        {"output RGBA16F -> RGBA8", DXGI_FORMAT_R8G8B8A8_UNORM, 4},
        {"output RGBA16F -> BGRA8", DXGI_FORMAT_B8G8R8A8_UNORM, 4},
        {"output RGBA16F -> RGB10A2", DXGI_FORMAT_R10G10B10A2_UNORM, 4},
        {"output RGBA16F -> R11G11B10", DXGI_FORMAT_R11G11B10_FLOAT, 4},
        {"output RGBA16F -> RGBA32F (exact)", DXGI_FORMAT_R32G32B32A32_FLOAT, 16},
    };
    for (const OutputCase& test : outputs)
    {
        std::vector<uint8_t> out;
        if (!convert_out(converter, test.format, test.bytes, halves, out))
        {
            if (test.format == DXGI_FORMAT_B8G8R8A8_UNORM && !converter.can_store(DXGI_FORMAT_B8G8R8A8_UNORM))
            {
                std::printf("PASS %s (typed BGRA8 stores unsupported here: left to the host path)\n", test.name);
                continue;
            }
            check(false, test.name);
            continue;
        }
        size_t wrong = 0;
        for (UINT i = 0; i < count; ++i)
        {
            const uint16_t* texel = halves.data() + i * 4;
            const uint8_t* got = out.data() + i * test.bytes;
            float f[4];
            for (int c = 0; c < 4; ++c)
                f[c] = half_to_float(texel[c]);
            uint32_t packed = 0;
            std::memcpy(&packed, got, 4);
            switch (test.format)
            {
            case DXGI_FORMAT_R8G8B8A8_UNORM:
                for (int c = 0; c < 4; ++c)
                    wrong += !close_unsigned(got[c], to_unorm(f[c], 8));
                break;
            case DXGI_FORMAT_B8G8R8A8_UNORM:
                wrong += !close_unsigned(got[0], to_unorm(f[2], 8)) + !close_unsigned(got[1], to_unorm(f[1], 8)) +
                         !close_unsigned(got[2], to_unorm(f[0], 8)) + !close_unsigned(got[3], to_unorm(f[3], 8));
                break;
            case DXGI_FORMAT_R10G10B10A2_UNORM:
                wrong += !close_unsigned(packed & 0x3ffu, to_unorm(f[0], 10)) +
                         !close_unsigned((packed >> 10) & 0x3ffu, to_unorm(f[1], 10)) +
                         !close_unsigned((packed >> 20) & 0x3ffu, to_unorm(f[2], 10)) +
                         !close_unsigned(packed >> 30, to_unorm(f[3], 2));
                break;
            case DXGI_FORMAT_R11G11B10_FLOAT:
            {
                // compare decoded values: within one step of the 6/5-bit mantissa, negatives clamped to zero
                const float decoded[3] = {small_float(packed & 0x7ffu, 6), small_float((packed >> 11) & 0x7ffu, 6),
                                          small_float((packed >> 22) & 0x3ffu, 5)};
                for (int c = 0; c < 3; ++c)
                {
                    const float want = f[c] > 0.0f ? f[c] : 0.0f;
                    wrong += std::fabs(decoded[c] - want) > want * (c == 2 ? 1.0f / 32 : 1.0f / 64) + 1e-6f;
                }
                break;
            }
            default:
            {
                float values[4];
                std::memcpy(values, got, 16);
                for (int c = 0; c < 4; ++c)
                    wrong += values[c] != f[c];
                break;
            }
            }
        }
        if (wrong != 0)
            std::printf("  %zu components differ\n", wrong);
        check(wrong == 0, test.name);
    }

    check(input_view(Kind::Color, false, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) == DXGI_FORMAT_UNKNOWN,
          "fully typed sRGB colour is left to the host path (no UNORM view of it)");
    std::printf("%d failure(s)\n", failures);
    return failures != 0;
}
