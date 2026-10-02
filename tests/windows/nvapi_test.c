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
    check(((DriverFn)query(0x2926aaad))(&driver, branch) == 0 && driver == 58142 && strcmp(branch, "r581_00") == 0,
          "driver 581.42 by default");

    LUID luid = {0};
    check(((HandleStructFn)query(0x0ff07fde))(gpus[0], &luid) == 0 && luid.HighPart == 0x1 && luid.LowPart == 0xabcd,
          "adapter LUID (D4R_NVAPI_LUID)");

    check(query(0x31aa0ab2) == NULL, "unimplemented interface (a D3D12 extension) is NULL");

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
