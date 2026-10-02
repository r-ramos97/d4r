/* d4r's Windows NVAPI (tools/d4r_nvapi_windows.c) as OptiScaler and NGX query it.
   usage: nvapi_test.exe NVAPI64_DLL */
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef void* (*QueryFn)(unsigned int);
typedef int (*NoArgsFn)(void);
typedef int (*StringFn)(char*);
typedef int (*EnumFn)(void**, unsigned int*);
typedef int (*HandleStructFn)(void*, void*);
typedef int (*DriverFn)(unsigned int*, char*);

static int failures;

static void check(int condition, const char* name)
{
    printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    failures += !condition;
}

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    HMODULE nvapi = LoadLibraryA(argv[1]);
    check(nvapi != NULL, "nvapi64.dll loads");
    if (nvapi == NULL)
        return 1;
    QueryFn query = (QueryFn)(void*)GetProcAddress(nvapi, "nvapi_QueryInterface");
    check(query != NULL, "nvapi_QueryInterface exported");
    if (query == NULL)
        return 1;

    check(((NoArgsFn)query(0x0150e828))() == 0, "NvAPI_Initialize");
    char text[64] = {0};
    check(((StringFn)query(0x01053fa5))(text) == 0 && strstr(text, "DXVK") == NULL && strstr(text, "NVIDIA") != NULL,
          "interface version string reads as NVIDIA's, not DXVK's");
    void* gpus[64] = {0};
    unsigned int count = 0;
    check(((EnumFn)query(0xe5ac921f))(gpus, &count) == 0 && count == 1 && gpus[0] != NULL, "one physical GPU");

    unsigned int arch[4] = {(unsigned int)(16 | (2 << 16)), 0, 0, 0};
    check(((HandleStructFn)query(0xd8265d24))(gpus[0], arch) == 0 && arch[1] == 0x190, "GetArchInfo reports AD100");
    unsigned int bad[4] = {(unsigned int)(12 | (2 << 16)), 0, 0, 0};
    check(((HandleStructFn)query(0xd8265d24))(gpus[0], bad) == -9, "wrong struct version refused");
    check(((HandleStructFn)query(0xd8265d24))((void*)0x1234, arch) == -101, "unknown GPU handle refused");

    unsigned int driver = 0;
    char branch[64] = {0};
    check(((DriverFn)query(0x2926aaad))(&driver, branch) == 0 && driver == 59636 && strcmp(branch, "r596_00") == 0,
          "driver 596.36 by default");

    LUID luid = {0};
    check(((HandleStructFn)query(0x0ff07fde))(gpus[0], &luid) == 0 && luid.HighPart == 0x1 && luid.LowPart == 0xabcd,
          "adapter LUID (D4R_NVAPI_LUID)");

    /* NGX's own queries, answered as dxvk-nvapi does under Proton */
    typedef int (*StructFn)(void*);
    unsigned char overrideState[64] = {0};
    const unsigned int overrideVersion = 64 | (1 << 16);
    memcpy(overrideState, &overrideVersion, 4);
    unsigned long long feedback = 0;
    check(((StructFn)query(0x3fd96fba))(overrideState) == 0 && (memcpy(&feedback, overrideState + 8, 8), feedback == 0x10000),
          "NGX override state: none (ERR_FAILED feedback for super resolution)");
    unsigned char features[288] = {0};
    const unsigned int featureVersion = 288 | (1 << 16), featureCount = 2, flipConfig = 3423695, other = 7;
    memcpy(features, &featureVersion, 4);
    memcpy(features + 4, &featureCount, 4);
    memcpy(features + 8, &flipConfig, 4);
    memcpy(features + 24, &other, 4);
    features[12] = features[28] = 0xfe; /* reserved bits above bSupported must survive */
    check(((StructFn)query(0x6194b19d))(features) == 0 && features[12] == 0xff && features[28] == 0xfe,
          "NGX driver feature support: flip configuration only");
    unsigned char driverInfo[144] = {0};
    const unsigned int driverInfoVersion = 144 | (2 << 16);
    memcpy(driverInfo, &driverInfoVersion, 4);
    unsigned int driverFlags = 0, driverVersion = 0;
    check(((StructFn)query(0x721faceb))(driverInfo) == 0 && (memcpy(&driverVersion, driverInfo + 4, 4), driverVersion == 59636) &&
              (memcpy(&driverFlags, driverInfo + 72, 4), driverFlags == 5),
          "display driver info: 596.36, DCH, Game Ready");
    unsigned char topology[136] = {0};
    const unsigned int topologyVersion = 136 | (1 << 16);
    memcpy(topology, &topologyVersion, 4);
    void* computeGpu = NULL;
    unsigned int computeCount = 0, computeFlags = 0;
    check(((StructFn)query(0x5786cc6e))(topology) == 0 && (memcpy(&computeCount, topology + 4, 4), computeCount == 1) &&
              (memcpy(&computeGpu, topology + 8, 8), computeGpu == gpus[0]) &&
              (memcpy(&computeFlags, topology + 16, 4), computeFlags == 0x0b),
          "one CUDA-capable GPU, the physical one");
    unsigned int memory[8] = {(unsigned int)(32 | (3 << 16))};
    const int memoryResult = ((HandleStructFn)query(0x07f9b368))(gpus[0], memory);
    check(memoryResult == 0 || memoryResult == -6, "memory info answers (or no such adapter here)");

    check(query(0x31aa0ab2) == NULL, "unimplemented interface (a D3D12 extension) is NULL");
    check(query(0x21382138) == NULL, "not fakenvapi (OptiScaler's check for it)");

    /* the log names what was asked for */
    char log[MAX_PATH];
    GetModuleFileNameA(nvapi, log, sizeof(log));
    strcpy(strrchr(log, '\\') + 1, "d4r_nvapi.log");
    FILE* file = fopen(log, "r");
    char contents[8192] = {0};
    if (file != NULL)
    {
        fread(contents, 1, sizeof(contents) - 1, file);
        fclose(file);
    }
    check(strstr(contents, "0xd8265d24 NvAPI_GPU_GetArchInfo: d4r") != NULL, "log names implemented interfaces");
    check(strstr(contents, "0x31aa0ab2") != NULL && strstr(contents, "unimplemented") != NULL,
          "log names unimplemented interfaces");
    printf("%d failure(s)\n", failures);
    return failures != 0;
}
