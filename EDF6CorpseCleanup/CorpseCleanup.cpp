// EDF6 Corpse Cleanup - EDFModLoader plugin.
//
// Enemy corpses in EDF6 are tracked by the game's RagdollManager singleton
// (EDF.dll+0x20B30E0). Each corpse normally lives 15-30 s (hardcoded per enemy
// family) or until the manager's ragdoll budget (256/512) evicts the oldest one.
//
// This plugin:
//   * redirects the game loop's single call to RagdollManager::Purge so that,
//     once per frame, any tracked corpse older than CorpseSeconds is removed with
//     the same SceneObject remove function the game uses for its own timeout;
//   * optionally overrides the ragdoll budget constants;
//   * optionally lets generator (teleport anchor) corpses into the manager;
//   * optionally removes corpse hitboxes: the Havok bodies of every tracked
//     corpse ragdoll are tagged each frame, and the world collision filter's
//     query-vs-body check rejects tagged bodies. Raycasts and shape casts
//     (bullets, explosions, line of sight) skip corpses, while body-vs-body
//     physics is untouched so corpses still land on the ground.
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
#include <unordered_map>
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
constexpr uintptr_t RagdollManagerPtr = 0x20B30E0;  // RagdollManager*
constexpr uintptr_t Purge = 0x6EF0E0;              // void RagdollManager::Purge(this)
constexpr uintptr_t PurgeCallBlock = 0x7059F3;     // mov rcx,[mgr]; test; je; call Purge
constexpr uintptr_t PurgeCall = 0x7059FF;          // E8 rel32
constexpr uintptr_t RemoveObject = 0x118A1B0;      // void SceneObject::Remove(this)
constexpr uintptr_t RegisterLimits = 0x6EEEF9;     // budget select inside Register
constexpr uintptr_t RegisterLimitHi = 0x6EEF0B + 2; // mov r14d, 0x200
constexpr uintptr_t RegisterLimitLo = 0x6EEF13 + 2; // mov r14d, 0x100
constexpr uintptr_t LimitFn = 0x6EF0B0;            // standalone budget getter
constexpr uintptr_t LimitFnHi = 0x6EF0BF + 1;      // mov eax, 0x200
constexpr uintptr_t LimitFnLo = 0x6EF0C9 + 1;      // mov eax, 0x100

// hknpCollisionFilter slot 6: bool isCollisionEnabled(queryType, const hknpQueryFilterData&, const hknpBody&)
constexpr uintptr_t GroupFilter5Vtbl = 0x1761BA8;  // hknpGroupCollisionFilterBase<Config<5,5,5,16>>
constexpr uintptr_t GroupFilterVtbl = 0x18BBFC0;   // hknpGroupCollisionFilter
constexpr uintptr_t GroupFilter6Vtbl = 0x18BC010;  // hknpGroupCollisionFilterBase<Config<6,5,5,16>>
constexpr uintptr_t GroupFilter5QueryBody = 0xD93700;
constexpr uintptr_t GroupFilter6QueryBody = 0xD93800;
constexpr int QueryBodySlot = 6;
// Layout witnesses for the hitbox feature.
constexpr uintptr_t WorldIsBodyValid = 0xDAC200;   // body stride 0xB0, hknpBody::m_id at +0x50
constexpr uintptr_t RagdollActivate = 0x6E40B0;    // RagdollController bodies at +0x78, count +0x88, stride 0xC0
constexpr uintptr_t BodyWrapperQuery = 0x17E720;   // physics body wrapper: body id at +0xF0
}  // namespace rva

struct Signature {
    uintptr_t rva;
    const char* hex;
};

static const Signature kSignatures[] = {
    {rva::PurgeCallBlock, "48 8b 0d e6 d6 9a 01 48 85 c9 74 05 e8 dc 96 fe ff"},
    {rva::Purge, "48 89 5c 24 18 57 48 83 ec 20 48 8b 41 08 48 8b f9 48 8b 18 48 3b d8 74 75"},
    {rva::RegisterLimits,
     "48 8b 0d 90 39 9c 01 8b 51 40 83 ea 01 74 0b 83 fa 01 41 be 00 02 00 00 75 06 41 be 00 01 00 00"},
    {rva::LimitFn, "48 8b 0d d9 37 9c 01 8b 51 40 83 ea 01 74 0a b8 00 02 00 00 83 fa 01 75 05 b8 00 01 00 00 c3"},
    {rva::RemoveObject, "48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 60 48 8b d9 f6 41 18 04"},
};

static const Signature kHitboxSignatures[] = {
    {rva::GroupFilter5QueryBody, "41 8b 91 8c 00 00 00 4c 8b d1 45 8b 48 04"},
    {rva::GroupFilter6QueryBody, "41 8b 91 8c 00 00 00 4c 8b d1 45 8b 48 04"},
    {rva::WorldIsBodyValid,
     "8b c2 25 ff ff ff 00 3d ff ff ff 00 74 23 3b 41 20 73 1e 4c 69 c0 b0 00 00 00 4c 03 41 18 41 f6 40 54 03 "
     "74 0c 41 39 50 50"},
    {rva::RagdollActivate,
     "48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 20 48 8b f1 48 8b 89 c0 00 00 00 48 8b 49 58 e8 de 84 6c 00 "
     "48 8b 5e 78 48 8b 86 88 00 00 00 48 8d 3c 40 48 c1 e7 06"},
    {rva::BodyWrapperQuery, "40 53 48 83 ec 20 48 8b 81 00 01 00 00 48 8b d9 48 8b 48 58 48 8b 41 20 48 83 c1 20 8b 93 f0 00 00 00"},
};

// RagdollManager
//   +0x08  Node*    list sentinel
//   +0x10  size_t   count
//   +0x18  uint32   flags (bit0 = accept generator objects)
// Node
//   +0x00  Node*    next
//   +0x08  Node*    prev
//   +0x10  void*    ragdoll controller
//   +0x18  void*    owning scene object (shared_ptr element)
//   +0x20  void*    shared_ptr control block (_Ref_count_base)
//   +0x28  int32    budget cost
// RagdollController
//   +0x78  Bone*    bones (0xC0 each)
//   +0x88  size_t   bone count
// Bone
//   +0x50  void*    physics body wrapper (+0xF0 = hknpBodyId)
// hknpBody (0xB0)
//   +0x50  uint32   m_id (low 24 bits = index, high 8 bits = serial)
//   +0x8C  uint32   m_collisionFilterInfo

using PurgeFn = void(__fastcall*)(void* manager);
using RemoveFn = void(__fastcall*)(void* object);
using RefCountFn = void(__fastcall*)(void* ctrl);
using QueryBodyFilterFn = bool(__fastcall*)(void* filter, int queryType, const void* queryData, const uint8_t* body);

// ---------------------------------------------------------------------------
// State

static HMODULE g_self;
static uintptr_t g_image;
static PurgeFn g_purge;
static RemoveFn g_remove;

static float g_corpseSeconds = 5.0f;
static int g_ragdollBudget = 0;
static bool g_acceptGenerator = true;
static bool g_disableHitbox = true;
static int g_logLevel = 1;
static bool g_lifetimeActive = false;
static bool g_hitboxActive = false;

// Corpse body tags, indexed by hknpBodyId index. Value is (full body id + 1), 0 = not a corpse.
// Written on the game thread, read by the collision filter on Havok worker threads.
constexpr uint32_t kMaxBodies = 1u << 18;
static volatile uint32_t g_corpseTag[kMaxBodies];
static uint32_t g_tagGeneration[kMaxBodies];
static std::vector<uint32_t> g_taggedIndices;
static QueryBodyFilterFn g_origQueryBody[3];
static volatile LONG64 g_hitsRejected;

static FILE* g_log;
static LARGE_INTEGER g_qpcFreq;
static LARGE_INTEGER g_lastTick;
static double g_clock;  // seconds, advanced only while the game loop runs
static uint32_t g_generation;
static double g_nextStats;
static uint64_t g_removedTotal;

struct Tracked {
    void* object;
    double born;
    uint32_t generation;
    bool removed;
};
static std::unordered_map<void*, Tracked> g_tracked;

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

// weak_ptr::lock() on an MSVC _Ref_count_base.
static bool TryLock(void* ctrl) {
    volatile LONG* uses = reinterpret_cast<volatile LONG*>(static_cast<char*>(ctrl) + 8);
    LONG c = *uses;
    while (c != 0) {
        LONG prev = InterlockedCompareExchange(uses, c + 1, c);
        if (prev == c) return true;
        c = prev;
    }
    return false;
}

static void ReleaseShared(void* ctrl) {
    volatile LONG* uses = reinterpret_cast<volatile LONG*>(static_cast<char*>(ctrl) + 8);
    volatile LONG* weaks = reinterpret_cast<volatile LONG*>(static_cast<char*>(ctrl) + 12);
    if (InterlockedDecrement(uses) == 0) {
        void** vt = *reinterpret_cast<void***>(ctrl);
        reinterpret_cast<RefCountFn>(vt[0])(ctrl);  // _Destroy
        if (InterlockedDecrement(weaks) == 0) reinterpret_cast<RefCountFn>(vt[1])(ctrl);  // _Delete_this
    }
}

// Removes the corpse that owns `node` the same way the game evicts over-budget
// ragdolls: lock the weak reference, call SceneObject::Remove, release.
static bool RemoveCorpse(uint8_t* node) {
    void* object = *reinterpret_cast<void**>(node + 0x18);
    void* ctrl = *reinterpret_cast<void**>(node + 0x20);
    if (!object || !ctrl || !TryLock(ctrl)) return false;
    g_remove(object);
    ReleaseShared(ctrl);
    return true;
}

// ---------------------------------------------------------------------------
// Per-frame hook

static void TagCorpseBodies(const uint8_t* ragdoll, uint32_t gen) {
    const uint8_t* bones = *reinterpret_cast<uint8_t* const*>(ragdoll + 0x78);
    const size_t count = *reinterpret_cast<const size_t*>(ragdoll + 0x88);
    if (!bones || count > 512) return;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* wrapper = *reinterpret_cast<uint8_t* const*>(bones + i * 0xC0 + 0x50);
        if (!wrapper) continue;
        const uint32_t id = *reinterpret_cast<const uint32_t*>(wrapper + 0xF0);
        const uint32_t index = id & 0xFFFFFF;
        if (id == 0xFFFFFFFF || index >= kMaxBodies) continue;
        if (g_corpseTag[index] == 0) g_taggedIndices.push_back(index);
        g_corpseTag[index] = id + 1;
        g_tagGeneration[index] = gen;
    }
}

static void ClearAllTags() {
    for (uint32_t index : g_taggedIndices) g_corpseTag[index] = 0;
    g_taggedIndices.clear();
}

// Walks the RagdollManager's corpse list once per frame: ages and removes old
// corpses, and refreshes the hitbox tags.
static void WalkCorpses(uint8_t* manager) {
    uint8_t* head = *reinterpret_cast<uint8_t**>(manager + 0x08);
    if (!head) return;

    const uint32_t gen = ++g_generation;
    size_t guard = 0;
    for (uint8_t* node = *reinterpret_cast<uint8_t**>(head); node && node != head;
         node = *reinterpret_cast<uint8_t**>(node)) {
        if (++guard > 100000) break;
        void* object = *reinterpret_cast<void**>(node + 0x18);

        if (g_lifetimeActive) {
            Tracked& t = g_tracked[node];
            if (t.generation == 0 || t.object != object) {
                t.object = object;
                t.born = g_clock;
                t.removed = false;
            }
            t.generation = gen;
            if (!t.removed && g_clock - t.born >= g_corpseSeconds) {
                if (RemoveCorpse(node)) {
                    ++g_removedTotal;
                    if (g_logLevel >= 3) Log("removed corpse %p (age %.1fs)", object, g_clock - t.born);
                }
                t.removed = true;
            }
        }

        if (g_hitboxActive) {
            // The ragdoll controller lives inside the corpse object; only touch it while the object is alive.
            void* ctrl = *reinterpret_cast<void**>(node + 0x20);
            const uint8_t* ragdoll = *reinterpret_cast<uint8_t**>(node + 0x10);
            if (ctrl && ragdoll && *reinterpret_cast<volatile LONG*>(static_cast<char*>(ctrl) + 8) != 0)
                TagCorpseBodies(ragdoll, gen);
        }
    }

    if (g_lifetimeActive) {
        for (auto it = g_tracked.begin(); it != g_tracked.end();) {
            if (it->second.generation != gen)
                it = g_tracked.erase(it);
            else
                ++it;
        }
    }

    if (g_hitboxActive) {
        size_t keep = 0;
        for (uint32_t index : g_taggedIndices) {
            if (g_tagGeneration[index] == gen)
                g_taggedIndices[keep++] = index;
            else
                g_corpseTag[index] = 0;
        }
        g_taggedIndices.resize(keep);
    }
}

static int FilterAll(unsigned int) { return EXCEPTION_EXECUTE_HANDLER; }

static void WalkCorpsesGuarded(uint8_t* manager) {
    __try {
        WalkCorpses(manager);
    } __except (FilterAll(GetExceptionCode())) {
        g_lifetimeActive = false;
        g_hitboxActive = false;
        ClearAllTags();
        Log("exception 0x%08lx while walking corpses; plugin features disabled for this session",
            GetExceptionCode());
    }
}

// ---------------------------------------------------------------------------
// Collision filter hook (Havok worker threads)

template <int N>
static bool __fastcall QueryBodyFilterHook(void* filter, int queryType, const void* queryData, const uint8_t* body) {
    if (body) {
        const uint32_t id = *reinterpret_cast<const uint32_t*>(body + 0x50);
        const uint32_t index = id & 0xFFFFFF;
        if (index < kMaxBodies && g_corpseTag[index] == id + 1) {
            g_hitsRejected = g_hitsRejected + 1;  // stats only; races are fine
            return false;
        }
    }
    return g_origQueryBody[N](filter, queryType, queryData, body);
}

static void __fastcall HookedPurge(void* manager) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = static_cast<double>(now.QuadPart - g_lastTick.QuadPart) / g_qpcFreq.QuadPart;
    g_lastTick = now;
    // Clamp so time spent outside the game loop (loading, menus) is not counted.
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.1) dt = 0.1;
    g_clock += dt;

    uint8_t* m = static_cast<uint8_t*>(manager);
    if (g_acceptGenerator) *reinterpret_cast<volatile uint32_t*>(m + 0x18) |= 1u;
    if (g_lifetimeActive || g_hitboxActive) WalkCorpsesGuarded(m);

    if (g_logLevel >= 2 && g_clock >= g_nextStats) {
        g_nextStats = g_clock + 10.0;
        Log("stats: tracked=%zu manager_count=%llu removed_total=%llu corpse_bodies=%zu hits_ignored=%lld",
            g_tracked.size(), static_cast<unsigned long long>(*reinterpret_cast<size_t*>(m + 0x10)),
            static_cast<unsigned long long>(g_removedTotal), g_taggedIndices.size(),
            static_cast<long long>(g_hitsRejected));
    }

    g_purge(manager);
}

// ---------------------------------------------------------------------------
// Setup

static void LoadConfig(const wchar_t* dir) {
    wchar_t ini[MAX_PATH];
    swprintf_s(ini, L"%sEDF6CorpseCleanup.ini", dir);
    if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, ini, L"w") == 0 && f) {
            fputs(
                "[CorpseCleanup]\n"
                "; Seconds an enemy corpse may stay before it is removed.\n"
                "; Game default is 15-30 depending on enemy type. 0 = leave the game's timing alone.\n"
                "CorpseSeconds=5\n"
                "\n"
                "; Ragdoll budget: when the total cost of live corpses reaches this, the oldest\n"
                "; corpse is removed. Game default is 512 (256 on some settings). 0 = game default.\n"
                "RagdollBudget=0\n"
                "\n"
                "; 1 = also track corpses of enemies spawned by teleport anchors / generators\n"
                "; (the game only does this in a couple of missions). Same effect as\n"
                "; EDF6GeneratorBodyBudget; the two are compatible.\n"
                "AcceptGeneratorCorpses=1\n"
                "\n"
                "; 1 = corpses have no hitbox: bullets, explosions and line-of-sight checks pass\n"
                "; through them. Corpses still fall and rest on the ground normally.\n"
                "DisableCorpseHitbox=1\n"
                "\n"
                "; 0 = off, 1 = startup info, 2 = stats every 10 s, 3 = every removal\n"
                "Log=1\n",
                f);
            fclose(f);
        }
    }
    wchar_t buf[64];
    GetPrivateProfileStringW(L"CorpseCleanup", L"CorpseSeconds", L"5", buf, 64, ini);
    g_corpseSeconds = static_cast<float>(_wtof(buf));
    g_ragdollBudget = GetPrivateProfileIntW(L"CorpseCleanup", L"RagdollBudget", 0, ini);
    g_acceptGenerator = GetPrivateProfileIntW(L"CorpseCleanup", L"AcceptGeneratorCorpses", 1, ini) != 0;
    g_disableHitbox = GetPrivateProfileIntW(L"CorpseCleanup", L"DisableCorpseHitbox", 1, ini) != 0;
    g_logLevel = GetPrivateProfileIntW(L"CorpseCleanup", L"Log", 1, ini);

    if (g_corpseSeconds < 0.0f) g_corpseSeconds = 0.0f;
    if (g_corpseSeconds > 0.0f && g_corpseSeconds < 1.0f) g_corpseSeconds = 1.0f;
    if (g_ragdollBudget < 0) g_ragdollBudget = 0;
    if (g_ragdollBudget > 0 && g_ragdollBudget < 16) g_ragdollBudget = 16;
    if (g_ragdollBudget > 65536) g_ragdollBudget = 65536;
}

static bool InstallHitboxFilter() {
    for (const Signature& sig : kHitboxSignatures) {
        if (!MatchSignature(sig)) return false;
    }
    struct Slot {
        uintptr_t vtbl;
        uintptr_t expected;
        QueryBodyFilterFn hook;
    };
    const Slot slots[3] = {
        {rva::GroupFilter5Vtbl, rva::GroupFilter5QueryBody, &QueryBodyFilterHook<0>},
        {rva::GroupFilterVtbl, rva::GroupFilter5QueryBody, &QueryBodyFilterHook<1>},
        {rva::GroupFilter6Vtbl, rva::GroupFilter6QueryBody, &QueryBodyFilterHook<2>},
    };
    for (const Slot& s : slots) {
        void** entry = reinterpret_cast<void**>(g_image + s.vtbl) + rva::QueryBodySlot;
        if (reinterpret_cast<uintptr_t>(*entry) != g_image + s.expected) {
            Log("collision filter vtable at EDF.dll+%llx does not match", static_cast<unsigned long long>(s.vtbl));
            return false;
        }
    }
    for (int i = 0; i < 3; ++i) {
        void** entry = reinterpret_cast<void**>(g_image + slots[i].vtbl) + rva::QueryBodySlot;
        g_origQueryBody[i] = reinterpret_cast<QueryBodyFilterFn>(*entry);
        void* hook = reinterpret_cast<void*>(slots[i].hook);
        if (!WriteCode(entry, &hook, sizeof hook)) return false;
    }
    return true;
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

    g_purge = reinterpret_cast<PurgeFn>(g_image + rva::Purge);
    g_remove = reinterpret_cast<RemoveFn>(g_image + rva::RemoveObject);

    if (g_ragdollBudget > 0) {
        const uint32_t v = static_cast<uint32_t>(g_ragdollBudget);
        const uintptr_t sites[] = {rva::RegisterLimitHi, rva::RegisterLimitLo, rva::LimitFnHi, rva::LimitFnLo};
        for (uintptr_t s : sites) WriteCode(reinterpret_cast<void*>(g_image + s), &v, sizeof v);
        Log("ragdoll budget set to %u", v);
    }

    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_lastTick);
    g_lifetimeActive = g_corpseSeconds > 0.0f;
    g_taggedIndices.reserve(4096);

    // jmp qword ptr [rip+0]; dq HookedPurge
    uint8_t* stub = static_cast<uint8_t*>(AllocNear(g_image + rva::PurgeCall));
    if (!stub) {
        Log("could not allocate a stub near EDF.dll");
        return false;
    }
    const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(stub, jmp, sizeof jmp);
    const uint64_t target = reinterpret_cast<uint64_t>(&HookedPurge);
    memcpy(stub + 6, &target, sizeof target);
    FlushInstructionCache(GetCurrentProcess(), stub, 14);

    uint8_t* call = reinterpret_cast<uint8_t*>(g_image + rva::PurgeCall);
    const int64_t rel = reinterpret_cast<int64_t>(stub) - reinterpret_cast<int64_t>(call + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) {
        Log("stub out of rel32 range");
        return false;
    }
    const int32_t rel32 = static_cast<int32_t>(rel);
    if (!WriteCode(call + 1, &rel32, sizeof rel32)) {
        Log("VirtualProtect failed on purge call site");
        return false;
    }

    // Enabled only after the purge hook is live, so tags are always maintained.
    if (g_disableHitbox) {
        g_hitboxActive = InstallHitboxFilter();
        if (!g_hitboxActive) Log("corpse hitbox removal unavailable on this EDF.dll; other features still active");
    }

    Log("installed: CorpseSeconds=%.1f RagdollBudget=%d AcceptGeneratorCorpses=%d DisableCorpseHitbox=%d Log=%d",
        g_corpseSeconds, g_ragdollBudget, g_acceptGenerator ? 1 : 0, g_hitboxActive ? 1 : 0, g_logLevel);
    return true;
}

extern "C" __declspec(dllexport) bool __fastcall EML6_Load(PluginInfo* info) {
    if (!info) return false;
    info->infoVersion = 1;
    info->name = "EDF6 Corpse Cleanup";
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
        swprintf_s(logPath, L"%sEDF6CorpseCleanup.log", dir);
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
