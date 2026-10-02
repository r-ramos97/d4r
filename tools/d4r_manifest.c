/* d4r-manifest: writes the d4r-kernels.txt manifest of every native kernel folder of an install, like
   kernels/tools/kernel_manifest.py but without Python (it builds for Windows with MinGW-w64, and for Linux).

   usage: d4r-manifest [--force] KERNELS_DIR NVNGX_DLSS_DLL [NVNGX_DLSS_DLL...]

   KERNELS_DIR is an install's d4r/kernels folder: each folder in it, and in its accuracy folder, that holds .hsaco
   files gets a manifest, and so does KERNELS_DIR itself when it holds .hsaco files. For every NAME.hsaco the
   manifest lists the FNV-1a 64 hash of each DLL's PTX module that defines `.entry NAME`. The nvcuda bridge serves a
   native kernel only while DLSS loads a module with a listed hash (docs/native-kernels.md).

   The native kernels were written for the kernels of DLSS 310.7 and 310.9. A manifest written from another DLSS
   version would list that version's changed kernels as matching, so other versions are refused unless --force. */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "d4r_fatbin.h"

typedef struct
{
    char name[128];
    uint64_t hash;
} Entry;

typedef struct
{
    Entry* items;
    size_t count, capacity;
} Entries;

static void add_entry(Entries* entries, const char* name, uint64_t hash)
{
    for (size_t i = 0; i < entries->count; ++i)
        if (entries->items[i].hash == hash && strcmp(entries->items[i].name, name) == 0)
            return;
    if (entries->count == entries->capacity)
    {
        entries->capacity = entries->capacity != 0 ? entries->capacity * 2 : 256;
        entries->items = (Entry*)realloc(entries->items, entries->capacity * sizeof(Entry));
        if (entries->items == NULL)
        {
            fprintf(stderr, "d4r-manifest: out of memory\n");
            exit(1);
        }
    }
    Entry* entry = &entries->items[entries->count++];
    snprintf(entry->name, sizeof(entry->name), "%s", name);
    entry->hash = hash;
}

static void collect_module(const unsigned char* text, size_t size, void* context)
{
    const uint64_t hash = d4r_ptx_hash(text, &size);
    char name[128];
    for (size_t position = 0; d4r_next_ptx_entry(text, size, &position, name, sizeof(name));)
        add_entry((Entries*)context, name, hash);
}

static unsigned char* read_file(const char* path, size_t* size)
{
    FILE* file = fopen(path, "rb");
    if (file == NULL)
        return NULL;
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    unsigned char* data = length > 0 ? (unsigned char*)malloc((size_t)length) : NULL;
    if (data == NULL || fread(data, 1, (size_t)length, file) != (size_t)length)
    {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

/* The file version of a PE image's VS_FIXEDFILEINFO; 0 when it has none. */
static int file_version(const unsigned char* data, size_t size, unsigned int version[4])
{
    for (size_t i = 0; i + 16 <= size; i += 4)
    {
        uint32_t signature, ms, ls;
        memcpy(&signature, data + i, 4);
        if (signature != 0xfeef04bdu)
            continue;
        memcpy(&ms, data + i + 8, 4);
        memcpy(&ls, data + i + 12, 4);
        version[0] = ms >> 16, version[1] = ms & 0xffff, version[2] = ls >> 16, version[3] = ls & 0xffff;
        return 1;
    }
    return 0;
}

/* Every fatbin container (version 1, 16-byte header) in file order, as extract_dlss_ptx.py finds them. */
static void collect_dll(const unsigned char* data, size_t size, Entries* entries)
{
    const unsigned char magic[4] = {0x50, 0xed, 0x55, 0xba};
    size_t start = 0;
    while (start + 16 <= size)
    {
        const unsigned char* found = NULL;
        for (size_t i = start; i + 4 <= size; ++i)
            if (memcmp(data + i, magic, 4) == 0)
            {
                found = data + i;
                break;
            }
        if (found == NULL)
            break;
        const size_t offset = (size_t)(found - data);
        uint16_t version = 0, header_size = 0;
        uint64_t files_size = 0;
        if (offset + 16 <= size)
        {
            memcpy(&version, found + 4, 2);
            memcpy(&header_size, found + 6, 2);
            memcpy(&files_size, found + 8, 8);
        }
        if (offset + 16 <= size && version == 1 && header_size == 16 && files_size <= size - offset - 16 &&
            d4r_fatbin_ptx(found, size - offset, collect_module, entries))
            start = offset + 16 + (size_t)files_size;
        else
            start = offset + 4;
    }
}

static int compare_names(const void* a, const void* b)
{
    return strcmp(*(char* const*)a, *(char* const*)b);
}

static int compare_hashes(const void* a, const void* b)
{
    const uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}

static int is_directory(const char* path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

/* Writes directory/d4r-kernels.txt; 1 when written, 0 when the folder has no .hsaco files, -1 on failure. */
static int write_manifest(const char* directory, const Entries* entries, const char* sources)
{
    DIR* listing = opendir(directory);
    if (listing == NULL)
        return 0;
    char** names = NULL;
    size_t count = 0, capacity = 0;
    for (struct dirent* item; (item = readdir(listing)) != NULL;)
    {
        const size_t length = strlen(item->d_name);
        if (length <= 6 || strcmp(item->d_name + length - 6, ".hsaco") != 0)
            continue;
        if (count == capacity)
        {
            capacity = capacity != 0 ? capacity * 2 : 64;
            names = (char**)realloc(names, capacity * sizeof(char*));
        }
        names[count] = (char*)malloc(length - 5);
        memcpy(names[count], item->d_name, length - 6);
        names[count][length - 6] = '\0';
        ++count;
    }
    closedir(listing);
    if (count == 0)
    {
        free(names);
        return 0;
    }
    qsort(names, count, sizeof(char*), compare_names);

    int result = 1;
    char missing[2048] = {0};
    for (size_t n = 0; n < count; ++n)
    {
        int found = 0;
        for (size_t i = 0; i < entries->count && !found; ++i)
            found = strcmp(entries->items[i].name, names[n]) == 0;
        if (!found)
        {
            strncat(missing, missing[0] != '\0' ? ", " : "", sizeof(missing) - strlen(missing) - 1);
            strncat(missing, names[n], sizeof(missing) - strlen(missing) - 1);
        }
    }
    if (missing[0] != '\0')
    {
        fprintf(stderr, "d4r-manifest: %s: no DLL defines %s; not writing a manifest\n", directory, missing);
        result = -1;
    }
    else
    {
        char path[4096];
        snprintf(path, sizeof(path), "%s/d4r-kernels.txt", directory);
        FILE* out = fopen(path, "wb");
        if (out == NULL)
        {
            fprintf(stderr, "d4r-manifest: cannot write %s\n", path);
            result = -1;
        }
        else
        {
            fprintf(out, "# d4r native kernels: NAME and the FNV-1a 64 hash of the DLSS PTX module each was written for\n");
            fprintf(out, "# (written by d4r-manifest from:\n%s", sources);
            for (size_t n = 0; n < count; ++n)
            {
                uint64_t hashes[64];
                size_t listed = 0;
                for (size_t i = 0; i < entries->count && listed < 64; ++i)
                    if (strcmp(entries->items[i].name, names[n]) == 0)
                        hashes[listed++] = entries->items[i].hash;
                qsort(hashes, listed, sizeof(hashes[0]), compare_hashes);
                for (size_t h = 0; h < listed; ++h)
                    fprintf(out, "%s %016llx\n", names[n], (unsigned long long)hashes[h]);
            }
            fclose(out);
            printf("%zu kernels listed in %s\n", count, path);
        }
    }
    for (size_t n = 0; n < count; ++n)
        free(names[n]);
    free(names);
    return result;
}

static int write_folder_manifests(const char* root, const Entries* entries, const char* sources, int* written)
{
    int failed = 0;
    int result = write_manifest(root, entries, sources);
    failed |= result < 0;
    *written += result > 0;
    DIR* listing = opendir(root);
    if (listing == NULL)
        return failed;
    for (struct dirent* item; (item = readdir(listing)) != NULL;)
    {
        if (item->d_name[0] == '.')
            continue;
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", root, item->d_name);
        if (!is_directory(path))
            continue;
        if (strcmp(item->d_name, "accuracy") == 0)
            failed |= write_folder_manifests(path, entries, sources, written);
        else
        {
            result = write_manifest(path, entries, sources);
            failed |= result < 0;
            *written += result > 0;
        }
    }
    closedir(listing);
    return failed;
}

int main(int argc, char** argv)
{
    int force = 0, first = 1;
    if (argc > 1 && strcmp(argv[1], "--force") == 0)
        force = 1, first = 2;
    if (argc - first < 2)
    {
        fprintf(stderr, "usage: d4r-manifest [--force] KERNELS_DIR NVNGX_DLSS_DLL [NVNGX_DLSS_DLL...]\n");
        return 2;
    }
    Entries entries = {0};
    char sources[4096] = {0};
    for (int i = first + 1; i < argc; ++i)
    {
        size_t size = 0;
        unsigned char* data = read_file(argv[i], &size);
        if (data == NULL)
        {
            fprintf(stderr, "d4r-manifest: cannot read %s\n", argv[i]);
            return 2;
        }
        unsigned int version[4] = {0};
        const int has_version = file_version(data, size, version);
        const int verified = has_version && version[0] == 310 && (version[1] == 7 || version[1] == 9);
        if (!verified && !force)
        {
            if (has_version)
                fprintf(stderr, "d4r-manifest: %s is DLSS %u.%u.%u.%u; d4r's native kernels were written for 310.7 and "
                        "310.9, and a manifest from another version could serve them for changed kernels. Use a 310.7 "
                        "or 310.9 nvngx_dlss.dll here (any version works in the game), or --force.\n",
                        argv[i], version[0], version[1], version[2], version[3]);
            else
                fprintf(stderr, "d4r-manifest: %s has no version information (not nvngx_dlss.dll?); use --force to "
                        "accept it\n", argv[i]);
            free(data);
            return 2;
        }
        const size_t before = entries.count;
        collect_dll(data, size, &entries);
        free(data);
        const char* base = strrchr(argv[i], '/');
        const char* back = strrchr(argv[i], '\\');
        base = back != NULL && (base == NULL || back > base) ? back : base;
        char line[512];
        snprintf(line, sizeof(line), "#   %s %u.%u.%u.%u)\n", base != NULL ? base + 1 : argv[i], version[0], version[1],
                 version[2], version[3]);
        strncat(sources, line, sizeof(sources) - strlen(sources) - 1);
        printf("%s: DLSS %u.%u.%u.%u, %zu kernel entries\n", argv[i], version[0], version[1], version[2], version[3],
               entries.count - before);
    }
    int written = 0;
    const int failed = write_folder_manifests(argv[first], &entries, sources, &written);
    free(entries.items);
    if (written == 0 && !failed)
    {
        fprintf(stderr, "d4r-manifest: no .hsaco files in %s or its folders\n", argv[first]);
        return 2;
    }
    return failed ? 1 : 0;
}
