// EDF6 Swarm Perf - EDFModLoader plugin.
//
// Every insect enemy (InsectBase: ants, spiders, bees, small dragons) runs a
// body-contact query each frame (EDF.dll+0x429B00): a Havok broadphase sphere
// query, then for every body found a closest-point / penetration test, a
// line-of-sight check and a de-duplicating insert into a hit list
// (EDF.dll+0x542860). The caller (EDF.dll+0x431B80) then discards every hit
// whose team is not hostile to the insect.
//
// In a dense swarm almost every body found is another insect or a corpse of the
// same team, so most of the per-frame CPU time goes to narrow-phase tests whose
// results are thrown away. This plugin applies the caller's team test before
// the narrow phase instead of after it, for this one query only. The hit list
// the insect acts on is unchanged.
//
// Every patch site is byte-verified first; on mismatch (game update) nothing is
// patched.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <share.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <vector>

// ---------------------------------------------------------------------------
// EDFModLoader interface

struct PluginInfo {
    uint32_t infoVersion;
    const char* name;
    uint32_t version;
    uint32_t reserved;
};

// ---------------------------------------------------------------------------
// EDF.dll layout (Steam build, EDF.dll SHA-256 0d509239...733d0916)

namespace rva {
constexpr uintptr_t InsectContactQuery = 0x429B00;  // void(insect, hits, ?, ?, ?, ?, float radius)
constexpr uintptr_t InsectQueryCall = 0x4322AC;     // the only call to InsectContactQuery (E8 rel32)
constexpr uintptr_t TeamCheck = 0x432314;           // caller's hostile test on each hit (witness)
constexpr uintptr_t TeamTablePtr = 0x20B2978;       // team relation table singleton*
constexpr uintptr_t SelfCheck = 0x542D28;           // cmp rax,[rbp+3B8h]; je next_candidate
constexpr uintptr_t SelfCheckEnd = 0x542D35;        // continue to narrow phase
constexpr uintptr_t NextCandidate = 0x542FFC;
constexpr uintptr_t CandidateInsert = 0x542FAC;     // weak_ptr copy from owner+0x28/+0x30 (witness)
}  // namespace rva

struct Signature {
    uintptr_t rva;
    const char* hex;
};

static const Signature kSignatures[] = {
    {rva::InsectQueryCall, "e8 4f 78 ff ff"},
    {rva::InsectContactQuery, "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8d ac 24 28 ff ff ff 48 81 ec d8 01 00 00"},
    // mov rcx,[TeamTablePtr]; imul rdx,rax,38h; mov rax,[rcx+38h]; mov rax,[rdx+rax+18h]; cmp dword [rax+r8*4],2; jne
    {rva::TeamCheck, "48 8b 0d 5d 06 c8 01 48 6b d0 38 48 8b 41 38 48 8b 44 02 18 42 83 3c 80 02 75"},
    // movsxd r8,[rax+314h]; movsxd rax,[rdi+314h] (team ids at +0x314)
    {0x4322FB, "4c 63 80 14 03 00 00 48 63 87 14 03 00 00"},
    {rva::SelfCheck, "48 3b 85 b8 03 00 00 0f 84 c7 02 00 00"},
    {rva::CandidateInsert, "49 8b 4e 30 48 85 c9 74 12 49 8b 46 28"},
};

// ---------------------------------------------------------------------------
// State

static HMODULE g_self;
static uintptr_t g_image;
static int g_logLevel = 1;
static bool g_enabled = true;
static FILE* g_log;

using InsectQueryFn = void(__fastcall*)(void*, void*, void*, void*, void*, void*, float);
static InsectQueryFn g_origQuery;

// The insect whose contact query is running. The filter stub only acts when the
// query's self object equals this, so other callers of the shared narrow-phase
// function and any concurrent thread are unaffected.
static void* volatile g_currentInsect;
static volatile LONG64 g_checked;
static volatile LONG64 g_skipped;
static ULONGLONG g_nextStats;

// ---------------------------------------------------------------------------
// Helpers

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static bool MatchSignature(const Signature& sig) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(g_image + sig.rva);
    const char* h = sig.hex;
    size_t i = 0;
    while (*h) {
        while (*h == ' ') ++h;
        if (!*h) break;
        unsigned v = 0;
        if (sscanf_s(h, "%2x", &v) != 1) return false;
        if (p[i] != static_cast<uint8_t>(v)) {
            Log("signature mismatch at EDF.dll+%llx byte %zu: have %02x want %02x",
                static_cast<unsigned long long>(sig.rva), i, p[i], v);
            return false;
        }
        ++i;
        h += 2;
    }
    return true;
}

static bool WriteCode(void* dst, const void* src, size_t len) {
    DWORD old;
    if (!VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(dst, src, len);
    VirtualProtect(dst, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, len);
    return true;
}

// Allocates executable memory within rel32 reach of `origin`.
static void* AllocNear(uintptr_t origin) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const uintptr_t lo = origin > 0x7FF00000 ? origin - 0x7FF00000 : gran;
    const uintptr_t hi = origin + 0x7FF00000;
    for (uintptr_t addr = (origin & ~(gran - 1)); addr > lo; addr -= gran) {
        void* p = VirtualAlloc(reinterpret_cast<void*>(addr), gran, MEM_RESERVE | MEM_COMMIT,
                               PAGE_EXECUTE_READWRITE);
        if (p) return p;
    }
    for (uintptr_t addr = (origin & ~(gran - 1)) + gran; addr < hi; addr += gran) {
        void* p = VirtualAlloc(reinterpret_cast<void*>(addr), gran, MEM_RESERVE | MEM_COMMIT,
                               PAGE_EXECUTE_READWRITE);
        if (p) return p;
    }
    return nullptr;
}

// Minimal x64 byte emitter with forward rel8 labels.
struct Asm {
    std::vector<uint8_t> b;
    struct Fixup {
        size_t at;
        int label;
    };
    std::vector<Fixup> fixups;
    size_t labels[8] = {};

    void bytes(std::initializer_list<uint8_t> v) { b.insert(b.end(), v); }
    void imm64(uint64_t v) {
        for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }
    void jcc8(uint8_t op, int label) {
        b.push_back(op);
        fixups.push_back({b.size(), label});
        b.push_back(0);
    }
    void bind(int label) { labels[label] = b.size(); }
    void jmpAbs(uint64_t target) {
        bytes({0xFF, 0x25, 0, 0, 0, 0});  // jmp qword ptr [rip+0]
        imm64(target);
    }
    bool finish() {
        for (const Fixup& f : fixups) {
            const ptrdiff_t d = static_cast<ptrdiff_t>(labels[f.label]) - static_cast<ptrdiff_t>(f.at + 1);
            if (d < -128 || d > 127) return false;
            b[f.at] = static_cast<uint8_t>(static_cast<int8_t>(d));
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// Hooks

static void __fastcall InsectQueryHook(void* insect, void* hits, void* a3, void* a4, void* a5, void* a6,
                                       float radius) {
    g_currentInsect = insect;
    g_origQuery(insect, hits, a3, a4, a5, a6, radius);
    g_currentInsect = nullptr;

    if (g_logLevel >= 2) {
        const ULONGLONG now = GetTickCount64();
        if (now >= g_nextStats) {
            g_nextStats = now + 10000;
            Log("stats: candidates=%lld skipped_non_hostile=%lld", static_cast<long long>(g_checked),
                static_cast<long long>(g_skipped));
        }
    }
}

// Replaces "cmp rax,[rbp+3B8h]; je next" in the narrow-phase candidate loop.
// rax = candidate owner object, [rbp+3B8h] = querying object.
static bool BuildFilterStub(uint8_t* stub) {
    enum { kSkip, kSkipPop, kSkipCount, kCont };
    Asm a;
    a.bytes({0x48, 0x3B, 0x85, 0xB8, 0x03, 0x00, 0x00});  // cmp rax,[rbp+3B8h]   (original)
    a.bytes({0x0F, 0x85, 0x0E, 0x00, 0x00, 0x00});        // jne +14 (over the jmp below)
    a.jmpAbs(g_image + rva::NextCandidate);               // self -> next candidate (original)
    a.bytes({0x51});                                      // push rcx
    a.bytes({0x52});                                      // push rdx
    a.bytes({0x48, 0x8B, 0x8D, 0xB8, 0x03, 0x00, 0x00});  // mov rcx,[rbp+3B8h]
    // Other callers pass no self object; null must never match an idle g_currentInsect.
    a.bytes({0x48, 0x85, 0xC9});                          // test rcx,rcx
    a.jcc8(0x74, kCont);                                  // je cont
    a.bytes({0x48, 0xBA});                                // mov rdx,&g_currentInsect
    a.imm64(reinterpret_cast<uint64_t>(&g_currentInsect));
    a.bytes({0x48, 0x3B, 0x0A});                          // cmp rcx,[rdx]
    a.jcc8(0x75, kCont);                                  // jne cont (not an insect contact query)
    a.bytes({0x48, 0xBA});                                // mov rdx,&g_checked
    a.imm64(reinterpret_cast<uint64_t>(&g_checked));
    a.bytes({0xF0, 0x48, 0xFF, 0x02});                    // lock inc qword [rdx]
    // The hit list stores the owner's weak self pointer (+0x28 ptr, +0x30 control
    // block); the caller ignores entries whose pointer is empty.
    a.bytes({0x48, 0x83, 0x78, 0x30, 0x00});              // cmp qword [rax+30h],0
    a.jcc8(0x74, kSkipCount);                             // je skip
    a.bytes({0x48, 0x8B, 0x50, 0x28});                    // mov rdx,[rax+28h]
    a.bytes({0x48, 0x85, 0xD2});                          // test rdx,rdx
    a.jcc8(0x74, kSkipCount);                             // je skip
    // Same test as EDF.dll+0x432302: keep unless both teams are set and the
    // relation is not hostile (2).
    a.bytes({0x48, 0x63, 0x92, 0x14, 0x03, 0x00, 0x00});  // movsxd rdx,dword [rdx+314h]  other team
    a.bytes({0x83, 0xFA, 0xFF});                          // cmp edx,-1
    a.jcc8(0x74, kCont);                                  // je cont
    a.bytes({0x48, 0x63, 0x89, 0x14, 0x03, 0x00, 0x00});  // movsxd rcx,dword [rcx+314h]  own team
    a.bytes({0x83, 0xF9, 0xFF});                          // cmp ecx,-1
    a.jcc8(0x74, kCont);                                  // je cont
    a.bytes({0x50});                                      // push rax
    a.bytes({0x48, 0xB8});                                // mov rax,&TeamTablePtr
    a.imm64(g_image + rva::TeamTablePtr);
    a.bytes({0x48, 0x8B, 0x00});                          // mov rax,[rax]
    a.bytes({0x48, 0x8B, 0x40, 0x38});                    // mov rax,[rax+38h]
    a.bytes({0x48, 0x6B, 0xC9, 0x38});                    // imul rcx,rcx,38h
    a.bytes({0x48, 0x8B, 0x44, 0x01, 0x18});              // mov rax,[rcx+rax+18h]
    a.bytes({0x83, 0x3C, 0x90, 0x02});                    // cmp dword [rax+rdx*4],2
    a.bytes({0x58});                                      // pop rax (flags kept)
    a.jcc8(0x75, kSkipCount);                             // jne skip (not hostile)
    a.bind(kCont);
    a.bytes({0x5A});                                      // pop rdx
    a.bytes({0x59});                                      // pop rcx
    a.jmpAbs(g_image + rva::SelfCheckEnd);
    a.bind(kSkipCount);
    a.bytes({0x48, 0xB9});                                // mov rcx,&g_skipped
    a.imm64(reinterpret_cast<uint64_t>(&g_skipped));
    a.bytes({0xF0, 0x48, 0xFF, 0x01});                    // lock inc qword [rcx]
    a.bind(kSkipPop);
    a.bytes({0x5A});                                      // pop rdx
    a.bytes({0x59});                                      // pop rcx
    a.bind(kSkip);
    a.jmpAbs(g_image + rva::NextCandidate);
    if (!a.finish() || a.b.size() > 512) return false;
    memcpy(stub, a.b.data(), a.b.size());
    return true;
}

// ---------------------------------------------------------------------------
// Setup

static void LoadConfig(const wchar_t* dir) {
    wchar_t ini[MAX_PATH];
    swprintf_s(ini, L"%sEDF6SwarmPerf.ini", dir);
    if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, ini, L"w") == 0 && f) {
            fputs(
                "[SwarmPerf]\n"
                "; 1 = insects skip contact tests against non-hostile bodies (other insects,\n"
                "; their corpses) before the expensive narrow phase. Gameplay is unchanged:\n"
                "; the game already discards those hits afterwards.\n"
                "SkipAllyContactTests=1\n"
                "\n"
                "; 0 = off, 1 = startup info, 2 = stats every 10 s\n"
                "Log=1\n",
                f);
            fclose(f);
        }
    }
    g_enabled = GetPrivateProfileIntW(L"SwarmPerf", L"SkipAllyContactTests", 1, ini) != 0;
    g_logLevel = GetPrivateProfileIntW(L"SwarmPerf", L"Log", 1, ini);
}

static bool Install() {
    g_image = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"EDF.dll"));
    if (!g_image) {
        Log("EDF.dll is not loaded");
        return false;
    }
    for (const Signature& sig : kSignatures) {
        if (!MatchSignature(sig)) {
            Log("unsupported EDF.dll version; nothing patched");
            return false;
        }
    }
    if (!g_enabled) {
        Log("SkipAllyContactTests=0; nothing patched");
        return true;
    }

    uint8_t* mem = static_cast<uint8_t*>(AllocNear(g_image + rva::SelfCheck));
    if (!mem) {
        Log("could not allocate a stub near EDF.dll");
        return false;
    }
    uint8_t* callThunk = mem;          // jmp [rip] -> InsectQueryHook
    uint8_t* filterStub = mem + 0x40;  // candidate filter

    Asm thunk;
    thunk.jmpAbs(reinterpret_cast<uint64_t>(&InsectQueryHook));
    memcpy(callThunk, thunk.b.data(), thunk.b.size());
    if (!BuildFilterStub(filterStub)) {
        Log("filter stub assembly failed");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), mem, 0x400);

    g_origQuery = reinterpret_cast<InsectQueryFn>(g_image + rva::InsectContactQuery);

    // Filter first: it is inert until the call hook sets g_currentInsect.
    uint8_t* site = reinterpret_cast<uint8_t*>(g_image + rva::SelfCheck);
    uint8_t jmp[13] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    const int64_t relJmp = reinterpret_cast<int64_t>(filterStub) - reinterpret_cast<int64_t>(site + 5);
    uint8_t* call = reinterpret_cast<uint8_t*>(g_image + rva::InsectQueryCall);
    const int64_t relCall = reinterpret_cast<int64_t>(callThunk) - reinterpret_cast<int64_t>(call + 5);
    if (relJmp < INT32_MIN || relJmp > INT32_MAX || relCall < INT32_MIN || relCall > INT32_MAX) {
        Log("stub out of rel32 range");
        return false;
    }
    const int32_t relJmp32 = static_cast<int32_t>(relJmp);
    memcpy(jmp + 1, &relJmp32, 4);
    if (!WriteCode(site, jmp, sizeof jmp)) {
        Log("VirtualProtect failed on candidate loop");
        return false;
    }
    const int32_t relCall32 = static_cast<int32_t>(relCall);
    if (!WriteCode(call + 1, &relCall32, sizeof relCall32)) {
        Log("VirtualProtect failed on insect query call");
        return false;
    }

    Log("installed: SkipAllyContactTests=1 Log=%d", g_logLevel);
    return true;
}

extern "C" __declspec(dllexport) bool __fastcall EML6_Load(PluginInfo* info) {
    if (!info) return false;
    info->infoVersion = 1;
    info->name = "EDF6 Swarm Perf";
    info->version = 0x00010000;
    info->reserved = 1;

    wchar_t dir[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_self, dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) slash[1] = 0;

    LoadConfig(dir);
    if (g_logLevel > 0) {
        wchar_t logPath[MAX_PATH];
        swprintf_s(logPath, L"%sEDF6SwarmPerf.log", dir);
        g_log = _wfsopen(logPath, L"w", _SH_DENYWR);  // readable while the game runs
    }
    return Install();
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
