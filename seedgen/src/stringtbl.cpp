#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <Windows.h>
#include "stringtbl.h"
#include "d2loader.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace MapData {
namespace {

// .tbl layout from plugy/Commons/d2StringTblStruct.h. Packed; sizes 0x15/0x11.
#pragma pack(push, 1)
struct TblHeader {
    uint16_t usCRC;            // +00
    uint16_t numElements;      // +02
    uint32_t hashTableSize;    // +04
    uint8_t  version;          // +08
    uint32_t indexStart;       // +09 — file offset to indexed entries
    uint32_t numLoops;         // +0D
    uint32_t indexEnd;         // +11 — file length
};
struct TblNode {
    uint8_t  active;           // +00
    uint16_t keyIndex;         // +01
    uint32_t hashValue;        // +03
    uint32_t keyOffset;        // +07 — file offset to NUL-terminated ASCII key
    uint32_t stringOffset;     // +0B — file offset to NUL-terminated UTF-16 value
    uint16_t stringLength;     // +0F — bytes
};
#pragma pack(pop)
static_assert(sizeof(TblHeader) == 0x15, "TblHeader size");
static_assert(sizeof(TblNode)   == 0x11, "TblNode size");

// Combined case-insensitive map. Lowercase the key on insert/lookup so we
// don't pay for a custom hash + equal pair.
std::unordered_map<std::string, std::string> g_strings;
const std::string                            g_empty;

std::string LowerAscii(const char* s, size_t n = SIZE_MAX) {
    std::string out;
    for (size_t i = 0; i < n && s[i]; ++i) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        out.push_back(c);
    }
    return out;
}

// Convert D2's stored string-table value to UTF-8. Values are 8-bit bytes
// (Windows-1252 for English/Western); NOT UTF-16, despite what some refs
// suggest. `maxBytes` caps the read at the node's declared StringLength.
// Returns "" on empty / conversion failure.
std::string ValueToUtf8(const char* s, size_t maxBytes) {
    if (!s || maxBytes == 0) return "";
    size_t len = 0;
    while (len < maxBytes && s[len]) ++len;
    if (len == 0) return "";
    // Fast path: pure ASCII passes through as valid UTF-8.
    bool ascii = true;
    for (size_t i = 0; i < len; ++i)
        if (static_cast<unsigned char>(s[i]) >= 0x80) { ascii = false; break; }
    if (ascii) return std::string(s, len);
    // Slow path: go CP-1252 -> UTF-16 -> UTF-8 via Windows.
    const int nW = MultiByteToWideChar(1252, 0, s, static_cast<int>(len),
                                       nullptr, 0);
    if (nW <= 0) return "";
    std::vector<wchar_t> wbuf(static_cast<size_t>(nW));
    MultiByteToWideChar(1252, 0, s, static_cast<int>(len), wbuf.data(), nW);
    const int n8 = WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), nW,
                                       nullptr, 0, nullptr, nullptr);
    if (n8 <= 0) return "";
    std::string out(static_cast<size_t>(n8), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), nW, &out[0], n8, nullptr, nullptr);
    return out;
}

bool ParseTbl(const uint8_t* data, size_t size, int& addedOut) {
    addedOut = 0;
    if (size < sizeof(TblHeader)) return false;
    const auto* hdr = reinterpret_cast<const TblHeader*>(data);
    // Bounds: hashTableSize TblNodes start right after the header + the index
    // table (numElements WORDs).
    const size_t indexBytes = size_t(hdr->numElements) * sizeof(uint16_t);
    const size_t nodesStart = sizeof(TblHeader) + indexBytes;
    const size_t nodesBytes = size_t(hdr->hashTableSize) * sizeof(TblNode);
    if (nodesStart + nodesBytes > size) return false;

    const auto* nodes = reinterpret_cast<const TblNode*>(data + nodesStart);
    for (uint32_t i = 0; i < hdr->hashTableSize; ++i) {
        const TblNode& n = nodes[i];
        if (!n.active) continue;
        if (n.keyOffset >= size || n.stringOffset >= size) continue;

        // Key is NUL-terminated ASCII at keyOffset. Defensively cap at the
        // remaining bytes so a corrupt offset doesn't run off the buffer.
        const char* keyPtr = reinterpret_cast<const char*>(data + n.keyOffset);
        const size_t keyMax = size - n.keyOffset;
        const size_t keyLen = strnlen(keyPtr, keyMax);
        if (keyLen == 0 || keyLen == keyMax) continue;

        // Value is an 8-bit (Windows-1252) byte string at stringOffset,
        // bounded by stringLength. Some "key-only" entries have garbage at
        // stringOffset; bounding by stringLength keeps the read safe.
        if (n.stringLength == 0) continue;
        const size_t bytesAvail = size - n.stringOffset;
        const size_t cap        = std::min<size_t>(n.stringLength, bytesAvail);
        const char* valPtr = reinterpret_cast<const char*>(data + n.stringOffset);
        std::string val = ValueToUtf8(valPtr, cap);

        // Insert lowercase key. If the same key already exists from a higher-
        // priority .tbl (loaded earlier), keep the existing one — D2's
        // priority is patch > expansion > string, which is the order we load.
        std::string lk = LowerAscii(keyPtr, keyLen);
        if (lk.empty()) continue;
        if (g_strings.emplace(std::move(lk), std::move(val)).second) ++addedOut;
    }
    return true;
}

} // anonymous

bool LoadStringTbl(bool verbose) {
    g_strings.clear();

    // Load order matches D2's priority chain: patch overrides expansion
    // overrides base.
    const char* kPaths[] = {
        "data\\local\\lng\\eng\\patchstring.tbl",
        "data\\local\\lng\\eng\\expansionstring.tbl",
        "data\\local\\lng\\eng\\string.tbl",
    };

    int loadedFiles = 0;
    for (const char* path : kPaths) {
        std::vector<uint8_t> raw;
        if (!D2_ReadMpqFile(path, raw)) {
            if (verbose) printf("LoadStringTbl: missing %s\n", path);
            continue;
        }
        int added = 0;
        if (!ParseTbl(raw.data(), raw.size(), added)) {
            fprintf(stderr, "LoadStringTbl: parse error in %s\n", path);
            continue;
        }
        if (verbose) printf("LoadStringTbl: %s -> %d new keys\n", path, added);
        ++loadedFiles;
    }

    if (verbose) printf("LoadStringTbl: %zu unique keys total\n", g_strings.size());
    return loadedFiles > 0;
}

const std::string& LookupString(const char* key) {
    if (!key || !*key || g_strings.empty()) return g_empty;
    std::string lk = LowerAscii(key);
    auto it = g_strings.find(lk);
    return (it == g_strings.end()) ? g_empty : it->second;
}

size_t StringTblSize() { return g_strings.size(); }

} // namespace MapData
