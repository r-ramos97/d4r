/* A stand-in for ZLUDA's Windows nvcuda.dll (built as zluda_nvcuda.dll) for the native bridge test: host
   memory instead of a GPU, and counters the test reads through mock_zluda_stats. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mock_cuda.h"

#define EXPORT __declspec(dllexport)
typedef int CUresult;

static int init_calls, sync_calls, module_loads, launches;
static char native_dir_at_init[1024];
static char wmma_fp8_native_at_init[16];

EXPORT CUresult cuInit(unsigned int flags)
{
    (void)flags;
    ++init_calls;
    /* what ZLUDA (Rust std::env) would see */
    if (GetEnvironmentVariableA("D4R_ZLUDA_NATIVE_DIR", native_dir_at_init, sizeof(native_dir_at_init)) == 0)
        native_dir_at_init[0] = '\0';
    if (GetEnvironmentVariableA("D4R_ZLUDA_WMMA_FP8_NATIVE", wmma_fp8_native_at_init, sizeof(wmma_fp8_native_at_init)) == 0)
        wmma_fp8_native_at_init[0] = '\0';
    return 0;
}
EXPORT CUresult cuDeviceGetCount(int* count) { *count = 3; return 0; } /* mock_hip's GPUs */
EXPORT CUresult cuDeviceGet(int* device, int ordinal)
{
    if (ordinal < 0 || ordinal >= 3)
        return 101;
    *device = ordinal;
    return 0;
}
static int context_device = -1;
EXPORT CUresult cuCtxCreate_v2(void** context, unsigned int flags, int device)
{
    (void)flags;
    context_device = device;
    *context = (void*)0x1234;
    return 0;
}
EXPORT int mock_zluda_context_device(void) { return context_device; }
EXPORT CUresult cuCtxSynchronize(void) { ++sync_calls; return 0; }
EXPORT CUresult cuModuleLoadData(void** module, const void* image)
{
    (void)image;
    ++module_loads;
    *module = (void*)(uintptr_t)module_loads;
    return 0;
}
EXPORT CUresult cuModuleGetFunction(void** function, void* module, const char* name)
{
    (void)module;
    *function = _strdup(name);
    return 0;
}
EXPORT CUresult cuLaunchKernel(void* f, unsigned gx, unsigned gy, unsigned gz, unsigned bx, unsigned by, unsigned bz,
                               unsigned shared, void* stream, void** params, void** extra)
{
    (void)f, (void)gx, (void)gy, (void)gz, (void)bx, (void)by, (void)bz, (void)shared, (void)stream, (void)params, (void)extra;
    ++launches;
    return 0;
}
EXPORT CUresult cuMemAlloc_v2(uint64_t* pointer, size_t bytes)
{
    *pointer = (uint64_t)(uintptr_t)malloc(bytes);
    return *pointer != 0 ? 0 : 2;
}
EXPORT CUresult cuMemFree_v2(uint64_t pointer) { free((void*)(uintptr_t)pointer); return 0; }
EXPORT CUresult cuDeviceGetLuid(char* luid, unsigned int* mask, int device)
{
    (void)device;
    for (int i = 0; i < 8; ++i)
        luid[i] = (char)(i + 1);
    *mask = 1;
    return 0;
}
/* host memory stands in for device memory (the interop probe's copies) */
EXPORT CUresult cuMemcpyDtoH_v2(void* host, uint64_t device, size_t bytes)
{
    memcpy(host, (const void*)(uintptr_t)device, bytes);
    return 0;
}
EXPORT CUresult cuMemcpyHtoD_v2(uint64_t device, const void* host, size_t bytes)
{
    memcpy((void*)(uintptr_t)device, host, bytes);
    return 0;
}
EXPORT CUresult cuMemcpyDtoD_v2(uint64_t destination, uint64_t source, size_t bytes)
{
    memmove((void*)(uintptr_t)destination, (const void*)(uintptr_t)source, bytes);
    return 0;
}
EXPORT CUresult cuMemsetD32_v2(uint64_t device, unsigned int value, size_t count)
{
    uint32_t* words = (uint32_t*)(uintptr_t)device;
    for (size_t i = 0; i < count; ++i)
        words[i] = value;
    return 0;
}
EXPORT CUresult cuMemsetD32Async(uint64_t device, unsigned int value, size_t count, void* stream)
{
    (void)stream;
    return cuMemsetD32_v2(device, value, count);
}
EXPORT CUresult cuStreamQuery(void* stream) { (void)stream; return 0; }
EXPORT CUresult cuDeviceGetName(char* name, int length, int device)
{
    snprintf(name, (size_t)length, "mock ZLUDA device %d", device);
    return 0;
}
/* arrays, texture and surface objects, 2D copies, host memory, streams and events, for the shim's evaluation
   path (tests/test_windows_native.py's harness test) */
EXPORT CUresult cuArrayCreate_v2(void** array, const size_t* descriptor) /* {Width, Height, Format, NumChannels} */
{
    unsigned int format, channels;
    memcpy(&format, descriptor + 2, 4);
    memcpy(&channels, (const char*)(descriptor + 2) + 4, 4);
    *array = mock_array_new(descriptor[0], descriptor[1], format, channels);
    return 0;
}
EXPORT CUresult cuArray3DCreate_v2(void** array, const size_t* descriptor) /* {Width, Height, Depth, Format, ...} */
{
    unsigned int format, channels;
    memcpy(&format, descriptor + 3, 4);
    memcpy(&channels, (const char*)(descriptor + 3) + 4, 4);
    *array = mock_array_new(descriptor[0], descriptor[1], format, channels);
    return 0;
}
EXPORT CUresult cuArrayDestroy(void* array)
{
    MockArray* mock = (MockArray*)array;
    if (mock == NULL || mock->magic != MOCK_ARRAY_MAGIC)
        return 1;
    mock->magic = 0;
    free(mock->data);
    free(mock);
    return 0;
}
EXPORT CUresult cuArrayGetDescriptor_v2(size_t* descriptor, void* array)
{
    const MockArray* mock = (const MockArray*)array;
    if (mock == NULL || mock->magic != MOCK_ARRAY_MAGIC)
        return 1;
    descriptor[0] = mock->width, descriptor[1] = mock->height;
    memcpy(descriptor + 2, &mock->format, 4);
    memcpy((char*)(descriptor + 2) + 4, &mock->channels, 4);
    return 0;
}
EXPORT CUresult cuMemcpy2D_v2(const MockMemcpy2D* copy) { return mock_copy_2d(copy); }

/* texture and surface objects: their resource descriptors (CUDA_RESOURCE_DESC, 144 bytes), by handle */
enum { MAX_OBJECTS = 256 };
static unsigned char objects[MAX_OBJECTS][144];
static int object_used[MAX_OBJECTS];
static CRITICAL_SECTION object_lock;
static INIT_ONCE object_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK init_objects(PINIT_ONCE once, PVOID parameter, PVOID* context)
{
    (void)once, (void)parameter, (void)context;
    InitializeCriticalSection(&object_lock);
    return TRUE;
}
static CUresult object_create(uint64_t* object, const void* resource)
{
    InitOnceExecuteOnce(&object_once, init_objects, NULL, NULL);
    EnterCriticalSection(&object_lock);
    for (int index = 0; index < MAX_OBJECTS; ++index)
        if (!object_used[index])
        {
            object_used[index] = 1;
            memcpy(objects[index], resource, sizeof(objects[index]));
            LeaveCriticalSection(&object_lock);
            *object = (uint64_t)index + 1;
            return 0;
        }
    LeaveCriticalSection(&object_lock);
    return 2; /* out of memory */
}
static CUresult object_destroy(uint64_t object)
{
    if (object == 0 || object > MAX_OBJECTS)
        return 1;
    InitOnceExecuteOnce(&object_once, init_objects, NULL, NULL);
    EnterCriticalSection(&object_lock);
    object_used[object - 1] = 0;
    LeaveCriticalSection(&object_lock);
    return 0;
}
EXPORT CUresult cuTexObjectCreate(uint64_t* object, const void* resource, const void* texture, const void* view)
{
    (void)texture, (void)view;
    return object_create(object, resource);
}
EXPORT CUresult cuTexObjectDestroy(uint64_t object) { return object_destroy(object); }
EXPORT CUresult cuSurfObjectCreate(uint64_t* object, const void* resource) { return object_create(object, resource); }
EXPORT CUresult cuSurfObjectDestroy(uint64_t object) { return object_destroy(object); }
EXPORT CUresult cuTexObjectGetResourceDesc(void* resource, uint64_t object)
{
    if (object == 0 || object > MAX_OBJECTS || !object_used[object - 1])
        return 1;
    memcpy(resource, objects[object - 1], 144);
    return 0;
}
EXPORT CUresult cuSurfObjectGetResourceDesc(void* resource, uint64_t object)
{
    return cuTexObjectGetResourceDesc(resource, object);
}
/* for the mock NGX core: an object's array, or its pitch-linear memory (data, width, height, pitch) */
EXPORT int mock_zluda_object(uint64_t object, unsigned char** data, size_t* width, size_t* height, size_t* pitch,
                             unsigned int* element_bytes)
{
    if (object == 0 || object > MAX_OBJECTS || !object_used[object - 1])
        return 0;
    const unsigned char* resource = objects[object - 1];
    uint32_t type;
    memcpy(&type, resource, 4);
    if (type == 0) /* array */
    {
        MockArray* array;
        memcpy(&array, resource + 8, sizeof(array));
        if (array == NULL || array->magic != MOCK_ARRAY_MAGIC)
            return 0;
        *data = array->data, *width = array->width, *height = array->height;
        *pitch = array->width * array->element_bytes, *element_bytes = array->element_bytes;
        return 1;
    }
    if (type == 3) /* pitch 2D: {devPtr, format, numChannels, width, height, pitchInBytes} */
    {
        uint64_t pointer;
        unsigned int format, channels;
        memcpy(&pointer, resource + 8, 8);
        memcpy(&format, resource + 16, 4);
        memcpy(&channels, resource + 20, 4);
        memcpy(width, resource + 24, 8);
        memcpy(height, resource + 32, 8);
        memcpy(pitch, resource + 40, 8);
        *data = (unsigned char*)(uintptr_t)pointer;
        *element_bytes = mock_format_bytes(format) * channels;
        return 1;
    }
    return 0;
}

EXPORT CUresult cuMemHostAlloc(void** pointer, size_t bytes, unsigned int flags)
{
    (void)flags;
    *pointer = calloc(1, bytes);
    return *pointer != NULL ? 0 : 2;
}
EXPORT CUresult cuMemFreeHost(void* pointer) { free(pointer); return 0; }
EXPORT CUresult cuMemcpyHtoDAsync_v2(uint64_t device, const void* host, size_t bytes, void* stream)
{
    (void)stream;
    memcpy((void*)(uintptr_t)device, host, bytes);
    return 0;
}
EXPORT CUresult cuMemcpyDtoHAsync_v2(void* host, uint64_t device, size_t bytes, void* stream)
{
    (void)stream;
    memcpy(host, (const void*)(uintptr_t)device, bytes);
    return 0;
}
EXPORT CUresult cuMemcpyDtoDAsync_v2(uint64_t destination, uint64_t source, size_t bytes, void* stream)
{
    (void)stream;
    memmove((void*)(uintptr_t)destination, (const void*)(uintptr_t)source, bytes);
    return 0;
}
static int streams;
EXPORT CUresult cuStreamCreate(void** stream, unsigned int flags)
{
    (void)flags;
    *stream = (void*)(uintptr_t)(0x5000 + ++streams);
    return 0;
}
EXPORT CUresult cuStreamSynchronize(void* stream) { (void)stream; return 0; }
EXPORT CUresult cuStreamDestroy_v2(void* stream) { (void)stream; return 0; }
static int events;
EXPORT CUresult cuEventCreate(void** event, unsigned int flags)
{
    (void)flags;
    *event = (void*)(uintptr_t)(0x6000 + ++events);
    return 0;
}
EXPORT CUresult cuEventRecord(void* event, void* stream) { (void)event, (void)stream; return 0; }
EXPORT CUresult cuEventSynchronize(void* event) { (void)event; return 0; }
EXPORT CUresult cuEventQuery(void* event) { (void)event; return 0; }
EXPORT CUresult cuEventDestroy_v2(void* event) { (void)event; return 0; }
EXPORT CUresult cuEventElapsedTime(float* milliseconds, void* start, void* end)
{
    (void)start, (void)end;
    *milliseconds = 1.0f;
    return 0;
}
EXPORT CUresult cuCtxGetCurrent(void** context) { *context = (void*)0x1234; return 0; }

EXPORT CUresult cuGetErrorString(CUresult code, const char** message)
{
    (void)code;
    *message = "mock error";
    return 0;
}
EXPORT CUresult cuDriverGetVersion(int* version) { *version = 12080; return 0; }

EXPORT void mock_zluda_stats(int* init, int* sync, int* loads, int* launched, const char** native_dir,
                             const char** fp8_native)
{
    *init = init_calls, *sync = sync_calls, *loads = module_loads, *launched = launches;
    *native_dir = native_dir_at_init;
    *fp8_native = wmma_fp8_native_at_init;
}
