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
constexpr uintptr_t DynamicCast = 0x12DA7AA;        // __RTDynamicCast import thunk
constexpr uintptr_t TdSceneObject = 0x2006450;      // RTTI type of the body owner
constexpr uintptr_t TdGameObjectBase = 0x2006400;   // RTTI type that has the team at +0x314
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
    {rva::DynamicCast, "ff 25 e8 b8 47 00"},
};

struct TypeName {
    uintptr_t rva;  // RTTI TypeDescriptor; the decorated name is at +0x10
    const char* name;
};

static const TypeName kTypeNames[] = {
    {rva::TdSceneObject, ".?AVSceneObject@game@xgs@@"},
    {rva::TdGameObjectBase, ".?AVGameObjectBase@@"},
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
using DynamicCastFn = void*(__cdecl*)(void*, long, void*, void*, int);
static DynamicCastFn g_dynamicCast;

// The insect whose contact query is running on this thread. The filter only acts
// when the query's self object equals this, so other callers of the shared
// narrow-phase function and other threads are unaffected.
static thread_local void* t_currentInsect;
static volatile LONG64 g_checked;
static volatile LONG64 g_skipped;
static volatile LONG64 g_faults;
static ULONGLONG g_nextStats;

// Log=2 diagnostics: candidates the 1.0.1 checks reject, and the value 1.0.0
// used as the other team for non-unit owners (it indexed the relation table
// with it, so a large value meant an out-of-bounds read).
static volatile LONG64 g_deadOwners;
static volatile LONG64 g_nonUnits;
static volatile LONG64 g_wildTeams;
static volatile uintptr_t g_loggedTypes[64];
static volatile LONG g_wildLines;

// Owner vtable -> is a GameObjectBase, with the answer in bit 0 (vtables are
// 8-byte aligned). A collision just costs one more dynamic_cast.
static volatile uintptr_t g_typeCache[512];

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
    t_currentInsect = insect;
    g_origQuery(insect, hits, a3, a4, a5, a6, radius);
    t_currentInsect = nullptr;

    if (g_logLevel >= 2) {
        const ULONGLONG now = GetTickCount64();
        if (now >= g_nextStats) {
            g_nextStats = now + 10000;
            Log("stats: candidates=%lld skipped_non_hostile=%lld recovered_faults=%lld dead_owners=%lld "
                "non_unit_owners=%lld wild_team_values=%lld",
                static_cast<long long>(g_checked), static_cast<long long>(g_skipped),
                static_cast<long long>(g_faults), static_cast<long long>(g_deadOwners),
                static_cast<long long>(g_nonUnits), static_cast<long long>(g_wildTeams));
        }
    }
}

// The game reads a team only from owners that dynamic_cast to GameObjectBase
// (EDF.dll+0x109E49). Buildings, props and other scene objects keep something
// else at +0x314, so the early test must not touch them.
static bool IsGameObject(void* owner) {
    const uintptr_t vt = *static_cast<uintptr_t*>(owner);
    volatile uintptr_t& slot = g_typeCache[(vt >> 3 ^ vt >> 12) & 511];
    const uintptr_t e = slot;
    if ((e & ~uintptr_t{1}) == vt) return (e & 1) != 0;
    const bool is = g_dynamicCast(owner, 0, reinterpret_cast<void*>(g_image + rva::TdSceneObject),
                                  reinterpret_cast<void*>(g_image + rva::TdGameObjectBase), 0) != nullptr;
    slot = vt | (is ? 1 : 0);
    return is;
}

// MSVC x64 RTTI: vtable[-1] is the CompleteObjectLocator, which holds the
// TypeDescriptor and its own address as image-relative offsets.
static const char* ClassName(const uint8_t* owner) {
    const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(owner);
    const uint8_t* col = *reinterpret_cast<uint8_t* const*>(vt - 8);
    const uintptr_t base = reinterpret_cast<uintptr_t>(col) - *reinterpret_cast<const uint32_t*>(col + 0x14);
    return reinterpret_cast<const char*>(base + *reinterpret_cast<const uint32_t*>(col + 0x0C) + 0x10);
}

static void NoteNonUnit(const uint8_t* owner, const uint8_t* obj) {
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(owner);
        const int value = *reinterpret_cast<const int*>(obj + 0x314);
        // Real team ids are small; the relation rows are tiny int arrays.
        const bool wild = value < -1 || value > 255;
        if (wild) InterlockedIncrement64(&g_wildTeams);
        bool first = false;
        for (volatile uintptr_t& slot : g_loggedTypes) {
            if (slot == vt) break;
            if (slot == 0 && InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&slot),
                                                          static_cast<LONG64>(vt), 0) == 0) {
                first = true;
                break;
            }
        }
        if (first)
            Log("non-unit owner type %s (EDF.dll+%llx): 1.0.0 read team value %d", ClassName(owner),
                static_cast<unsigned long long>(vt - g_image), value);
        else if (wild && InterlockedIncrement(&g_wildLines) <= 40)
            Log("wild team value %d from %s: 1.0.0 would index the relation table out of bounds", value,
                ClassName(owner));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_wildLines) <= 40) Log("non-unit owner unreadable (1.0.0 would fault here)");
    }
}

// Returns true to skip `owner` before the narrow phase. Only returns true where
// the caller (EDF.dll+0x4322E3) would discard the hit anyway; anything uncertain
// returns false and the game runs its normal test.
static bool SkipCandidateUnsafe(uint8_t* self, uint8_t* owner) {
    // The hit list stores the owner's weak pointer (+0x28 object, +0x30 control
    // block); the caller locks it and drops the hit if that fails.
    const uint8_t* cb = *reinterpret_cast<uint8_t* const*>(owner + 0x30);
    const uint8_t* obj = *reinterpret_cast<uint8_t* const*>(owner + 0x28);
    if (!cb || !obj || *reinterpret_cast<const volatile LONG*>(cb + 8) == 0) {
        if (g_logLevel >= 2 && cb && obj) InterlockedIncrement64(&g_deadOwners);
        return true;
    }
    if (!IsGameObject(owner)) {
        if (g_logLevel >= 2) {
            InterlockedIncrement64(&g_nonUnits);
            NoteNonUnit(owner, obj);
        }
        return false;
    }
    // Same test as EDF.dll+0x4322FB: keep unless both teams are set and the
    // relation is not hostile (2).
    const int other = *reinterpret_cast<const int*>(obj + 0x314);
    const int own = *reinterpret_cast<const int*>(self + 0x314);
    if (other == -1 || own == -1) return false;
    const uint8_t* table = *reinterpret_cast<uint8_t* const*>(g_image + rva::TeamTablePtr);
    const uint8_t* rows = *reinterpret_cast<uint8_t* const*>(table + 0x38);
    const int* relation = *reinterpret_cast<int* const*>(rows + static_cast<intptr_t>(own) * 0x38 + 0x18);
    return relation[other] != 2;
}

static bool __fastcall SkipCandidate(uint8_t* self, uint8_t* owner) {
    if (!self || self != t_currentInsect) return false;
    bool skip = false;
    __try {
        skip = SkipCandidateUnsafe(self, owner);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // A body whose owner is half torn down; let the game's own test decide.
        if (InterlockedIncrement64(&g_faults) <= 5) Log("recovered from a fault reading a candidate");
        return false;
    }
    if (g_logLevel >= 2) {
        InterlockedIncrement64(&g_checked);
        if (skip) InterlockedIncrement64(&g_skipped);
    }
    return skip;
}

// Replaces "cmp rax,[rbp+3B8h]; je next" in the narrow-phase candidate loop.
// rax = candidate owner object, [rbp+3B8h] = querying object.
static bool BuildFilterStub(uint8_t* stub) {
    Asm a;
    a.bytes({0x48, 0x3B, 0x85, 0xB8, 0x03, 0x00, 0x00});  // cmp rax,[rbp+3B8h]   (original)
    a.bytes({0x0F, 0x85, 0x0E, 0x00, 0x00, 0x00});        // jne +14 (over the jmp below)
    a.jmpAbs(g_image + rva::NextCandidate);               // self -> next candidate (original)
    // Volatile registers are dead here (the loop just called EDF.dll+0x108200),
    // but keep them anyway so the stub does not depend on that.
    a.bytes({0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});  // push rax..r11
    a.bytes({0x53});                                      // push rbx
    a.bytes({0x48, 0x89, 0xE3});                          // mov rbx,rsp
    a.bytes({0x48, 0x83, 0xE4, 0xF0});                    // and rsp,-16
    a.bytes({0x48, 0x83, 0xC4, 0x80});                    // add rsp,-80h (shadow + xmm0-5)
    for (uint8_t i = 0; i < 6; ++i)                       // movdqu [rsp+20h+i*10h],xmm<i>
        a.bytes({0xF3, 0x0F, 0x7F, static_cast<uint8_t>(0x44 | i << 3), 0x24, static_cast<uint8_t>(0x20 + i * 0x10)});
    a.bytes({0x48, 0x8B, 0x8D, 0xB8, 0x03, 0x00, 0x00});  // mov rcx,[rbp+3B8h]  self
    a.bytes({0x48, 0x89, 0xC2});                          // mov rdx,rax         owner
    a.bytes({0x48, 0xB8});                                // mov rax,&SkipCandidate
    a.imm64(reinterpret_cast<uint64_t>(&SkipCandidate));
    a.bytes({0xFF, 0xD0});                                // call rax
    a.bytes({0x84, 0xC0});                                // test al,al (flags survive the restores)
    for (uint8_t i = 0; i < 6; ++i)                       // movdqu xmm<i>,[rsp+20h+i*10h]
        a.bytes({0xF3, 0x0F, 0x6F, static_cast<uint8_t>(0x44 | i << 3), 0x24, static_cast<uint8_t>(0x20 + i * 0x10)});
    a.bytes({0x48, 0x89, 0xDC});                          // mov rsp,rbx
    a.bytes({0x5B});                                      // pop rbx
    a.bytes({0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58});  // pop r11..rax
    a.bytes({0x0F, 0x85, 0x0E, 0x00, 0x00, 0x00});        // jnz +14 (skip)
    a.jmpAbs(g_image + rva::SelfCheckEnd);                // keep: run the narrow phase
    a.jmpAbs(g_image + rva::NextCandidate);               // skip
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
    for (const TypeName& t : kTypeNames) {
        if (strcmp(reinterpret_cast<const char*>(g_image + t.rva + 0x10), t.name) != 0) {
            Log("RTTI type %s not found at EDF.dll+%llx; nothing patched", t.name,
                static_cast<unsigned long long>(t.rva));
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
    g_dynamicCast = reinterpret_cast<DynamicCastFn>(g_image + rva::DynamicCast);

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

    Log("installed v1.0.1: SkipAllyContactTests=1 Log=%d", g_logLevel);
    return true;
}

extern "C" __declspec(dllexport) bool __fastcall EML6_Load(PluginInfo* info) {
    if (!info) return false;
    info->infoVersion = 1;
    info->name = "EDF6 Swarm Perf";
    info->version = 0x00010001;
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
