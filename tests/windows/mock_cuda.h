/* Host-memory stand-ins for CUDA arrays and 2D copies, shared by the mock ZLUDA (zluda_nvcuda.dll) and the mock
   HIP (amdhip64_7.dll): "device" memory is host memory, an array is a MockArray. The copy descriptor is
   CUDA_MEMCPY2D's (and hip_Memcpy2D's) layout; memory types are CUDA's (1 host, 2 device, 3 array, 4 unified) or
   HIP's (1 host, 2 device, 10 array). */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MOCK_ARRAY_MAGIC 0x41524d4bu /* "KMRA" */

typedef struct
{
    uint32_t magic;
    uint32_t element_bytes; /* channels x format size */
    size_t width, height;
    unsigned int format, channels;
    unsigned char* data; /* rows of width * element_bytes */
} MockArray;

typedef struct
{
    size_t srcXInBytes, srcY;
    uint32_t srcMemoryType;
    const void* srcHost;
    uint64_t srcDevice;
    void* srcArray;
    size_t srcPitch;
    size_t dstXInBytes, dstY;
    uint32_t dstMemoryType;
    void* dstHost;
    uint64_t dstDevice;
    void* dstArray;
    size_t dstPitch;
    size_t WidthInBytes, Height;
} MockMemcpy2D;

_Static_assert(sizeof(MockMemcpy2D) == 128, "CUDA_MEMCPY2D layout");

static inline unsigned int mock_format_bytes(unsigned int format)
{
    switch (format)
    {
    case 0x01: case 0x08: return 1; /* (UN)SIGNED_INT8 */
    case 0x02: case 0x09: case 0x10: return 2; /* INT16, HALF */
    default: return 4; /* INT32, FLOAT */
    }
}

static inline MockArray* mock_array_new(size_t width, size_t height, unsigned int format, unsigned int channels)
{
    MockArray* array = (MockArray*)calloc(1, sizeof(MockArray));
    array->magic = MOCK_ARRAY_MAGIC;
    array->element_bytes = mock_format_bytes(format) * channels;
    array->width = width;
    array->height = height != 0 ? height : 1;
    array->format = format;
    array->channels = channels;
    array->data = (unsigned char*)calloc(array->width * array->height, array->element_bytes);
    return array;
}

/* base and pitch of one side of a copy; 0 for an unknown memory type */
static inline unsigned char* mock_side(uint32_t type, const void* host, uint64_t device, void* array, size_t pitch,
                                       size_t x, size_t y, size_t* row_pitch)
{
    if (type == 3 || type == 10) /* CUDA or HIP array */
    {
        MockArray* mock = (MockArray*)array;
        if (mock == NULL || mock->magic != MOCK_ARRAY_MAGIC)
            return NULL;
        *row_pitch = mock->width * mock->element_bytes;
        return mock->data + y * *row_pitch + x;
    }
    if (type == 1 || type == 2 || type == 4)
    {
        unsigned char* base = type == 1 ? (unsigned char*)host : (unsigned char*)(uintptr_t)device;
        if (type == 4 && base == NULL)
            base = (unsigned char*)host;
        *row_pitch = pitch;
        return base != NULL ? base + y * pitch + x : NULL;
    }
    return NULL;
}

static inline int mock_copy_2d(const MockMemcpy2D* copy)
{
    size_t src_pitch = 0, dst_pitch = 0;
    const unsigned char* src = mock_side(copy->srcMemoryType, copy->srcHost, copy->srcDevice, copy->srcArray,
                                         copy->srcPitch, copy->srcXInBytes, copy->srcY, &src_pitch);
    unsigned char* dst = mock_side(copy->dstMemoryType, copy->dstHost, copy->dstDevice, copy->dstArray,
                                   copy->dstPitch, copy->dstXInBytes, copy->dstY, &dst_pitch);
    if (src == NULL || dst == NULL)
        return 1; /* invalid value */
    for (size_t row = 0; row < copy->Height; ++row)
        memmove(dst + row * dst_pitch, src + row * src_pitch, copy->WidthInBytes);
    return 0;
}
