#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <Windows.h>
#include <cstdio>
#include "d2loader.h"

// =============================================================================
// Target: Diablo II 1.13c (separate DLLs — D2Common.dll, D2Client.dll, etc.)
//
// All function pointers are resolved via LoadLibraryA + GetProcAddress.
// Ordinals come from d2mapapi/D2Map.DllWrapper/d2ptrs.h (authoritative for
// 1.13c). Two addresses in D2Client.dll are not exported and are resolved by
// fixed offset from the DLL base.
//
// Init sequence mirrors d2mapapi/D2Map.DllWrapper/exports.cpp::Initialize.
// =============================================================================

// --- Function pointer typedefs ---

using D2COMMON_LoadAct_t        = Act*     (__stdcall*)(uint32_t actNo, uint32_t seed,
                                                         uint32_t unk, void* pGame,
                                                         uint32_t difficulty, void* pMempool,
                                                         uint32_t townLevelId,
                                                         uint32_t func1, uint32_t func2);
using D2COMMON_UnloadAct_t      = void     (__stdcall*)(Act*);
using D2COMMON_GetLevel_t       = Level*   (__fastcall*)(ActMisc*, uint32_t levelNo);
using D2COMMON_InitLevel_t      = void     (__stdcall*)(Level*);
using D2COMMON_AddRoom_t        = void     (__stdcall*)(Act*, int levelId, int x, int y, Room1*);
using D2COMMON_RemRoom_t        = void     (__stdcall*)(Act*, int levelId, int x, int y, Room1*);
using D2COMMON_InitDataTables_t = uint32_t (__stdcall*)(uint32_t, uint32_t, uint32_t);

using FOG_10021_t           = void     (__fastcall*)(const char*);
using FOG_10089_t           = uint32_t (__fastcall*)(uint32_t);
using FOG_10101_t           = uint32_t (__fastcall*)(uint32_t, uint32_t);
using FOG_10218_t           = uint32_t (__fastcall*)();
using D2WIN_10086_t         = uint32_t (__fastcall*)();
using D2WIN_10005_t         = uint32_t (__fastcall*)(uint32_t, uint32_t, uint32_t, D2ClientStruct*);
using D2LANG_10008_t        = uint32_t (__fastcall*)(uint32_t, const char*, uint32_t);

// Storm.dll MPQ file I/O. With hMpq = NULL, SFileOpenFileEx searches all
// currently-open archives in priority order. D2's stock init (FOG_10218 /
// InitDataTables) already opens d2data, d2exp, and patch_d2 — so 1.13c
// patched files (e.g. the updated Levels.txt LevelName keys) are reachable
// without us touching the MPQ chain.
using SFileOpenFileEx_t  = BOOL  (__stdcall*)(HANDLE hMpq, LPCSTR szFileName,
                                              DWORD dwSearchScope, HANDLE* phFile);
using SFileGetFileSize_t = DWORD (__stdcall*)(HANDLE hFile, DWORD* pdwHigh);
using SFileReadFile_t    = BOOL  (__stdcall*)(HANDLE hFile, void* pvBuffer,
                                              DWORD dwToRead, DWORD* pdwRead,
                                              LPOVERLAPPED lpOverlapped);
using SFileCloseFile_t   = BOOL  (__stdcall*)(HANDLE hFile);

// D2Client.dll InitGameMisc — never called directly; entered via the naked shim
// below so that ECX/EBP/ESI/EDI land on the stack the way the real function
// expects them (the original is invoked from inside D2Client with those regs
// holding meaningful values).
using D2CLIENT_InitGameMisc_I_t = void (__stdcall*)(uint32_t, uint32_t, uint32_t);

// --- Resolved function pointers ---

static D2COMMON_LoadAct_t        s_LoadAct        = nullptr;
static D2COMMON_UnloadAct_t      s_UnloadAct      = nullptr;
static D2COMMON_GetLevel_t       s_GetLevel       = nullptr;
static D2COMMON_InitLevel_t      s_InitLevel      = nullptr;
static D2COMMON_AddRoom_t        s_AddRoomData    = nullptr;
static D2COMMON_RemRoom_t        s_RemRoomData    = nullptr;
static D2COMMON_InitDataTables_t s_InitDataTables = nullptr;

static FOG_10021_t    s_FOG_10021    = nullptr;
static FOG_10089_t    s_FOG_10089    = nullptr;
static FOG_10101_t    s_FOG_10101    = nullptr;
static FOG_10218_t    s_FOG_10218    = nullptr;
static D2WIN_10086_t  s_D2WIN_10086  = nullptr;
static D2WIN_10005_t  s_D2WIN_10005  = nullptr;
static D2LANG_10008_t s_D2LANG_10008 = nullptr;

static SFileOpenFileEx_t  s_SFileOpenFileEx  = nullptr;
static SFileGetFileSize_t s_SFileGetFileSize = nullptr;
static SFileReadFile_t    s_SFileReadFile    = nullptr;
static SFileCloseFile_t   s_SFileCloseFile   = nullptr;

static D2CLIENT_InitGameMisc_I_t s_InitGameMisc_I = nullptr;

// Address-offset resolves (not exported by ordinal, taken as DLL base + offset)
static uint32_t  s_LoadAct_cb1 = 0;  // D2Client +0x62AA0
static uint32_t  s_LoadAct_cb2 = 0;  // D2Client +0x62760
static uint32_t* s_StormMPQHashTable = nullptr;  // Storm +0x53120

static D2ClientStruct s_D2Client = {};
static bool           s_initialized = false;

// =============================================================================
// Naked shim for D2CLIENT_InitGameMisc — must preserve exact register layout
// that the real function reads from the stack. Copied verbatim from d2mapapi.
// =============================================================================
#if defined(_MSC_VER)
static void __declspec(naked) D2CLIENT_InitGameMisc(void) {
    __asm {
        PUSH ECX
        PUSH EBP
        PUSH ESI
        PUSH EDI
        JMP s_InitGameMisc_I
        RETN
    }
}
#else
#error "Naked-function shim requires MSVC inline asm; non-MSVC builds unsupported"
#endif

// =============================================================================
// Resolution helpers
// =============================================================================

static uint32_t D2ClientCallback() { return s_D2Client.dwInit; }

template<typename T>
static bool ResolveOrdinal(T& out, HMODULE hDll, int ordinal, const char* tag, bool verbose) {
    FARPROC p = GetProcAddress(hDll, reinterpret_cast<LPCSTR>(ordinal));
    if (!p) {
        fprintf(stderr, "  FAIL %s (ordinal %d)\n", tag, ordinal);
        return false;
    }
    out = reinterpret_cast<T>(p);
    if (verbose) printf("  ok   %-32s ord %5d -> %p\n", tag, ordinal, reinterpret_cast<void*>(p));
    return true;
}

template<typename T>
static void ResolveOffset(T& out, HMODULE hDll, uint32_t offset, const char* tag, bool verbose) {
    auto addr = reinterpret_cast<uint8_t*>(hDll) + offset;
    out = reinterpret_cast<T>(addr);
    if (verbose) printf("  ok   %-32s +0x%05X -> %p\n", tag, offset, reinterpret_cast<void*>(addr));
}

// =============================================================================
// D2_Initialize — mirrors d2mapapi exports.cpp::Initialize
// =============================================================================

bool D2_Initialize(const char* gameDir, bool verbose) {
    if (verbose) printf("D2_Initialize: game dir = %s\n", gameDir);

    // Save caller's CWD — D2 init reads MPQs/DLLs from gameDir, but our own
    // output (JSONs, CSVs) should land in the caller's working dir.
    char savedCwd[MAX_PATH] = {};
    GetCurrentDirectoryA(MAX_PATH, savedCwd);

    if (!SetCurrentDirectoryA(gameDir)) {
        fprintf(stderr, "  SetCurrentDirectory failed: error %lu\n", GetLastError());
        return false;
    }

    // --- Load DLLs ---
    struct DllSpec { const char* name; HMODULE handle; };
    DllSpec dlls[] = {
        { "D2Common.dll", nullptr },
        { "D2Client.dll", nullptr },
        { "D2Win.dll",    nullptr },
        { "D2Lang.dll",   nullptr },
        { "Fog.dll",      nullptr },
        { "Storm.dll",    nullptr },
    };
    if (verbose) printf("\nLoading DLLs:\n");
    for (auto& d : dlls) {
        d.handle = LoadLibraryA(d.name);
        if (!d.handle) {
            fprintf(stderr, "  FAIL LoadLibrary(%s): error %lu\n", d.name, GetLastError());
            return false;
        }
        if (verbose) printf("  ok   %-16s base %p\n", d.name, reinterpret_cast<void*>(d.handle));
    }
    HMODULE hD2Common = dlls[0].handle;
    HMODULE hD2Client = dlls[1].handle;
    HMODULE hD2Win    = dlls[2].handle;
    HMODULE hD2Lang   = dlls[3].handle;
    HMODULE hFog      = dlls[4].handle;
    HMODULE hStorm    = dlls[5].handle;

    // --- Resolve functions (ordinals authoritative for 1.13c per d2ptrs.h) ---
    if (verbose) printf("\nResolving functions:\n");
    bool ok = true;
    ok &= ResolveOrdinal(s_LoadAct,        hD2Common, 10951, "D2COMMON_LoadAct",        verbose);
    ok &= ResolveOrdinal(s_UnloadAct,      hD2Common, 10868, "D2COMMON_UnloadAct",      verbose);
    ok &= ResolveOrdinal(s_GetLevel,       hD2Common, 10207, "D2COMMON_GetLevel",       verbose);
    ok &= ResolveOrdinal(s_InitLevel,      hD2Common, 10322, "D2COMMON_InitLevel",      verbose);
    ok &= ResolveOrdinal(s_AddRoomData,    hD2Common, 10401, "D2COMMON_AddRoomData",    verbose);
    ok &= ResolveOrdinal(s_RemRoomData,    hD2Common, 11099, "D2COMMON_RemoveRoomData", verbose);
    ok &= ResolveOrdinal(s_InitDataTables, hD2Common, 10943, "D2COMMON_InitDataTables", verbose);

    ok &= ResolveOrdinal(s_FOG_10021,    hFog,    10021, "FOG_10021",    verbose);
    ok &= ResolveOrdinal(s_FOG_10089,    hFog,    10089, "FOG_10089",    verbose);
    ok &= ResolveOrdinal(s_FOG_10101,    hFog,    10101, "FOG_10101",    verbose);
    ok &= ResolveOrdinal(s_FOG_10218,    hFog,    10218, "FOG_10218",    verbose);
    ok &= ResolveOrdinal(s_D2WIN_10086,  hD2Win,  10086, "D2WIN_10086",  verbose);
    ok &= ResolveOrdinal(s_D2WIN_10005,  hD2Win,  10005, "D2WIN_10005",  verbose);
    ok &= ResolveOrdinal(s_D2LANG_10008, hD2Lang, 10008, "D2LANG_10008", verbose);

    ok &= ResolveOrdinal(s_SFileOpenFileEx,  hStorm,  268, "SFileOpenFileEx",  verbose);
    ok &= ResolveOrdinal(s_SFileGetFileSize, hStorm,  265, "SFileGetFileSize", verbose);
    ok &= ResolveOrdinal(s_SFileReadFile,    hStorm,  269, "SFileReadFile",    verbose);
    ok &= ResolveOrdinal(s_SFileCloseFile,   hStorm,  253, "SFileCloseFile",   verbose);

    if (!ok) return false;

    // Non-exported addresses — resolved by fixed offset from each DLL's base.
    // (1.13c offsets, per d2mapapi d2ptrs.h.)
    ResolveOffset(s_InitGameMisc_I, hD2Client, 0x4454B, "D2CLIENT_InitGameMisc", verbose);
    s_LoadAct_cb1 = reinterpret_cast<uint32_t>(hD2Client) + 0x62AA0;
    s_LoadAct_cb2 = reinterpret_cast<uint32_t>(hD2Client) + 0x62760;
    s_StormMPQHashTable = reinterpret_cast<uint32_t*>(
        reinterpret_cast<uint8_t*>(hStorm) + 0x53120);
    if (verbose) {
        printf("  ok   %-32s +0x%05X -> %p\n", "D2CLIENT_LoadAct_cb1", 0x62AA0,
            reinterpret_cast<void*>(s_LoadAct_cb1));
        printf("  ok   %-32s +0x%05X -> %p\n", "D2CLIENT_LoadAct_cb2", 0x62760,
            reinterpret_cast<void*>(s_LoadAct_cb2));
        printf("  ok   %-32s +0x%05X -> %p\n", "STORM_MPQHashTable",   0x53120,
            reinterpret_cast<void*>(s_StormMPQHashTable));
    }

    // --- Init sequence (verbatim order from exports.cpp::Initialize) ---
    if (verbose) printf("\nRunning init sequence:\n");

    *s_StormMPQHashTable = 0;
    s_D2Client.dwInit = 1;
    s_D2Client.fpInit = &D2ClientCallback;

    if (verbose) printf("  FOG_10021(\"D2\")\n");
    s_FOG_10021("D2");
    if (verbose) printf("  FOG_10101(1, 0)\n");
    s_FOG_10101(1, 0);
    if (verbose) printf("  FOG_10089(1)\n");
    s_FOG_10089(1);

    if (verbose) printf("  FOG_10218()\n");
    if (!s_FOG_10218()) { fprintf(stderr, "    FOG_10218 returned 0, abort\n"); return false; }

    if (verbose) printf("  D2WIN_10086()\n");
    if (!s_D2WIN_10086()) { fprintf(stderr, "    D2WIN_10086 returned 0, abort\n"); return false; }

    if (verbose) printf("  D2WIN_10005(0,0,0,&D2Client)\n");
    if (!s_D2WIN_10005(0, 0, 0, &s_D2Client)) { fprintf(stderr, "    D2WIN_10005 returned 0, abort\n"); return false; }

    if (verbose) printf("  D2LANG_10008(0, \"ENG\", 0)\n");
    s_D2LANG_10008(0, "ENG", 0);

    if (verbose) printf("  D2COMMON_InitDataTables(0,0,0)\n");
    if (!s_InitDataTables(0, 0, 0)) { fprintf(stderr, "    InitDataTables returned 0, abort\n"); return false; }

    if (verbose) printf("  D2CLIENT_InitGameMisc()  [naked shim]\n");
    D2CLIENT_InitGameMisc();

    // Restore the caller's CWD. MPQ handles stay open inside the D2 DLLs,
    // so subsequent LoadAct calls do not need gameDir as CWD.
    if (savedCwd[0]) SetCurrentDirectoryA(savedCwd);

    if (verbose) printf("\nD2_Initialize: success\n");
    s_initialized = true;
    return true;
}

// =============================================================================
// Public wrappers
// =============================================================================

Act* D2_LoadAct(uint32_t actNo, uint32_t seed, uint32_t difficulty, uint32_t townLevelId) {
    if (!s_initialized) return nullptr;
    // Arg layout matches d2mapapi LoadAct(): (act, seed, TRUE, FALSE, diff, NULL, town, cb1, cb2)
    return s_LoadAct(actNo, seed, /*unk=*/1, /*pGame=*/nullptr,
                     difficulty, /*pMempool=*/nullptr, townLevelId,
                     s_LoadAct_cb1, s_LoadAct_cb2);
}

void D2_UnloadAct(Act* pAct) {
    if (s_initialized && pAct) s_UnloadAct(pAct);
}

Level* D2_GetLevel(ActMisc* pMisc, uint32_t levelNo) {
    if (!s_initialized) return nullptr;
    for (Level* l = pMisc->pLevelFirst; l; l = l->pNextLevel)
        if (l->dwLevelNo == levelNo) return l;
    return s_GetLevel(pMisc, levelNo);
}

void D2_InitLevel(Level* pLevel) {
    if (s_initialized) s_InitLevel(pLevel);
}

void D2_AddRoomData(Act* pAct, int levelNo, int x, int y) {
    if (s_initialized) s_AddRoomData(pAct, levelNo, x, y, nullptr);
}

void D2_RemoveRoomData(Act* pAct, int levelNo, int x, int y) {
    if (s_initialized) s_RemRoomData(pAct, levelNo, x, y, nullptr);
}

bool D2_ReadMpqFile(const char* mpqPath, std::vector<uint8_t>& out) {
    out.clear();
    if (!s_initialized || !s_SFileOpenFileEx || !s_SFileGetFileSize
        || !s_SFileReadFile || !s_SFileCloseFile) return false;

    HANDLE hFile = nullptr;
    if (!s_SFileOpenFileEx(nullptr, mpqPath, 0, &hFile) || !hFile) return false;

    DWORD size = s_SFileGetFileSize(hFile, nullptr);
    if (size == 0 || size == 0xFFFFFFFFu) { s_SFileCloseFile(hFile); return false; }

    out.resize(size);
    DWORD read = 0;
    BOOL ok = s_SFileReadFile(hFile, out.data(), size, &read, nullptr);
    s_SFileCloseFile(hFile);
    if (!ok || read != size) { out.clear(); return false; }
    return true;
}
