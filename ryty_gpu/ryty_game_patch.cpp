// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// GTS-001: run-time patches of GT Sport's code (CUSA03220 v1.69), applied in DllMain before the
// game's code runs. Each patch checks the original bytes first and is skipped when they differ
// (another game version, another converter build). The game's files are not changed.
//
// RYGT_PATCH=<rva>:<original hex>:<new hex>[,...] applies arbitrary patches the same way, for
// experiments (RVA in GTSport.exe: eboot.bin address + 0x10000).

#include <cstdio>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <fmt/format.h>

#include "common/perf_monitor.h"
#include "common/types.h"
#include "core/debug_state.h"

namespace Vulkan {
void RytyWatchWrites(std::uint64_t address, std::uint32_t ms, std::string label); // DIAG-067
}

namespace {

struct Patch {
    u64 rva;
    std::vector<u8> original;
    std::vector<u8> replacement;
    std::string name;
};

std::vector<u8> ParseHex(const std::string& text) {
    std::vector<u8> bytes;
    for (size_t i = 0; i + 1 < text.size(); i += 2) {
        bytes.push_back(static_cast<u8>(std::strtoul(text.substr(i, 2).c_str(), nullptr, 16)));
    }
    return bytes;
}

bool Apply(u8* base, const Patch& patch) {
    u8* target = base + patch.rva;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(target, &info, sizeof(info)) || info.State != MEM_COMMIT) {
        std::fprintf(stderr, "[patch] %s: %#llx is not mapped\n", patch.name.c_str(),
                     static_cast<unsigned long long>(patch.rva));
        return false;
    }
    if (patch.original.size() != patch.replacement.size() ||
        std::memcmp(target, patch.original.data(), patch.original.size()) != 0) {
        std::fprintf(stderr, "[patch] %s: original bytes at %#llx differ, not applied\n",
                     patch.name.c_str(), static_cast<unsigned long long>(patch.rva));
        return false;
    }
    DWORD old_protect = 0;
    if (!VirtualProtect(target, patch.replacement.size(), PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }
    std::memcpy(target, patch.replacement.data(), patch.replacement.size());
    DWORD ignored = 0;
    VirtualProtect(target, patch.replacement.size(), old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, patch.replacement.size());
    std::fprintf(stderr, "[patch] %s applied at %#llx\n", patch.name.c_str(),
                 static_cast<unsigned long long>(patch.rva));
    return true;
}

// DIAG-097 (temporary): GT Sport's per-course view distances, GTRender::EnvironmentSetting::Misc
// (reflection field list at eboot 0x2be6ac0; defaults set by its constructor at 0x6c7370). The
// three calls of that constructor are redirected through a stub so every Misc instance is known;
// a thread logs their values when the game changes them (RYGT_ENV_DIAG=1) and can scale chosen
// fields (RYGT_ENV_SCALE=far=0.5,farPassFar=0.5,...) to test how draw counts react.
struct MiscField {
    const char* name;
    u32 offset;
};
constexpr MiscField kMiscFields[] = {
    {"far", 0x0},          {"baseNear", 0x4},        {"extra_far", 0xc},
    {"depthPassFar", 0x10}, {"envFar", 0x18},         {"localEnvFar", 0x1c},
    {"mirrorFar", 0x24},    {"reflectFar", 0x30},     {"farPassBaseNear", 0x34},
    {"farPassFar", 0x38},   {"occluderFar", 0x3c},    {"tree_far", 0xbc},
    {"tree_far_mirror", 0xc0}, {"tree_lod_off0", 0xc4}, {"tree_lod_off1", 0xc8},
    {"tree_lod_off2", 0xcc},
    // EnvironmentSetting::lod (LODSetting at 0x54c): discreteBase, smoothMeshLODBase,
    // nonOnboardLODScale. Only meaningful for instances built by the EnvironmentSetting
    // constructor (site 0).
    {"lod.discreteBase", 0x54c}, {"lod.smoothMeshLODBase", 0x550},
    {"lod.nonOnboardLODScale", 0x558},
};
constexpr u64 kMiscCtor = 0x6c7370;
constexpr u64 kMiscCtorCalls[] = {0x10de5b9, 0x6c712a, 0x6c8faa};
constexpr u64 kTextRvaShift = 0x10000;

using MiscCtorFn = void(PS4_SYSV_ABI*)(void*);
MiscCtorFn g_misc_ctor = nullptr;

struct MiscInstance {
    u8* base;
    int site;
    float seen[std::size(kMiscFields)];    // last value the game left there
    float written[std::size(kMiscFields)]; // last value written by the scale override
    std::string reject;
};
std::mutex g_misc_mutex;
std::vector<MiscInstance> g_misc;
float g_misc_scale[std::size(kMiscFields)];
bool g_misc_any_scale = false;

bool ReadableRange(const void* address, size_t size) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    return static_cast<const u8*>(address) + size <=
           static_cast<const u8*>(info.BaseAddress) + info.RegionSize;
}

// Instances are often temporaries that the game frees again; one whose memory was reused must
// not be written. Only whole EnvironmentSettings whose near/far planes and four LODSettings all
// hold plausible values pass.
bool LooksLikeEnvironmentSetting(const u8* base, std::string* reject_log = nullptr) {
    if (!ReadableRange(base, 0x590)) {
        return false;
    }
    const auto f = [base](u32 offset) {
        float value;
        std::memcpy(&value, base + offset, 4);
        return value;
    };
    const auto in = [](float value, float lo, float hi) { return value >= lo && value <= hi; };
    if (!in(f(0x0), 10.0f, 1e6f) || !in(f(0x4), 0.001f, 10.0f) || !in(f(0x8), 0.01f, 10.0f) ||
        !in(f(0x14), 0.001f, 100.0f) || !in(f(0x18), 1.0f, 1e6f) || !in(f(0x1c), 1.0f, 1e7f) ||
        !in(f(0x24), 1.0f, 1e6f) || !in(f(0x28), 1.0f, 1e6f) || !in(f(0x2c), 0.001f, 100.0f) ||
        base[0x40] > 1) {
        return false;
    }
    for (const u32 lod : {0x54c, 0x55c, 0x56c, 0x57c}) {
        if (!in(f(lod), 0.0f, 100.0f) || !in(f(lod + 4), 0.0f, 100.0f) || base[lod + 8] > 1 ||
            !in(f(lod + 0xc), 0.001f, 10.0f)) {
            if (reject_log) {
                *reject_log = fmt::format("LODSetting at {:#x}: {} {} {} {}", lod, f(lod),
                                          f(lod + 4), base[lod + 8], f(lod + 0xc));
            }
            return false;
        }
    }
    return true;
}

void MiscMonitor() {
    // RYGT_ENV_AB=<seconds>: the scale is on and off for alternating periods of the same run,
    // with draws per frame and frame rate logged for each state (run-to-run differences of the
    // test race are larger than the effects measured).
    const char* ab_env = std::getenv("RYGT_ENV_AB");
    const u32 ab_ticks = ab_env ? static_cast<u32>(std::atof(ab_env) * 10.0) : 0;
    struct AbTotals {
        u64 draws, frames, ticks;
    } ab[2]{};
    u64 last_draws = Common::GetWorkCounters().draws.load();
    u64 last_frames = DebugState.GetFrameNum();
    for (u32 tick = 0;; ++tick) {
        Sleep(100);
        const bool scale_on = ab_ticks == 0 || (tick / ab_ticks) % 2 == 0;
        if (ab_ticks) {
            const u64 draws = Common::GetWorkCounters().draws.load();
            const u64 frames = DebugState.GetFrameNum();
            // The first tick after a switch still shows the previous state's frames.
            if (tick % ab_ticks != 0) {
                auto& totals = ab[scale_on ? 0 : 1];
                totals.draws += draws - last_draws;
                totals.frames += frames - last_frames;
                ++totals.ticks;
            }
            last_draws = draws;
            last_frames = frames;
            if (tick % 200 == 199) {
                const auto line = [](const AbTotals& t) {
                    return fmt::format("{:.0f} draws/frame, {:.1f} fps",
                                       double(t.draws) / std::max<u64>(1, t.frames),
                                       t.frames * 10.0 / std::max<u64>(1, t.ticks));
                };
                std::fprintf(stderr, "[env] DIAG-097 A/B: scaled %s; original %s\n",
                             line(ab[0]).c_str(), line(ab[1]).c_str());
                std::fflush(stderr);
                ab[0] = ab[1] = {};
            }
        }
        std::scoped_lock lock{g_misc_mutex};
        for (auto& instance : g_misc) {
            const size_t span = 0x590;
            std::string reject;
            if (instance.site != 0 || !LooksLikeEnvironmentSetting(instance.base, &reject)) {
                if (!reject.empty() && reject != instance.reject) {
                    std::fprintf(stderr, "[env] DIAG-097 misc %p rejected: %s\n", instance.base,
                                 reject.c_str());
                    instance.reject = reject;
                }
                continue;
            }
            std::string changes;
            for (size_t i = 0; i < std::size(kMiscFields); ++i) {
                if (kMiscFields[i].offset >= span) {
                    continue;
                }
                float* field = reinterpret_cast<float*>(instance.base + kMiscFields[i].offset);
                const float value = *field;
                if (value == instance.written[i]) {
                    if (!scale_on) {
                        *field = instance.seen[i]; // A/B: back to the game's value
                        instance.written[i] = -1.0f;
                    }
                    continue; // our own override
                }
                if (value != instance.seen[i]) {
                    changes += fmt::format(" {}={}", kMiscFields[i].name, value);
                    instance.seen[i] = value;
                }
                // A copy of another instance's already scaled value is not scaled again.
                const bool copied_override = std::ranges::any_of(
                    g_misc, [&](const MiscInstance& other) { return other.written[i] == value; });
                if (scale_on && g_misc_scale[i] != 1.0f && value > 0.0f && !copied_override) {
                    const float scaled = value * g_misc_scale[i];
                    *field = scaled;
                    instance.written[i] = scaled;
                }
            }
            if (!changes.empty()) {
                std::fprintf(stderr, "[env] DIAG-097 misc %p (site %d):%s\n", instance.base,
                             instance.site, changes.c_str());
                std::fflush(stderr);
            }
            // RYGT_ENV_WATCH=<hex offset>: which code reads that field of the first instance that
            // gets a course's values (a hardware read/write watch, at once and again 40 s later).
            static const char* watch_env = std::getenv("RYGT_ENV_WATCH");
            static const u32 watch_offset =
                watch_env ? static_cast<u32>(std::strtoul(watch_env, nullptr, 16)) : 0;
            static u8* watched = nullptr;
            static u32 watch_tick = 0;
            float far_plane;
            std::memcpy(&far_plane, instance.base, 4);
            if (watch_env && !watched && far_plane != 1000.0f) {
                watched = instance.base;
                watch_tick = tick;
                Vulkan::RytyWatchWrites(reinterpret_cast<u64>(watched) + watch_offset, 3000,
                                        fmt::format("rw env+{:#x}", watch_offset));
            } else if (watch_env && watched == instance.base && tick == watch_tick + 400) {
                Vulkan::RytyWatchWrites(reinterpret_cast<u64>(watched) + watch_offset, 2000,
                                        fmt::format("rw env+{:#x} later", watch_offset));
            }
        }
    }
}

void RecordMisc(void* self, int site) {
    static std::once_flag started;
    std::call_once(started, [] {
        std::thread{MiscMonitor}.detach();
    });
    std::scoped_lock lock{g_misc_mutex};
    for (auto& instance : g_misc) {
        if (instance.base == self) {
            instance.site = site;
            std::fill(std::begin(instance.seen), std::end(instance.seen), -1.0f);
            return;
        }
    }
    MiscInstance instance{static_cast<u8*>(self), site, {}, {}, {}};
    std::fill(std::begin(instance.seen), std::end(instance.seen), -1.0f);
    std::fill(std::begin(instance.written), std::end(instance.written), -1.0f);
    g_misc.push_back(instance);
    // RYGT_ENV_WATCH_NEW=<hex offset>: reads and writes of that field of each new
    // EnvironmentSetting for its first 1.5 s (skipped while another watch runs).
    // (Not during boot: the watch logs through the renderer's log, which starts later.)
    static const auto first_record = std::chrono::steady_clock::now();
    if (const char* env = std::getenv("RYGT_ENV_WATCH_NEW");
        env && site == 0 && std::chrono::steady_clock::now() - first_record > std::chrono::seconds{30}) {
        const u32 offset = static_cast<u32>(std::strtoul(env, nullptr, 16));
        Vulkan::RytyWatchWrites(reinterpret_cast<u64>(self) + offset, 1500,
                                fmt::format("rw new env {} +{:#x}", self, offset));
    }
}

template <int Site>
void PS4_SYSV_ABI MiscCtorHook(void* self) {
    g_misc_ctor(self);
    RecordMisc(self, Site);
}

// Memory for jump stubs within +-2 GB of the game's code, so its rel32 calls can reach them.
u8* AllocNear(u8* base) {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const u64 granularity = system.dwAllocationGranularity;
    for (u64 distance = granularity; distance < (1ULL << 30); distance += granularity) {
        for (const s64 sign : {-1, 1}) {
            const u64 address =
                (reinterpret_cast<u64>(base) + sign * static_cast<s64>(distance)) & ~(granularity - 1);
            if (void* memory = VirtualAlloc(reinterpret_cast<void*>(address), 0x1000,
                                            MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) {
                return static_cast<u8*>(memory);
            }
        }
    }
    return nullptr;
}

// GTS-002: the Lewis Hamilton Time Trial Challenge. v1.69 is Polyphony's end-of-service build: its
// main menu (gtmode.ad, setupDisplay) shows the mode only when the player's inventory says the free
// add-on was downloaded, i.e. when the KeyValue ints unlock_dlc_gm_LewisHamilton and
// dlc_gm_validLewisHamilton are 1. The servers that set them are gone, but all ten events, Lewis's
// ghosts and voice-overs are in the game's files. The KeyValue lookup that MKeyValue::getInt calls
// answers 1 for those two keys; every other key is untouched. RYGT_LEWIS=0 turns this off.
constexpr u64 kKeyValueGetIntCall = 0x212204; // call of the lookup inside MKeyValue::getInt
constexpr u64 kKeyValueLookup = 0xbae080;     // bool lookup(store, const char* key, s64* value)

using KeyValueLookupFn = bool(PS4_SYSV_ABI*)(void*, const char*, s64*);
KeyValueLookupFn g_key_value_lookup = nullptr;

bool PS4_SYSV_ABI KeyValueLookupHook(void* store, const char* key, s64* value) {
    const bool found = g_key_value_lookup(store, key, value);
    if (key && (std::strcmp(key, "unlock_dlc_gm_LewisHamilton") == 0 ||
                std::strcmp(key, "dlc_gm_validLewisHamilton") == 0)) {
        *value = 1;
        return true;
    }
    return found;
}

// Points the rel32 call at `call_rva` (which must call `target_rva`) at `hook` through a stub.
bool RedirectCall(u8* base, u8* stub, u64 call_rva, u64 target_rva, void* hook, const char* name) {
    u8* call = base + call_rva + kTextRvaShift;
    s32 rel = 0;
    std::memcpy(&rel, call + 1, 4);
    if (call[0] != 0xe8 || call + 5 + rel != base + target_rva + kTextRvaShift) {
        std::fprintf(stderr, "[patch] %s: call at %#llx differs, not applied\n", name,
                     static_cast<unsigned long long>(call_rva));
        return false;
    }
    const u8 jump[6] = {0xff, 0x25, 0, 0, 0, 0}; // jmp qword ptr [rip]
    std::memcpy(stub, jump, 6);
    std::memcpy(stub + 6, &hook, 8);
    FlushInstructionCache(GetCurrentProcess(), stub, 16);
    const s32 new_rel = static_cast<s32>(stub - (call + 5));
    DWORD old_protect = 0;
    if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }
    std::memcpy(call + 1, &new_rel, 4);
    DWORD ignored = 0;
    VirtualProtect(call, 5, old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    std::fprintf(stderr, "[patch] %s applied at %#llx\n", name,
                 static_cast<unsigned long long>(call_rva));
    return true;
}

void InstallLewisChallenge(u8* base) {
    const char* env = std::getenv("RYGT_LEWIS");
    if (env && env[0] == '0') {
        return;
    }
    u8* stub = AllocNear(base);
    if (!stub) {
        std::fprintf(stderr, "[patch] GTS-002: no memory near the game's code for the hook\n");
        return;
    }
    g_key_value_lookup = reinterpret_cast<KeyValueLookupFn>(base + kKeyValueLookup + kTextRvaShift);
    RedirectCall(base, stub, kKeyValueGetIntCall, kKeyValueLookup,
                 reinterpret_cast<void*>(&KeyValueLookupHook), "GTS-002 Lewis Hamilton Challenge");
}

// GTS-003: the Livery Editor offline. The main menu (gtmode.ad, setupDisplay) disables the Livery
// button whenever the game is offline (`disable: [!enable_menu || DisableLivery]`, enable_menu =
// online), although the editor itself has an offline mode that saves liveries locally. The menu
// script is patched while the game reads it: that one condition uses completed_playgo (true once the
// game is installed) instead of enable_menu, like the Garage button. The bytes are only changed
// where the whole original sequence (78 bytes of v1.69's GTMODE.ADC) is found in one 4 KB read.
// RYGT_LIVERY=0 turns this off.
struct ScriptPatch {
    const char* name;
    std::vector<u8> original;
    size_t offset; // of `replacement` inside `original`
    std::vector<u8> replacement;
};
std::vector<ScriptPatch> g_script_patches;

struct GuestIoVec {
    u8* base;
    u64 length;
};
// The game's file read (eboot 0x1be7930); its buffered streams refill 4 KB at a time from these
// three calls, which is how scripts are read.
constexpr u64 kFileRead = 0x1be7930;
constexpr u64 kStreamRefillCalls[] = {0x17626ef, 0x170f88e, 0x1762a38};
using FileReadFn = s64(PS4_SYSV_ABI*)(void*, GuestIoVec*);
FileReadFn g_file_read = nullptr;

s64 PS4_SYSV_ABI StreamRefillHook(void* file, GuestIoVec* iov) {
    const s64 read = g_file_read(file, iov);
    if (read <= 0 || !iov || !iov->base) {
        return read;
    }
    u8* const begin = iov->base;
    u8* const end = begin + read;
    for (const auto& patch : g_script_patches) {
        u8* found = std::search(begin, end, patch.original.begin(), patch.original.end());
        if (found != end) {
            std::memcpy(found + patch.offset, patch.replacement.data(), patch.replacement.size());
            std::fprintf(stderr, "[patch] %s applied to the menu script\n", patch.name);
        }
    }
    return read;
}

void InstallScriptPatches(u8* base) {
    const char* livery = std::getenv("RYGT_LIVERY");
    if (!(livery && livery[0] == '0')) {
        // STRING_CONST 'disable', ARRAY_CONST 1, VARIABLE_EVAL enable_menu [7], UNARY_OPERATOR
        // __not__, LOGICAL_OR, VARIABLE_EVAL VariableManager.get, STRING_CONST
        // 'GameDemo::DisableLivery', CALL 1. enable_menu [7] becomes completed_playgo [2].
        g_script_patches.push_back(
            {"GTS-003 Livery Editor offline",
             ParseHex("940200001d811e940200003201000000940200002601000000811c070000009402000022160000"
                      "0000407001000094020000260300000076777800000000940200001d8142940200000601000000"),
             25, ParseHex("811302000000")});
    }
    if (g_script_patches.empty()) {
        return;
    }
    u8* stubs = AllocNear(base);
    if (!stubs) {
        std::fprintf(stderr, "[patch] GTS-003: no memory near the game's code for the hooks\n");
        return;
    }
    g_file_read = reinterpret_cast<FileReadFn>(base + kFileRead + kTextRvaShift);
    for (size_t i = 0; i < std::size(kStreamRefillCalls); ++i) {
        RedirectCall(base, stubs + i * 16, kStreamRefillCalls[i], kFileRead,
                     reinterpret_cast<void*>(&StreamRefillHook), "GTS-003 script reads");
    }
}

void InstallMiscHooks(u8* base) {
    const char* diag = std::getenv("RYGT_ENV_DIAG");
    const char* scale = std::getenv("RYGT_ENV_SCALE");
    if (!(diag && diag[0] == '1') && !scale) {
        return;
    }
    std::fill(std::begin(g_misc_scale), std::end(g_misc_scale), 1.0f);
    if (scale) {
        std::string list{scale};
        for (size_t start = 0; start < list.size();) {
            const size_t end = std::min(list.find(',', start), list.size());
            const std::string item = list.substr(start, end - start);
            const size_t eq = item.find('=');
            for (size_t i = 0; eq != std::string::npos && i < std::size(kMiscFields); ++i) {
                if (item.compare(0, eq, kMiscFields[i].name) == 0 &&
                    std::strlen(kMiscFields[i].name) == eq) {
                    g_misc_scale[i] = std::strtof(item.c_str() + eq + 1, nullptr);
                    g_misc_any_scale = true;
                    std::fprintf(stderr, "[env] scaling %s by %g\n", kMiscFields[i].name,
                                 g_misc_scale[i]);
                }
            }
            start = end + 1;
        }
    }
    u8* stubs = AllocNear(base);
    if (!stubs) {
        std::fprintf(stderr, "[env] no memory near the game's code for hook stubs\n");
        return;
    }
    g_misc_ctor = reinterpret_cast<MiscCtorFn>(base + kMiscCtor + kTextRvaShift);
    void* hooks[] = {reinterpret_cast<void*>(&MiscCtorHook<0>),
                     reinterpret_cast<void*>(&MiscCtorHook<1>),
                     reinterpret_cast<void*>(&MiscCtorHook<2>)};
    for (size_t i = 0; i < std::size(kMiscCtorCalls); ++i) {
        u8* call = base + kMiscCtorCalls[i] + kTextRvaShift;
        s32 rel = 0;
        std::memcpy(&rel, call + 1, 4);
        if (call[0] != 0xe8 || call + 5 + rel != base + kMiscCtor + kTextRvaShift) {
            std::fprintf(stderr, "[env] call at %#llx is not the Misc constructor, not hooked\n",
                         static_cast<unsigned long long>(kMiscCtorCalls[i]));
            continue;
        }
        u8* stub = stubs + i * 16;
        const u8 jump[6] = {0xff, 0x25, 0, 0, 0, 0}; // jmp qword ptr [rip]
        std::memcpy(stub, jump, 6);
        std::memcpy(stub + 6, &hooks[i], 8);
        const s32 new_rel = static_cast<s32>(stub - (call + 5));
        DWORD old_protect = 0;
        VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old_protect);
        std::memcpy(call + 1, &new_rel, 4);
        DWORD ignored = 0;
        VirtualProtect(call, 5, old_protect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), call, 5);
    }
    FlushInstructionCache(GetCurrentProcess(), stubs, 0x1000);
    std::fprintf(stderr, "[env] DIAG-097 Misc constructor hooks installed\n");
}

} // namespace

void RytyApplyGamePatches() {
    InstallMiscHooks(reinterpret_cast<u8*>(GetModuleHandleA(nullptr)));
    InstallLewisChallenge(reinterpret_cast<u8*>(GetModuleHandleA(nullptr)));
    InstallScriptPatches(reinterpret_cast<u8*>(GetModuleHandleA(nullptr)));
    auto* base = reinterpret_cast<u8*>(GetModuleHandleA(nullptr));
    char buffer[4096];
    const DWORD length = GetEnvironmentVariableA("RYGT_PATCH", buffer, sizeof(buffer));
    if (length == 0 || length >= sizeof(buffer)) {
        return;
    }
    std::string list{buffer, length};
    size_t start = 0;
    while (start < list.size()) {
        const size_t end = std::min(list.find(',', start), list.size());
        const std::string item = list.substr(start, end - start);
        const size_t a = item.find(':');
        const size_t b = item.find(':', a + 1);
        if (a != std::string::npos && b != std::string::npos) {
            Patch patch{std::strtoull(item.substr(0, a).c_str(), nullptr, 16),
                        ParseHex(item.substr(a + 1, b - a - 1)), ParseHex(item.substr(b + 1)),
                        "RYGT_PATCH " + item.substr(0, a)};
            Apply(base, patch);
        }
        start = end + 1;
    }
}
