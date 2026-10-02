/* A stand-in for the HIP SDK's amdhip64_7.dll. It lists a Ryzen 7000 integrated GPU (gfx1036) before two
   discrete GPUs, an RX 9070 XT (gfx1201) and a larger RX 7900 XTX (gfx1100), as HIP reports a desktop with its
   iGPU enabled. It also records the D3D12 interop calls (external memory and semaphores) for
   mock_hip_interop, reading HIP's descriptors at the byte offsets of HIP's headers. */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "d4r_hip_props.h"

#define EXPORT __declspec(dllexport)

static const struct
{
    const char* name;
    const char* target;
    int compute_units;
    int integrated;
    uint32_t luid_low;
} devices[] = {
    {"AMD Radeon(TM) Graphics", "gfx1036", 1, 1, 0x1000},
    {"AMD Radeon RX 9070 XT", "gfx1201:sramecc-:xnack-", 32, 0, 0x2000}, /* HIP counts WGPs on RDNA */
    {"AMD Radeon RX 7900 XTX", "gfx1100", 48, 0, 0x3000},
};
enum { DEVICES = sizeof(devices) / sizeof(devices[0]) };

EXPORT int hipGetDeviceCount(int* count) { *count = DEVICES; return 0; }
EXPORT int hipGetDevicePropertiesR0600(void* out, int device)
{
    D4rHipDevicePropPrefix* properties = (D4rHipDevicePropPrefix*)out;
    memset(out, 0, HIP_DEVICE_PROP_R0600_SIZE);
    if (device < 0 || device >= DEVICES)
        return 101; /* hipErrorInvalidDevice */
    strcpy(properties->name, devices[device].name);
    strcpy(properties->gcnArchName, devices[device].target);
    properties->multiProcessorCount = devices[device].compute_units;
    properties->integrated = devices[device].integrated;
    memcpy(properties->luid, &devices[device].luid_low, 4); /* LowPart, then HighPart 0 */
    return 0;
}
EXPORT int hipStreamWaitValue32(void* stream, void* pointer, unsigned value, unsigned flags, unsigned mask)
{
    (void)stream, (void)pointer, (void)value, (void)flags, (void)mask;
    return 0;
}

/* what the interop calls received; field offsets as in hip_runtime_api.h on x86-64 */
static struct
{
    int memory_type;
    void* memory_handle;
    uint64_t memory_size;
    unsigned int memory_flags;
    uint64_t mapped_size;
    int memory_destroyed;
    int semaphore_type;
    void* semaphore_handle;
    uint64_t waited, signalled;
    void* wait_stream;
    int semaphore_destroyed;
} interop = {.wait_stream = (void*)1};

EXPORT int hipImportExternalMemory(void** out, const unsigned char* desc)
{
    memcpy(&interop.memory_type, desc + 0, 4);
    memcpy(&interop.memory_handle, desc + 8, 8);
    memcpy(&interop.memory_size, desc + 24, 8);
    memcpy(&interop.memory_flags, desc + 32, 4);
    *out = &interop;
    return interop.memory_type == 5 || interop.memory_type == 4 || interop.memory_type == 2 ? 0 : 1;
}
EXPORT int hipExternalMemoryGetMappedBuffer(void** device, void* memory, const unsigned char* desc)
{
    (void)memory;
    memcpy(&interop.mapped_size, desc + 8, 8); /* {offset, size, flags} */
    *device = calloc(1, (size_t)interop.mapped_size); /* host memory, as the mock ZLUDA's "device" memory */
    return *device != NULL ? 0 : 2;
}
EXPORT int hipDestroyExternalMemory(void* memory) { interop.memory_destroyed = memory == &interop; return 0; }
EXPORT int hipImportExternalSemaphore(void** out, const unsigned char* desc)
{
    memcpy(&interop.semaphore_type, desc + 0, 4);
    memcpy(&interop.semaphore_handle, desc + 8, 8);
    *out = &interop.semaphore_type;
    return interop.semaphore_type == 4 ? 0 : 1;
}
EXPORT int hipWaitExternalSemaphoresAsync(void* const* semaphores, const unsigned char* params, unsigned count,
                                          void* stream)
{
    if (count != 1 || semaphores[0] != &interop.semaphore_type)
        return 1;
    memcpy(&interop.waited, params + 0, 8);
    interop.wait_stream = stream;
    return 0;
}
EXPORT int hipSignalExternalSemaphoresAsync(void* const* semaphores, const unsigned char* params, unsigned count,
                                            void* stream)
{
    (void)stream;
    if (count != 1 || semaphores[0] != &interop.semaphore_type)
        return 1;
    memcpy(&interop.signalled, params + 0, 8);
    return 0;
}
EXPORT int hipDestroyExternalSemaphore(void* semaphore)
{
    interop.semaphore_destroyed = semaphore == &interop.semaphore_type;
    return 0;
}

EXPORT void mock_hip_interop(int* memory_type, void** memory_handle, uint64_t* memory_size, unsigned int* memory_flags,
                             uint64_t* mapped_size, int* memory_destroyed, int* semaphore_type, void** semaphore_handle,
                             uint64_t* waited, uint64_t* signalled, void** wait_stream, int* semaphore_destroyed)
{
    *memory_type = interop.memory_type, *memory_handle = interop.memory_handle, *memory_size = interop.memory_size;
    *memory_flags = interop.memory_flags, *mapped_size = interop.mapped_size;
    *memory_destroyed = interop.memory_destroyed, *semaphore_type = interop.semaphore_type;
    *semaphore_handle = interop.semaphore_handle, *waited = interop.waited, *signalled = interop.signalled;
    *wait_stream = interop.wait_stream, *semaphore_destroyed = interop.semaphore_destroyed;
}
