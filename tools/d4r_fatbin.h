/* CUDA fatbin and PTX helpers shared by the nvcuda bridge (which checks the modules NGX loads against the native
   kernel manifest) and d4r-manifest (tools/d4r_manifest.c, which writes that manifest from nvngx_dlss.dll), so both
   hash a module's PTX the same way. kernels/tools/kernel_manifest.py is the Python equivalent. */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum
{
    D4R_FATBIN_MAGIC = 0xba55ed50u,
    D4R_FATBIN_PTX = 1,
    D4R_FATBIN_LZ4 = 0x2000,
};

/* LZ4 block decoder for compressed fatbin entries; returns the decoded size, 0 on bad input. */
static size_t d4r_lz4_block(const unsigned char* src, size_t src_size, unsigned char* dst, size_t dst_size)
{
    size_t i = 0, o = 0;
    while (i < src_size)
    {
        const unsigned int token = src[i++];
        size_t literals = token >> 4;
        if (literals == 15)
        {
            unsigned char b;
            do
            {
                if (i >= src_size)
                    return 0;
                b = src[i++];
                literals += b;
            } while (b == 255);
        }
        if (literals > src_size - i || literals > dst_size - o)
            return 0;
        memcpy(dst + o, src + i, literals);
        i += literals;
        o += literals;
        if (i >= src_size || o >= dst_size)
            break;
        if (src_size - i < 2)
            return 0;
        const size_t offset = src[i] | ((size_t)src[i + 1] << 8);
        i += 2;
        if (offset == 0 || offset > o)
            return 0;
        size_t match = token & 15;
        if (match == 15)
        {
            unsigned char b;
            do
            {
                if (i >= src_size)
                    return 0;
                b = src[i++];
                match += b;
            } while (b == 255);
        }
        match += 4;
        if (match > dst_size - o)
            match = dst_size - o;
        for (size_t k = 0; k < match; ++k, ++o)
            dst[o] = dst[o - offset];
    }
    return o;
}

/* FNV-1a 64 of a PTX module's text without its trailing NUL bytes: the hash d4r-kernels.txt lists. */
static uint64_t d4r_ptx_hash(const unsigned char* text, size_t* size)
{
    while (*size > 0 && text[*size - 1] == '\0')
        --*size;
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < *size; ++i)
        hash = (hash ^ text[i]) * 0x100000001b3ull;
    return hash;
}

static int d4r_is_ptx_name_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$';
}

/* The next ".entry NAME(" from *position on: copies NAME (when shorter than name_size) and returns 1, or 0 at the
   end of the text. */
static int d4r_next_ptx_entry(const unsigned char* text, size_t size, size_t* position, char* name, size_t name_size)
{
    for (size_t i = *position; i + 7 < size; ++i)
    {
        if (memcmp(text + i, ".entry", 6) != 0 || (text[i + 6] != ' ' && text[i + 6] != '\t' && text[i + 6] != '\n'))
            continue;
        size_t p = i + 6;
        while (p < size && (text[p] == ' ' || text[p] == '\t' || text[p] == '\n' || text[p] == '\r'))
            ++p;
        const size_t start = p;
        while (p < size && d4r_is_ptx_name_char(text[p]))
            ++p;
        const size_t length = p - start;
        while (p < size && (text[p] == ' ' || text[p] == '\t' || text[p] == '\n' || text[p] == '\r'))
            ++p;
        if (length == 0 || length >= name_size || p >= size || text[p] != '(')
            continue;
        memcpy(name, text + start, length);
        name[length] = '\0';
        *position = p;
        return 1;
    }
    *position = size;
    return 0;
}

typedef void (*D4rPtxVisitor)(const unsigned char* text, size_t size, void* context);

/* Calls visit for every PTX entry of the CUDA fatbin v1 container at image (decoding LZ4-compressed entries). The
   container must fit in available bytes (SIZE_MAX when unknown). Returns 0 when image is no such container. */
static int d4r_fatbin_ptx(const unsigned char* image, size_t available, D4rPtxVisitor visit, void* context)
{
    uint32_t magic = 0;
    uint16_t version = 0, header_size = 0;
    uint64_t files_size = 0;
    if (available < 16)
        return 0;
    memcpy(&magic, image, sizeof(magic));
    memcpy(&version, image + 4, sizeof(version));
    memcpy(&header_size, image + 6, sizeof(header_size));
    memcpy(&files_size, image + 8, sizeof(files_size));
    if (magic != D4R_FATBIN_MAGIC || version != 1 || header_size < 16 || header_size > 4096 ||
        files_size > 256u * 1024u * 1024u || (uint64_t)header_size + files_size > available)
        return 0;
    size_t offset = header_size;
    const size_t end = (size_t)header_size + (size_t)files_size;
    while (offset + 64 <= end)
    {
        uint16_t kind = 0;
        uint32_t entry_header = 0;
        uint64_t entry_size = 0, flags = 0, decompressed = 0;
        memcpy(&kind, image + offset, sizeof(kind));
        memcpy(&entry_header, image + offset + 4, sizeof(entry_header));
        memcpy(&entry_size, image + offset + 8, sizeof(entry_size));
        memcpy(&flags, image + offset + 40, sizeof(flags));
        if (entry_header >= 64)
            memcpy(&decompressed, image + offset + 56, sizeof(decompressed));
        if (entry_header < 16 || entry_header > end - offset || entry_size > end - offset - entry_header)
            break;
        const unsigned char* payload = image + offset + entry_header;
        if (kind == D4R_FATBIN_PTX)
        {
            if ((flags & D4R_FATBIN_LZ4) != 0 && decompressed != 0 && decompressed <= 256u * 1024u * 1024u)
            {
                unsigned char* text = (unsigned char*)malloc((size_t)decompressed);
                const size_t size = text != NULL ? d4r_lz4_block(payload, (size_t)entry_size, text, (size_t)decompressed) : 0;
                if (size != 0)
                    visit(text, size, context);
                free(text);
            }
            else
                visit(payload, (size_t)entry_size, context);
        }
        offset += entry_header + entry_size;
    }
    return 1;
}
