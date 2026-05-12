// d2sedit — read/write the map seed in a Diablo II .d2s save file.
//
// Layout (verified empirically against 1.13c and D2R saves):
//   0x00  DWORD  signature  = 0xAA55AA55
//   0x04  DWORD  version    = 0x60 (1.13c), 0x61..0x63 (1.14), 0x69 (D2R)
//   0x08  DWORD  file size
//   0x0C  DWORD  checksum   (zero these 4 bytes when computing)
//   ...
//   ????  DWORD  map seed   ← target (offset depends on version, see below)
//
// Seed offset varies because D2R shortened the menu-appearance block:
//   1.13c / 1.14 (0x60..0x63): seed at 0xAB
//   D2R         (0x69):        seed at 0x9B
// Header signature and the checksum algorithm are the same in all versions.
//
// Checksum: c=0; for each byte b in file: c = rol(c,1) + b  (32-bit wrap).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr size_t kChecksumOff = 0x0C;
constexpr size_t kVersionOff  = 0x04;
constexpr uint32_t kSignature = 0xAA55AA55u;

size_t seed_offset_for_version(uint32_t version) {
    // D2R (0x69+) trimmed the pre-seed header; classic engines keep it at 0xAB.
    return version >= 0x69 ? 0x9B : 0xAB;
}

bool read_file(const char* path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::perror(path); return false; }
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(static_cast<size_t>(n));
    bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    if (!ok) std::fprintf(stderr, "%s: short read\n", path);
    return ok;
}

bool write_file(const char* path, const std::vector<uint8_t>& data) {
    FILE* f = std::fopen(path, "wb");
    if (!f) { std::perror(path); return false; }
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    return ok;
}

uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
}
void wr32(uint8_t* p, uint32_t v) {
    p[0]=uint8_t(v); p[1]=uint8_t(v>>8); p[2]=uint8_t(v>>16); p[3]=uint8_t(v>>24);
}

uint32_t compute_checksum(const std::vector<uint8_t>& data) {
    uint32_t c = 0;
    for (size_t i = 0; i < data.size(); ++i) {
        uint8_t b = (i >= kChecksumOff && i < kChecksumOff + 4) ? 0 : data[i];
        c = (c << 1) | (c >> 31);
        c += b;
    }
    return c;
}

bool parse_u32(const char* s, uint32_t& out) {
    char* end = nullptr;
    unsigned long long v = std::strtoull(s, &end, 0);  // 0 = auto-detect 0x prefix
    if (end == s || *end != '\0' || v > 0xFFFFFFFFull) return false;
    out = static_cast<uint32_t>(v);
    return true;
}

int usage() {
    std::fprintf(stderr,
        "Usage:\n"
        "  d2sedit <file.d2s>              print map seed\n"
        "  d2sedit <file.d2s> <new_seed>   set map seed (decimal or 0xHEX) and update checksum\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) return usage();

    std::vector<uint8_t> data;
    if (!read_file(argv[1], data)) return 1;

    if (data.size() < kVersionOff + 4) {
        std::fprintf(stderr, "%s: file too small (%zu bytes)\n", argv[1], data.size());
        return 1;
    }
    uint32_t sig = rd32(data.data());
    if (sig != kSignature) {
        std::fprintf(stderr, "%s: bad signature 0x%08X (expected 0x%08X)\n", argv[1], sig, kSignature);
        return 1;
    }

    uint32_t version  = rd32(&data[kVersionOff]);
    size_t   seedOff  = seed_offset_for_version(version);
    if (data.size() < seedOff + 4) {
        std::fprintf(stderr, "%s: file too small for version 0x%X seed offset 0x%zX (%zu bytes)\n",
                     argv[1], version, seedOff, data.size());
        return 1;
    }
    uint32_t stored_c = rd32(&data[kChecksumOff]);
    uint32_t seed     = rd32(&data[seedOff]);
    uint32_t calc_c   = compute_checksum(data);

    if (argc == 2) {
        std::printf("file:      %s\n", argv[1]);
        std::printf("size:      %zu bytes\n", data.size());
        std::printf("version:   0x%08X\n", version);
        std::printf("checksum:  0x%08X (%s)\n", stored_c, stored_c == calc_c ? "ok" : "MISMATCH");
        std::printf("map seed:  %u (0x%08X)\n", seed, seed);
        return 0;
    }

    uint32_t new_seed;
    if (!parse_u32(argv[2], new_seed)) {
        std::fprintf(stderr, "bad seed value: %s\n", argv[2]);
        return 2;
    }

    wr32(&data[seedOff], new_seed);
    uint32_t new_c = compute_checksum(data);
    wr32(&data[kChecksumOff], new_c);

    if (!write_file(argv[1], data)) return 1;

    std::printf("seed:      %u -> %u (0x%08X -> 0x%08X)\n", seed, new_seed, seed, new_seed);
    std::printf("checksum:  0x%08X -> 0x%08X\n", stored_c, new_c);
    return 0;
}
