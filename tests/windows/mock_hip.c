/* A stand-in for the HIP SDK's amdhip64_7.dll: a Ryzen 7000 integrated GPU (gfx1036) listed before an
   RX 9070 XT (gfx1201), as HIP reports a desktop with its iGPU enabled. */
#include <windows.h>
#include <string.h>
#include "d4r_hip_props.h"

#define EXPORT __declspec(dllexport)

EXPORT int hipGetDeviceCount(int* count) { *count = 2; return 0; }
EXPORT int hipGetDevicePropertiesR0600(void* out, int device)
{
    D4rHipDevicePropPrefix* properties = (D4rHipDevicePropPrefix*)out;
    memset(out, 0, HIP_DEVICE_PROP_R0600_SIZE);
    if (device == 0)
    {
        strcpy(properties->name, "AMD Radeon(TM) Graphics");
        strcpy(properties->gcnArchName, "gfx1036");
        properties->multiProcessorCount = 1;
        properties->integrated = 1;
    }
    else if (device == 1)
    {
        strcpy(properties->name, "AMD Radeon RX 9070 XT");
        strcpy(properties->gcnArchName, "gfx1201:sramecc-:xnack-");
        properties->multiProcessorCount = 32; /* HIP counts WGPs on RDNA */
    }
    else
        return 101; /* hipErrorInvalidDevice */
    return 0;
}
EXPORT int hipStreamWaitValue32(void* stream, void* pointer, unsigned value, unsigned flags, unsigned mask)
{
    (void)stream, (void)pointer, (void)value, (void)flags, (void)mask;
    return 0;
}
