#pragma once
#include "sdk.h"
#include "offsets.hpp"
#include <windows.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Offsets {

inline bool ParseFromFile(const char* filename) {
    HANDLE file = CreateFileA(filename, GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        cs2::Log("OffsetParser: file '%s' not found, using compiled defaults", filename);
        return true;
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart <= 0 || fileSize.QuadPart > 1024 * 1024) {
        CloseHandle(file);
        return true;
    }

    HANDLE mapping = CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) {
        CloseHandle(file);
        return true;
    }

    LPVOID mapped = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!mapped) {
        CloseHandle(mapping);
        CloseHandle(file);
        return true;
    }

    std::vector<char> localCopy(fileSize.QuadPart + 1, 0);
    memcpy(localCopy.data(), mapped, static_cast<size_t>(fileSize.QuadPart));
    UnmapViewOfFile(mapped);
    CloseHandle(mapping);
    CloseHandle(file);

    char* data = localCopy.data();

    cs2::Log("[+] OffsetParser: parsing offsets from '%s'", filename);

    int parsed = 0;
    int matched = 0;
    char* ctx = nullptr;
    const char* line = strtok_s(data, "\n", &ctx);
    while (line) {
        while (*line == ' ' || *line == '\t') line++;

        if (*line == '=' || *line == '\0') { line = strtok_s(nullptr, "\n", &ctx); continue; }

        if (strncmp(line, "=== ", 4) == 0 || strncmp(line, "NOT FOUND", 9) == 0) {
            line = strtok_s(nullptr, "\n", &ctx);
            continue;
        }

        const char* colon = strchr(line, ':');
        if (!colon) { line = strtok_s(nullptr, "\n", &ctx); continue; }

        // Parse the number BEFORE the colon (dump format: "5488: m_pBulletServices")
        char* endPtr = nullptr;
        std::ptrdiff_t decimalOffset = static_cast<std::ptrdiff_t>(strtoull(line, &endPtr, 10));
        if (endPtr == line || decimalOffset == 0) {
            line = strtok_s(nullptr, "\n", &ctx);
            continue;
        }
        parsed++;

        // Parse the name AFTER the colon
        const char* nameStart = colon + 1;
        while (*nameStart == ' ' || *nameStart == '\t') nameStart++;

        char nameBuf[128];
        size_t nameLen = 0;
        while (nameStart[nameLen] && nameStart[nameLen] != '\r' && nameStart[nameLen] != '\n' &&
               nameStart[nameLen] != ' ' && nameStart[nameLen] != '\t' && nameLen < sizeof(nameBuf) - 1) {
            nameBuf[nameLen++] = nameStart[nameLen];
        }
        nameBuf[nameLen] = '\0';

        for (auto& entry : g_offsetTable) {
            if (std::strcmp(entry.name, nameBuf) == 0) {
                *entry.actual = decimalOffset;
                matched++;
                break;
            }
        }

        line = strtok_s(nullptr, "\n", &ctx);
    }

    cs2::Log("[+] OffsetParser: parsed %d fields, updated %d known offsets", parsed, matched);
    if (matched == 0) {
        cs2::Log("[WARNING] OffsetParser: 0 offsets matched from dump - parser format mismatch or stale dump");
    }

    return true;
}

// include/ is the single source of truth for all offsets — it is generated for
// this build. Stale dump files on disk (e.g. astral_offsets.txt) previously
// overwrote verified constants at startup with older RVAs, which silently broke
// entity resolution and could corrupt game memory. Parsing is now inert.
inline bool ParseFromGamehacksFolder() {
    return true;
}

} // namespace Offsets
