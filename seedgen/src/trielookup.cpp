#define _CRT_SECURE_NO_WARNINGS
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <queue>
#include <string>
#include <thread>
#include <vector>
#include "trielookup.h"
#include "levelgraph_format.h"
#include "mmap_file.h"
#include "tower_rooms.h"

namespace fs = std::filesystem;

namespace {

// Portable fnmatch: '*' matches any sequence (including empty), '?' matches
// any single char. No character classes, no escapes — that's all the glob
// surface we need for level<id>_*_*.bin patterns.
bool FnMatch(const char* pat, const char* name) {
    if (*pat == '*') {
        while (*pat == '*') ++pat;
        if (!*pat) return true;
        for (; *name; ++name) {
            if (FnMatch(pat, name)) return true;
        }
        return false;
    }
    if (!*name) return !*pat;
    if (*pat == '?' || *pat == *name) return FnMatch(pat + 1, name + 1);
    return false;
}

// StairsUp encoded values (variant=0, graves=0, roomId 139..142).
constexpr uint16_t kStairsUpW = 0x2700;
constexpr uint16_t kStairsUpE = 0x2800;
constexpr uint16_t kStairsUpS = 0x2900;
constexpr uint16_t kStairsUpN = 0x2A00;

bool IsStairsUp(uint16_t encoded) {
    const uint16_t e = encoded & 0xFF00;  // strip graves
    return e == kStairsUpW || e == kStairsUpE
        || e == kStairsUpS || e == kStairsUpN;
}

const char* ShapeOf(uint16_t encoded) {
    // Schema lookup via the TowerRoom table. encoded & 0xFF00 strips graves
    // so we hit the (roomId, variant) entry.
    const MapData::TowerRoom tr = MapData::TowerRoom::Decode(encoded & 0xFF00);
    return tr.valid() ? tr.shape() : "";
}

bool ShapeHasExit(const char* shape, char dir) {
    for (const char* p = shape; *p; ++p) if (*p == dir) return true;
    return false;
}

struct CellEntry { uint8_t cellX, cellY; uint16_t encoded; };

// Decode one 24-byte levelgraph record into the cell list.
void DecodeRecord(const uint8_t (&rec)[kLevelGraphRecordSize],
                  std::vector<CellEntry>& out) {
    out.clear();
    for (int i = 0; i < kLevelGraphMaxRooms; ++i) {
        const uint8_t pos = rec[i * 3 + 0];
        if (pos == 0xFF) continue;
        CellEntry e;
        e.cellX   = pos & 0x0F;
        e.cellY   = (pos >> 4) & 0x0F;
        e.encoded = static_cast<uint16_t>(
            rec[i * 3 + 1] | (rec[i * 3 + 2] << 8));
        out.push_back(e);
    }
}

// Shape-BFS from StairsUp, N→E→S→W priority. Writes encoded values in visit
// order into `seq` and pads with 0xFFFF up to kIndexSeqSlots. Returns the
// number of visited rooms (0 if no StairsUp or unreachable layout).
int BuildSequence(const std::vector<CellEntry>& rooms, uint16_t* seq) {
    for (int i = 0; i < kIndexSeqSlots; ++i) seq[i] = 0xFFFF;

    // Build (cellX, cellY) -> encoded table on the small 16x16 grid.
    // 256 entries fits in 0.5 KB stack; we use a flat array indexed by
    // (cellY << 4) | cellX. 0xFFFF sentinel = no room here.
    uint16_t grid[256];
    for (auto& g : grid) g = 0xFFFF;
    int startX = -1, startY = -1;
    for (const auto& r : rooms) {
        grid[(r.cellY << 4) | r.cellX] = r.encoded;
        if (startX < 0 && IsStairsUp(r.encoded)) {
            startX = r.cellX; startY = r.cellY;
        }
    }
    if (startX < 0) return 0;

    // BFS state lives in two small arrays. Visited bitmap on the 16x16 grid.
    bool visited[256] = {};
    struct Cell { uint8_t x, y; };
    Cell queue[16];   // max 8 visited cells (kIndexSeqSlots) but allow slack
    int head = 0, tail = 0;
    queue[tail++] = { static_cast<uint8_t>(startX), static_cast<uint8_t>(startY) };
    visited[(startY << 4) | startX] = true;

    int out = 0;
    static const struct { int dx, dy; char ours, theirs; } kDirs[4] = {
        { 0, -1, 'N', 'S' },
        { 1,  0, 'E', 'W' },
        { 0,  1, 'S', 'N' },
        {-1,  0, 'W', 'E' },
    };

    while (head < tail && out < kIndexSeqSlots) {
        const Cell c = queue[head++];
        const uint16_t encHere = grid[(c.y << 4) | c.x];
        seq[out++] = encHere;
        const char* shapeHere = ShapeOf(encHere);
        for (const auto& d : kDirs) {
            if (!ShapeHasExit(shapeHere, d.ours)) continue;
            const int nx = c.x + d.dx;
            const int ny = c.y + d.dy;
            if (nx < 0 || nx > 15 || ny < 0 || ny > 15) continue;
            const int idx = (ny << 4) | nx;
            if (visited[idx]) continue;
            const uint16_t encNb = grid[idx];
            if (encNb == 0xFFFF) continue;
            const char* shapeNb = ShapeOf(encNb);
            if (!ShapeHasExit(shapeNb, d.theirs)) continue;
            visited[idx] = true;
            if (tail < static_cast<int>(sizeof(queue)/sizeof(queue[0]))) {
                queue[tail++] = { static_cast<uint8_t>(nx),
                                  static_cast<uint8_t>(ny) };
            }
        }
    }
    return out;
}

inline int CompareSequences(const uint16_t* a, const uint16_t* b) {
    for (int i = 0; i < kIndexSeqSlots; ++i) {
        if (a[i] != b[i]) return (a[i] < b[i]) ? -1 : 1;
    }
    return 0;
}

inline bool RecordLess(const IndexRecord& a, const IndexRecord& b) {
    const int c = CompareSequences(a.seq, b.seq);
    if (c != 0) return c < 0;
    return a.seed < b.seed;
}

// Stream-read 24-byte records from one .bin file, push 20-byte IndexRecords
// into `chunk`. Returns false on read error.
bool ProcessOneBinFile(const fs::path& path, uint32_t startSeed,
                       std::vector<IndexRecord>& chunk) {
    FILE* fp = fopen(path.string().c_str(), "rb");
    if (!fp) {
        fprintf(stderr, "[err] cannot open %s\n", path.string().c_str());
        return false;
    }
    // thread_local so each phase-1 worker gets its own stdio buffer —
    // otherwise multiple FILE*s share state and the stream output gets
    // interleaved (which we saw as out-of-order chunk records).
    thread_local char readBuf[1 << 20];
    setvbuf(fp, readBuf, _IOFBF, sizeof(readBuf));

    std::vector<CellEntry> rooms;
    uint8_t rec[kLevelGraphRecordSize];
    uint32_t seed = startSeed;
    while (fread(rec, 1, kLevelGraphRecordSize, fp) == kLevelGraphRecordSize) {
        IndexRecord ir;
        DecodeRecord(rec, rooms);
        BuildSequence(rooms, ir.seq);
        ir.seed = seed++;
        chunk.push_back(ir);
    }
    fclose(fp);
    return true;
}

// Parse a level<id>_<bs>_<be>.bin filename for its start seed. Returns -1 if
// the filename doesn't match the expected pattern.
int64_t StartSeedFromName(const fs::path& p, uint32_t levelId) {
    const std::string s = p.filename().string();
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "level%u_", levelId);
    if (s.rfind(prefix, 0) != 0) return -1;
    const char* rest = s.c_str() + strlen(prefix);
    char* end = nullptr;
    const uint64_t bs = strtoull(rest, &end, 10);
    if (!end || *end != '_') return -1;
    return static_cast<int64_t>(bs);
}

bool WriteChunkSorted(std::vector<IndexRecord>& chunk, const fs::path& out) {
    std::sort(chunk.begin(), chunk.end(), RecordLess);
    FILE* fp = fopen(out.string().c_str(), "wb");
    if (!fp) {
        fprintf(stderr, "[err] cannot write %s\n", out.string().c_str());
        return false;
    }
    thread_local char writeBuf[1 << 20];
    setvbuf(fp, writeBuf, _IOFBF, sizeof(writeBuf));
    const size_t written =
        fwrite(chunk.data(), kIndexRecordSize, chunk.size(), fp);
    fclose(fp);
    return written == chunk.size();
}

// K-way merge of sorted chunk files into one final sorted output.
bool MergeChunks(const std::vector<fs::path>& chunks, const fs::path& out) {
    struct Source {
        FILE*       fp;
        IndexRecord head;
        bool        eof;
    };
    std::vector<Source> srcs(chunks.size());
    static thread_local std::vector<char> bufs; bufs.clear();
    for (size_t i = 0; i < chunks.size(); ++i) {
        srcs[i].fp = fopen(chunks[i].string().c_str(), "rb");
        if (!srcs[i].fp) {
            fprintf(stderr, "[err] cannot open chunk %s\n",
                    chunks[i].string().c_str());
            return false;
        }
        // 16 KiB read buffer per source — chunk count can run into the
        // thousands when phase 1 emits one chunk per input batch, so we
        // keep total merge buffering bounded.
        char* buf = new char[1 << 14];
        setvbuf(srcs[i].fp, buf, _IOFBF, 1 << 14);
        const size_t n = fread(&srcs[i].head, kIndexRecordSize, 1, srcs[i].fp);
        srcs[i].eof = (n == 0);
    }

    FILE* fp = fopen(out.string().c_str(), "wb");
    if (!fp) { fprintf(stderr, "[err] cannot write %s\n", out.string().c_str()); return false; }
    static char writeBuf[1 << 20];
    setvbuf(fp, writeBuf, _IOFBF, sizeof(writeBuf));

    using HeapEntry = std::pair<IndexRecord, int>;  // record + source index
    auto cmp = [](const HeapEntry& a, const HeapEntry& b) {
        return RecordLess(b.first, a.first);  // min-heap
    };
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, decltype(cmp)> pq(cmp);
    for (int i = 0; i < (int)srcs.size(); ++i) {
        if (!srcs[i].eof) pq.push({ srcs[i].head, i });
    }
    while (!pq.empty()) {
        auto [rec, idx] = pq.top();
        pq.pop();
        fwrite(&rec, kIndexRecordSize, 1, fp);
        const size_t n = fread(&srcs[idx].head, kIndexRecordSize, 1, srcs[idx].fp);
        if (n == 1) pq.push({ srcs[idx].head, idx });
        else        srcs[idx].eof = true;
    }
    fclose(fp);
    for (auto& s : srcs) if (s.fp) fclose(s.fp);
    return true;
}

// Parse a comma-separated list of hex tokens into parallel base/mask vectors.
//
// Each token is 4 hex chars. A nibble of '?' marks 4 unknown bits. For finer
// control, append `/UMASK` (4 hex chars) where bits set in UMASK are marked
// unknown — e.g. `8900/0001` means "0x8900 with bit 0 free", a 1-bit fan-out
// of 2 lookups. Useful when a room has fewer than 4 grave slots and only one
// (or two) low bits should vary.
// Merge `chunks` into `out` in stages capped at `kMergeArity` open handles
// per pass. 256 fits a 214-chunk 2.14 B-seed build in a single pass while
// staying well under Windows' default 512-FILE handle limit (we open arity+1
// streams plus the 3 std streams).
constexpr size_t kMergeArity = 256;

bool HierarchicalMerge(std::vector<fs::path> chunks,
                       const fs::path& out,
                       const fs::path& tmpDir) {
    if (chunks.empty()) {
        fprintf(stderr, "[err] HierarchicalMerge: no chunks\n");
        return false;
    }
    int pass = 0;
    while (chunks.size() > kMergeArity) {
        std::vector<fs::path> next;
        next.reserve((chunks.size() + kMergeArity - 1) / kMergeArity);
        for (size_t i = 0; i < chunks.size(); i += kMergeArity) {
            const size_t end = std::min(i + kMergeArity, chunks.size());
            std::vector<fs::path> group(chunks.begin() + i, chunks.begin() + end);
            char name[64];
            snprintf(name, sizeof(name), "merge_%d_%zu.bin", pass, i / kMergeArity);
            const fs::path interm = tmpDir / name;
            if (!MergeChunks(group, interm)) return false;
            next.push_back(interm);
            for (const auto& cp : group) fs::remove(cp);
        }
        printf("[*]   merge pass %d: %zu -> %zu chunks\n",
               pass, chunks.size(), next.size());
        chunks = std::move(next);
        ++pass;
    }
    if (chunks.size() == 1) {
        fs::remove(out);
        fs::rename(chunks[0], out);
        return true;
    }
    if (!MergeChunks(chunks, out)) return false;
    for (const auto& cp : chunks) fs::remove(cp);
    return true;
}

// Parse a 4-char hex literal (no '?' allowed). Returns false on invalid input.
static bool ParseHex4(const std::string& tok, uint16_t& out) {
    if (tok.size() != 4) return false;
    uint16_t v = 0;
    for (char c : tok) {
        uint16_t nb;
        if      (c >= '0' && c <= '9') nb = c - '0';
        else if (c >= 'a' && c <= 'f') nb = 10 + c - 'a';
        else if (c >= 'A' && c <= 'F') nb = 10 + c - 'A';
        else return false;
        v = static_cast<uint16_t>((v << 4) | nb);
    }
    out = v;
    return true;
}

bool ParseHexU16ListWithMask(const std::string& s,
                              std::vector<uint16_t>& bases,
                              std::vector<uint16_t>& masks) {
    bases.clear(); masks.clear();
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        const std::string tok = s.substr(i, j - i);
        const size_t slash = tok.find('/');
        const std::string baseTok = (slash == std::string::npos) ? tok : tok.substr(0, slash);
        if (baseTok.size() != 4) return false;
        uint16_t base = 0, mask = 0;
        for (char c : baseTok) {
            uint16_t nb = 0, nm = 0;
            if (c == '?') { nb = 0; nm = 0; }
            else if (c >= '0' && c <= '9') { nb = c - '0';      nm = 0xF; }
            else if (c >= 'a' && c <= 'f') { nb = 10 + c - 'a'; nm = 0xF; }
            else if (c >= 'A' && c <= 'F') { nb = 10 + c - 'A'; nm = 0xF; }
            else return false;
            base = static_cast<uint16_t>((base << 4) | nb);
            mask = static_cast<uint16_t>((mask << 4) | nm);
        }
        if (slash != std::string::npos) {
            uint16_t um = 0;
            if (!ParseHex4(tok.substr(slash + 1), um)) return false;
            // Bits set in UMASK are unknown: clear them from `mask` and from
            // `base` so they don't leak into the lookup (their value is
            // re-derived by the fan-out loop).
            mask = static_cast<uint16_t>(mask & ~um);
            base = static_cast<uint16_t>(base & ~um);
        }
        bases.push_back(base);
        masks.push_back(mask);
        i = (j < s.size()) ? j + 1 : j;
    }
    return !bases.empty();
}

} // namespace

// ---------------------------------------------------------------------------

void PrintBuildIndexUsage() {
    printf("Usage: trielookup.exe buildindex --level <id> --input <glob> "
           "--output <file> [--chunk-mb <N>]\n");
    printf("  Builds a sorted (sequence, seed) index from one or more\n");
    printf("  levelgraph .bin files. The --input pattern is a Win32 glob\n");
    printf("  (e.g. 'tower-24-graphs/level24_00*.bin') so we don't have to\n");
    printf("  pass thousands of filenames on the command line.\n");
    printf("  Output records: %d bytes each.\n", kIndexRecordSize);
}

bool ParseBuildIndexArgs(int argc, char** argv, BuildIndexArgs& out) {
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
            out.levelId = static_cast<uint32_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            out.input = argv[++i];
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            out.outputFile = argv[++i];
        } else if (strcmp(argv[i], "--chunk-mb") == 0 && i + 1 < argc) {
            out.chunkBytes = static_cast<uint64_t>(atoll(argv[++i])) << 20;
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            return false;
        }
    }
    if (out.levelId == 0)        { fprintf(stderr, "--level is required\n");  return false; }
    if (out.outputFile.empty())  { fprintf(stderr, "--output is required\n"); return false; }
    if (out.input.empty())       { fprintf(stderr, "--input is required\n");  return false; }
    return true;
}

int RunBuildIndex(const BuildIndexArgs& args) {
    // Expand --input as a glob: split into dir + filename pattern, enumerate
    // the dir via std::filesystem, match each filename against the pattern.
    // Pure C++ — no Win32 calls.
    std::vector<std::pair<int64_t, fs::path>> sources;
    {
        const fs::path patPath(args.input);
        fs::path dir = patPath.parent_path();
        if (dir.empty()) dir = ".";
        const std::string pat = patPath.filename().string();
        const bool isGlob = pat.find('*') != std::string::npos
                         || pat.find('?') != std::string::npos;

        if (!isGlob) {
            // Plain path. Honour it directly; skip if it doesn't exist.
            if (!fs::exists(patPath) || !fs::is_regular_file(patPath)) {
                fprintf(stderr, "No file at: %s\n", args.input.c_str());
                return 1;
            }
            int64_t ss = StartSeedFromName(patPath, args.levelId);
            if (ss < 0) ss = 0;
            sources.push_back({ ss, patPath });
        } else {
            std::error_code ec;
            if (!fs::is_directory(dir, ec)) {
                fprintf(stderr, "Input directory does not exist: %s\n",
                        dir.string().c_str());
                return 1;
            }
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (!entry.is_regular_file()) continue;
                const std::string name = entry.path().filename().string();
                if (!FnMatch(pat.c_str(), name.c_str())) continue;
                int64_t ss = StartSeedFromName(entry.path(), args.levelId);
                if (ss < 0) ss = 0;
                sources.push_back({ ss, entry.path() });
            }
            std::sort(sources.begin(), sources.end());
        }
    }
    if (sources.empty()) {
        fprintf(stderr, "No input files matched: %s\n", args.input.c_str());
        return 1;
    }
    printf("[*] %zu input files, sorting and indexing...\n", sources.size());

    const fs::path outPath = args.outputFile;
    const fs::path tmpDir  = outPath.parent_path() / (outPath.filename().string() + ".chunks");
    fs::create_directories(tmpDir);

    // Phase 1 (parallel): one chunk file per input batch. A pool of N
    // worker threads pulls batch indices off an atomic counter; each
    // worker reads its batch, computes sequences, sorts them in-memory,
    // and spills a sorted chunk file. No cross-worker synchronization
    // beyond the counter and the (independent) chunk writes — NVMe takes
    // the parallel writes happily.
    const unsigned int nThreads =
        std::max(1u, std::thread::hardware_concurrency());
    printf("[*] phase 1: %u parallel workers\n", nThreads);

    std::vector<fs::path>  chunkFiles(sources.size());
    std::atomic<size_t>    nextIdx{0};
    std::atomic<bool>      hadError{false};
    std::atomic<uint64_t>  doneCount{0};

    auto worker = [&]() {
        std::vector<IndexRecord> chunk;
        while (!hadError.load(std::memory_order_relaxed)) {
            const size_t i = nextIdx.fetch_add(1, std::memory_order_relaxed);
            if (i >= sources.size()) break;
            chunk.clear();
            if (!ProcessOneBinFile(sources[i].second,
                                   static_cast<uint32_t>(sources[i].first),
                                   chunk)) {
                hadError.store(true);
                return;
            }
            char name[64];
            snprintf(name, sizeof(name), "chunk_%06zu.bin", i);
            const fs::path cp = tmpDir / name;
            if (!WriteChunkSorted(chunk, cp)) {
                hadError.store(true);
                return;
            }
            chunkFiles[i] = cp;
            const uint64_t d = doneCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((d % 100) == 0 || d == sources.size()) {
                printf("[*]   batch %llu/%zu\n",
                       static_cast<unsigned long long>(d), sources.size());
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(nThreads);
    for (unsigned int t = 0; t < nThreads; ++t) threads.emplace_back(worker);
    for (auto& t : threads) t.join();
    if (hadError) return 1;

    printf("[*] phase 1 done: %zu sorted chunks on disk\n", chunkFiles.size());

    // Phase 2: hierarchical k-way merge. Each pass merges up to
    // kMergeArity chunks at a time, keeping concurrent file handles
    // bounded so we don't blow Windows' default 512-FILE limit.
    if (!HierarchicalMerge(std::move(chunkFiles), outPath, tmpDir)) return 1;
    fs::remove(tmpDir);

    const uintmax_t outSize = fs::file_size(outPath);
    printf("[+] wrote %s (%llu bytes, %llu records of %d bytes)\n",
           outPath.string().c_str(),
           static_cast<unsigned long long>(outSize),
           static_cast<unsigned long long>(outSize / kIndexRecordSize),
           kIndexRecordSize);
    return 0;
}

// ---------------------------------------------------------------------------

void PrintLookupUsage() {
    printf("Usage: mapdump.exe lookup --index <file> --prefix <hex,hex,...>\n");
    printf("                          [--list N] [--max-unknown-bits K]\n");
    printf("                          [--parity even|odd]\n");
    printf("  Binary-searches the sorted sequence index for records whose\n");
    printf("  leading uint16 values match the given prefix.\n");
    printf("  Prefix tokens are 4-char hex; '?' marks 4 unknown bits, e.g.\n");
    printf("    --prefix 2700,11??,2E00  means seq[0]=0x2700, seq[1]=0x11xx, seq[2]=0x2E00.\n");
    printf("  For finer control append `/UMASK` to a token: bits set in UMASK\n");
    printf("  are unknown, e.g. 8900/0001 means 0x8900 with bit 0 free.\n");
    printf("  Each unknown bit doubles the number of internal lookups\n");
    printf("  (default cap: 4 bits = 16 lookups).\n");
    printf("  --list N prints the first N matching seeds (default: count only).\n");
    printf("  --parity even|odd keeps only seeds whose low bit matches; both\n");
    printf("    matchCount (raw) and matchCountFiltered (post-parity) are\n");
    printf("    reported, and the emitted seed list contains only survivors.\n");
}

bool ParseLookupArgs(int argc, char** argv, LookupArgs& out) {
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--index") == 0 && i + 1 < argc) {
            out.indexFile = argv[++i];
        } else if (strcmp(argv[i], "--prefix") == 0 && i + 1 < argc) {
            out.prefix = argv[++i];
        } else if (strcmp(argv[i], "--list") == 0 && i + 1 < argc) {
            out.listLimit = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--max-unknown-bits") == 0 && i + 1 < argc) {
            out.maxUnknownBits = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--json") == 0) {
            out.json = true;
        } else if (strcmp(argv[i], "--parity") == 0 && i + 1 < argc) {
            const char* p = argv[++i];
            if      (strcmp(p, "even") == 0) out.parity = 1;
            else if (strcmp(p, "odd")  == 0) out.parity = 2;
            else {
                fprintf(stderr, "--parity must be 'even' or 'odd'\n");
                return false;
            }
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            return false;
        }
    }
    if (out.indexFile.empty()) { fprintf(stderr, "--index is required\n");   return false; }
    if (out.prefix.empty())    { fprintf(stderr, "--prefix is required\n");  return false; }
    if (out.maxUnknownBits < 0) {
        fprintf(stderr, "--max-unknown-bits must be >= 0\n");
        return false;
    }
    return true;
}

namespace {

// Compare an IndexRecord's sequence against a prefix of `plen` uint16s.
//   <0 if record < prefix, 0 if record's first plen == prefix, >0 if record > prefix.
inline int ComparePrefix(const IndexRecord* rec,
                         const uint16_t* prefix, int plen) {
    for (int i = 0; i < plen; ++i) {
        if (rec->seq[i] != prefix[i])
            return (rec->seq[i] < prefix[i]) ? -1 : 1;
    }
    return 0;
}

} // namespace

void PrintDecodeUsage() {
    printf("Usage: trielookup.exe decode <hex,hex,...>\n");
    printf("  Decodes one or more 16-bit TowerRoom encoded values (the same\n");
    printf("  uint16s a trie sequence holds) through the C++ TowerRoom table\n");
    printf("  and prints shape/theme/variant/graves for each.\n");
}

int RunDecode(int argc, char** argv) {
    if (argc < 3) { PrintDecodeUsage(); return 1; }
    std::vector<uint16_t> bases, masks;
    if (!ParseHexU16ListWithMask(argv[2], bases, masks)) {
        fprintf(stderr, "Bad encoded list: %s\n", argv[2]);
        return 1;
    }
    for (size_t i = 0; i < bases.size(); ++i) {
        const uint16_t v = bases[i];
        const MapData::TowerRoom tr = MapData::TowerRoom::Decode(v);
        printf("  %2zu: 0x%04X  ", i + 1, v);
        if (!tr.valid()) {
            printf("(unknown roomId %u)\n", (v >> 8) & 0x7Fu);
            continue;
        }
        printf("roomId=%u variant=%u graves=%s  %s\n",
               tr.roomId(), tr.variant(),
               tr.gravesMaskString().empty() ? "-" : tr.gravesMaskString().c_str(),
               tr.asText().c_str());
    }
    return 0;
}

int RunLookup(const LookupArgs& args) {
    std::vector<uint16_t> bases, masks;
    if (!ParseHexU16ListWithMask(args.prefix, bases, masks)) {
        fprintf(stderr, "Bad --prefix value: %s "
                        "(each token must be 4 hex chars, '?' allowed)\n",
                args.prefix.c_str());
        return 1;
    }
    if ((int)bases.size() > kIndexSeqSlots) {
        fprintf(stderr, "Prefix length %zu > max %d\n",
                bases.size(), kIndexSeqSlots);
        return 1;
    }

    // Collect unknown bit positions across the prefix. We fan out one
    // concrete-bit lookup per assignment, so we cap the count.
    struct BitPos { int tokenIdx; int bit; };
    std::vector<BitPos> unknowns;
    for (int t = 0; t < (int)masks.size(); ++t) {
        for (int b = 0; b < 16; ++b) {
            if (((masks[t] >> b) & 1) == 0) unknowns.push_back({t, b});
        }
    }
    const int K = static_cast<int>(unknowns.size());
    if (K > args.maxUnknownBits) {
        fprintf(stderr,
                "Prefix has %d unknown bits, --max-unknown-bits=%d "
                "(would require %llu lookups). Bump the limit or supply\n"
                "more bits in the prefix.\n",
                K, args.maxUnknownBits,
                static_cast<unsigned long long>(1ull << K));
        return 1;
    }

    MmapFile mm;
    if (!mm.open(args.indexFile)) return 1;
    if (mm.size() % kIndexRecordSize != 0) {
        fprintf(stderr, "Index file size %zu not divisible by %d\n",
                mm.size(), kIndexRecordSize);
        return 1;
    }
    const IndexRecord* recs = static_cast<const IndexRecord*>(mm.data());
    const size_t n          = mm.size() / kIndexRecordSize;
    const int    plen       = static_cast<int>(bases.size());

    auto lowerBound = [&](const uint16_t* needle) -> size_t {
        size_t lo = 0, hi = n;
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (ComparePrefix(&recs[mid], needle, plen) < 0) lo = mid + 1;
            else                                              hi = mid;
        }
        return lo;
    };
    auto upperBound = [&](const uint16_t* needle) -> size_t {
        size_t lo = 0, hi = n;
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (ComparePrefix(&recs[mid], needle, plen) <= 0) lo = mid + 1;
            else                                               hi = mid;
        }
        return lo;
    };

    // For K unknowns, enumerate 2^K assignments. Each produces one concrete
    // prefix → one [lo, hi) range. Ranges are pairwise disjoint by
    // construction (different concrete bit settings produce different
    // sequences), so the union is just their concatenation.
    const uint64_t totalCombos = (K >= 64) ? 0ull : (1ull << K);
    struct Range { size_t lo, hi; };
    std::vector<Range> hits;
    hits.reserve(static_cast<size_t>(totalCombos));
    std::vector<uint16_t> probe(bases);
    size_t totalMatches = 0;

    for (uint64_t c = 0; c < totalCombos; ++c) {
        probe = bases;
        for (int k = 0; k < K; ++k) {
            if ((c >> k) & 1) {
                probe[unknowns[k].tokenIdx] |=
                    static_cast<uint16_t>(1u << unknowns[k].bit);
            }
        }
        const size_t lo = lowerBound(probe.data());
        const size_t hi = upperBound(probe.data());
        if (lo < hi) {
            hits.push_back({lo, hi});
            totalMatches += hi - lo;
        }
    }

    // Parity-aware match count. When --parity is set we walk every hit
    // record once to count survivors; cost is O(matches) but capped by the
    // already-narrowed trie range and runs only when the caller actually
    // asks for parity, so it's negligible relative to a single LoadAct.
    const uint32_t parityBit  = (args.parity == 2) ? 1u : 0u;
    const bool     filtering  = args.parity != 0;
    size_t parityMatches = totalMatches;
    if (filtering) {
        parityMatches = 0;
        for (const auto& r : hits) {
            for (size_t i = r.lo; i < r.hi; ++i) {
                if ((recs[i].seed & 1u) == parityBit) ++parityMatches;
            }
        }
    }
    // The seed list, when emitted, is bounded by the parity-filtered count
    // — so a --list cap that exceeded the raw matchCount but is still above
    // the filtered count is honored at exactly the filtered count.
    const size_t emitTotal = filtering ? parityMatches : totalMatches;
    const size_t emitCap   = (args.listLimit > 0)
        ? std::min<size_t>(emitTotal, static_cast<size_t>(args.listLimit))
        : 0;

    if (args.json) {
        // Compact one-line JSON. matchCount is the raw trie-hit count;
        // matchCountFiltered (present only when --parity is set) is the
        // count after parity. seeds[] holds only parity-matching seeds when
        // filtering, so the Go side can drop its in-memory parity pass.
        printf("{\"prefixLength\":%d,\"unknownBits\":%d,\"matchCount\":%zu",
               plen, K, totalMatches);
        if (filtering) {
            printf(",\"matchCountFiltered\":%zu", parityMatches);
        }
        printf(",\"seeds\":[");
        size_t emitted = 0;
        for (const auto& r : hits) {
            for (size_t i = r.lo; i < r.hi && emitted < emitCap; ++i) {
                if (filtering && (recs[i].seed & 1u) != parityBit) continue;
                if (emitted > 0) putchar(',');
                printf("%u", recs[i].seed);
                ++emitted;
            }
            if (emitted >= emitCap) break;
        }
        printf("]}\n");
        return 0;
    }

    printf("Prefix length:   %d\n", plen);
    printf("Unknown bits:    %d (fanned out to %llu lookups, %zu hit)\n",
           K, static_cast<unsigned long long>(totalCombos), hits.size());
    printf("Match count:     %zu\n", totalMatches);
    if (filtering) {
        printf("After parity:    %zu (%s)\n",
               parityMatches, args.parity == 1 ? "even" : "odd");
    }

    if (args.listLimit > 0 && emitTotal > 0) {
        printf("First %zu seeds:\n", emitCap);
        size_t emitted = 0;
        for (const auto& r : hits) {
            for (size_t i = r.lo; i < r.hi && emitted < emitCap; ++i) {
                if (filtering && (recs[i].seed & 1u) != parityBit) continue;
                printf("  %u  (seq:", recs[i].seed);
                for (int j = 0; j < kIndexSeqSlots; ++j) {
                    if (recs[i].seq[j] == 0xFFFF) break;
                    printf(" %04X", recs[i].seq[j]);
                }
                printf(")\n");
                ++emitted;
            }
            if (emitted >= emitCap) break;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------

void PrintCollisionsUsage() {
    printf("Usage: trielookup.exe collisions --index <file> "
           "[--top N] [--seeds-per-top N] [--parity]\n");
    printf("  Scans the sorted index, finds runs of records sharing the same\n");
    printf("  8-uint16 sequence, and prints group-size statistics + the\n");
    printf("  largest groups (sequence + sample seeds).\n");
    printf("  --parity adds seed&1 to the bucket key, modelling \"we know the\n");
    printf("  seed's parity from another level for free\".\n");
    printf("  --prefix-len N  use only first N (1..8) sequence slots as key\n");
    printf("                  (default: 8 = full sequence).\n");
    printf("  --strip-graves  mask the low byte (graves bitmask) from key\n");
    printf("                  elements so seeds differing only in grave layout\n");
    printf("                  share a bucket.\n");
    printf("  --export-min N --export-max N --export-file F\n");
    printf("    writes every seed belonging to a bucket with size in [N, N]\n");
    printf("    as packed uint32 LE (sorted ascending) into F.\n");
}

bool ParseCollisionsArgs(int argc, char** argv, CollisionsArgs& out) {
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--index") == 0 && i + 1 < argc) {
            out.indexFile = argv[++i];
        } else if (strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
            out.topGroups = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--seeds-per-top") == 0 && i + 1 < argc) {
            out.seedsPerTop = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--parity") == 0) {
            out.parity = true;
        } else if (strcmp(argv[i], "--prefix-len") == 0 && i + 1 < argc) {
            out.prefixLen = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--strip-graves") == 0) {
            out.stripGraves = true;
        } else if (strcmp(argv[i], "--export-min") == 0 && i + 1 < argc) {
            out.exportMin = static_cast<uint32_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--export-max") == 0 && i + 1 < argc) {
            out.exportMax = static_cast<uint32_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--export-file") == 0 && i + 1 < argc) {
            out.exportFile = argv[++i];
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            return false;
        }
    }
    if (out.indexFile.empty()) { fprintf(stderr, "--index is required\n"); return false; }
    const bool anyExport = !out.exportFile.empty() || out.exportMin > 0 || out.exportMax > 0;
    if (anyExport) {
        if (out.exportFile.empty()) {
            fprintf(stderr, "--export-file is required when --export-min/--export-max is set\n");
            return false;
        }
        if (out.exportMin == 0 || out.exportMax == 0 || out.exportMax < out.exportMin) {
            fprintf(stderr, "--export-min/--export-max must both be >0 and min<=max\n");
            return false;
        }
    }
    return true;
}

int RunCollisions(const CollisionsArgs& args) {
    MmapFile mm;
    if (!mm.open(args.indexFile)) return 1;
    if (mm.size() % kIndexRecordSize != 0) {
        fprintf(stderr, "Index file size %zu not divisible by %d\n",
                mm.size(), kIndexRecordSize);
        return 1;
    }
    const IndexRecord* recs = static_cast<const IndexRecord*>(mm.data());
    const size_t n          = mm.size() / kIndexRecordSize;

    const int plen = std::clamp(args.prefixLen, 1, kIndexSeqSlots);
    // High-byte-only mask if --strip-graves: graves live in the low byte.
    const uint16_t keyMask = args.stripGraves ? 0xFF00 : 0xFFFF;

    auto keyEqual = [&](const uint16_t* a, const uint16_t* b) {
        for (int i = 0; i < plen; ++i) {
            if ((a[i] & keyMask) != (b[i] & keyMask)) return false;
        }
        return true;
    };

    // Single linear scan. Records are pre-sorted by (sequence, seed), so a
    // collision group is just a maximal run of identical sequences.
    uint64_t uniqueSeqs  = 0;
    uint64_t groupCount  = 0;     // groups of size >= 2
    uint64_t seedsInGroups = 0;   // seeds covered by those groups
    uint64_t maxGroup    = 0;
    // Bucket sizes: power-of-two histogram covers 2,3,4..7,8..15, etc.
    // Linear up to 32 then power-of-two.
    constexpr int kLinearMax = 32;
    std::vector<uint64_t> linearHist(kLinearMax + 1, 0);  // index = size
    std::vector<uint64_t> log2Hist(32, 0);                // index = floor(log2 size)
    auto bumpHist = [&](uint64_t sz) {
        if (sz <= kLinearMax) { linearHist[sz]++; return; }
        int b = 0; uint64_t v = sz; while (v > 1) { v >>= 1; ++b; }
        if (b < (int)log2Hist.size()) log2Hist[b]++;
    };

    // Track top-N groups by size, with their starting index in `recs` and an
    // optional parity tag (0/1, or -1 when parity mode is off).
    struct Top { uint64_t size; size_t start; int parity; };
    auto cmpTop = [](const Top& a, const Top& b) { return a.size > b.size; };
    // We use a min-heap of size N so the smallest is on top and gets popped.
    std::priority_queue<Top, std::vector<Top>, decltype(cmpTop)> topQ(cmpTop);
    const int topN = std::max(0, args.topGroups);

    auto consider = [&](uint64_t sz, size_t start, int parity) {
        if (topN <= 0 || sz < 2) return;
        if ((int)topQ.size() < topN) {
            topQ.push({ sz, start, parity });
        } else if (sz > topQ.top().size) {
            topQ.pop();
            topQ.push({ sz, start, parity });
        }
    };

    const bool exporting = !args.exportFile.empty();
    std::vector<uint32_t> exportSeeds;
    if (exporting) exportSeeds.reserve(1u << 20);

    // Running accumulator for entropy. After the scan,
    //   bits = log2(N) - (1/N) * sumSizeLog2Size
    // is the average number of bits the bucket identifier reveals about
    // a uniformly-drawn seed.
    double sumSizeLog2Size = 0.0;

    auto countSubGroup = [&](uint64_t sz, size_t start, size_t end, int parity) {
        ++uniqueSeqs;
        if (sz >= 2) {
            ++groupCount;
            seedsInGroups += sz;
            if (sz > maxGroup) maxGroup = sz;
            consider(sz, start, parity);
        }
        bumpHist(sz);
        if (sz >= 2) sumSizeLog2Size += (double)sz * std::log2((double)sz);
        if (exporting && sz >= args.exportMin && sz <= args.exportMax) {
            for (size_t k = start; k < end; ++k) {
                if (parity >= 0 && (int)(recs[k].seed & 1u) != parity) continue;
                exportSeeds.push_back(recs[k].seed);
            }
        }
    };

    size_t i = 0;
    while (i < n) {
        size_t j = i + 1;
        while (j < n && keyEqual(recs[i].seq, recs[j].seq)) ++j;
        if (args.parity) {
            // Split this sequence-group into even-seed and odd-seed
            // sub-buckets. Within [i, j) seeds are sorted ascending (RecordLess
            // sorts by sequence then seed), so we just count parities.
            uint64_t nEven = 0, nOdd = 0;
            for (size_t k = i; k < j; ++k) {
                if (recs[k].seed & 1u) ++nOdd; else ++nEven;
            }
            if (nEven > 0) countSubGroup(nEven, i, j, 0);
            if (nOdd  > 0) countSubGroup(nOdd,  i, j, 1);
        } else {
            countSubGroup(j - i, i, j, -1);
        }
        i = j;
    }

    printf("Records:           %llu\n",
           static_cast<unsigned long long>(n));
    printf("Key:               first %d seq slot%s%s%s\n",
           plen, plen == 1 ? "" : "s",
           args.stripGraves ? ", graves stripped" : "",
           args.parity     ? ", + seed parity" : "");
    printf("%-19s%llu (%.4f%% of records)\n",
           args.parity ? "Buckets:" : "Unique keys:",
           static_cast<unsigned long long>(uniqueSeqs),
           100.0 * uniqueSeqs / n);
    printf("Collision groups:  %llu (buckets with >= 2 seeds)\n",
           static_cast<unsigned long long>(groupCount));
    printf("Seeds in groups:   %llu (%.4f%% of records)\n",
           static_cast<unsigned long long>(seedsInGroups),
           100.0 * seedsInGroups / n);
    printf("Largest group:     %llu seeds\n",
           static_cast<unsigned long long>(maxGroup));
    // Bits of information the key reveals about a uniformly-drawn seed:
    //   bits = log2(N) - (1/N) * sum_b size_b * log2(size_b)
    // Equivalently the mutual information between seed and key, in bits.
    const double bits = std::log2((double)n) - sumSizeLog2Size / (double)n;
    printf("Info revealed:     %.3f bits (max %.3f) — %.1f%% of seed-space entropy\n",
           bits, std::log2((double)n), 100.0 * bits / std::log2((double)n));

    printf("\nGroup-size histogram (#groups of each size):\n");
    for (int s = 1; s <= kLinearMax; ++s) {
        if (linearHist[s] == 0) continue;
        printf("  size %2d  %12llu\n", s,
               static_cast<unsigned long long>(linearHist[s]));
    }
    for (int b = 0; b < (int)log2Hist.size(); ++b) {
        if (log2Hist[b] == 0) continue;
        const uint64_t lo = 1ull << b;
        const uint64_t hi = (1ull << (b + 1)) - 1;
        printf("  size %5llu..%-5llu  %12llu\n",
               static_cast<unsigned long long>(lo),
               static_cast<unsigned long long>(hi),
               static_cast<unsigned long long>(log2Hist[b]));
    }

    if (topN > 0 && !topQ.empty()) {
        // Drain into a sorted-descending list.
        std::vector<Top> top;
        while (!topQ.empty()) { top.push_back(topQ.top()); topQ.pop(); }
        std::sort(top.begin(), top.end(),
                  [](const Top& a, const Top& b) { return a.size > b.size; });
        printf("\nTop %d collision groups:\n", (int)top.size());
        for (size_t t = 0; t < top.size(); ++t) {
            const auto& g = top[t];
            printf("  [%2zu] size=%llu", t + 1,
                   static_cast<unsigned long long>(g.size));
            if (g.parity >= 0) {
                printf(" parity=%s", g.parity ? "odd" : "even");
            }
            printf("  key:");
            for (int k = 0; k < plen; ++k) {
                const uint16_t v = recs[g.start].seq[k] & keyMask;
                if (v == (uint16_t)(0xFFFF & keyMask)) break;
                printf(" %04X", v);
            }
            printf("\n");
            // Walk the sequence-group from `start` and emit up to seedsPerTop
            // seeds matching the bucket parity. In non-parity mode every seed
            // qualifies.
            int shown = 0;
            const int wantParity = g.parity;
            // Determine the end of this key-group using the same key as the
            // scan above.
            size_t end = g.start + 1;
            while (end < n && keyEqual(recs[g.start].seq,
                                       recs[end].seq)) ++end;
            for (size_t k = g.start; k < end && shown < args.seedsPerTop; ++k) {
                if (wantParity >= 0 &&
                    (int)(recs[k].seed & 1u) != wantParity) continue;
                printf("        seed=%u\n", recs[k].seed);
                ++shown;
            }
            if ((int)g.size > shown) {
                printf("        ... %llu more\n",
                       static_cast<unsigned long long>(g.size - shown));
            }
        }
    }

    if (exporting) {
        std::sort(exportSeeds.begin(), exportSeeds.end());
        FILE* fp = fopen(args.exportFile.c_str(), "wb");
        if (!fp) {
            fprintf(stderr, "[err] cannot write %s\n", args.exportFile.c_str());
            return 1;
        }
        const size_t wrote = fwrite(exportSeeds.data(),
                                    sizeof(uint32_t),
                                    exportSeeds.size(), fp);
        fclose(fp);
        if (wrote != exportSeeds.size()) {
            fprintf(stderr, "[err] short write (%zu/%zu)\n",
                    wrote, exportSeeds.size());
            return 1;
        }
        printf("\n[+] exported %zu seeds (bucket size in [%u, %u]) to %s "
               "(%zu bytes, uint32 LE, sorted ascending)\n",
               exportSeeds.size(), args.exportMin, args.exportMax,
               args.exportFile.c_str(),
               exportSeeds.size() * sizeof(uint32_t));
    }
    return 0;
}
