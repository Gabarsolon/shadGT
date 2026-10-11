// Entry for the renderer library (libSceVideoOut.prx, which also exports libSceGnmDriver): opens
// the window on its own thread, then registers shadGT's Gnm driver and video output.

#include <windows.h>
#include <condition_variable>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/logging/log.h"
#include "common/perf_monitor.h"
#include "common/singleton.h"
#include "core/emulator_settings.h"
#include "core/user_settings.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#include "core/libraries/videoout/video_out.h"
#include "core/memory.h"
#include "core/signals.h"
#include "core/loader/symbols_resolver.h"
#include "input/controller.h"
#include "ryty_gpu/ryty_input_bridge.h"
#include "ryty_gpu/ryty_library.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "core/debug_state.h"
#include "imgui.h"
#ifdef ENABLE_DISCORD_RPC
#include "common/discord_rpc_handler.h"
#endif
#include <chrono>

namespace Libraries::SystemService {
void NoteGameDrew(); // ryty_stubs.cpp: the splash screen ends at the game's first drawn frame
}

Frontend::WindowSDL* g_window = nullptr;

namespace {

std::mutex g_window_mutex;
std::condition_variable g_window_cv;
bool g_window_ready = false;
bool g_renderer_ready = false;

void RunWindowThread() {
    auto* controllers = Common::Singleton<Input::GameControllers>::Instance();
    static Frontend::WindowSDL window(EmulatorSettings.GetWindowWidth(),
                                      EmulatorSettings.GetWindowHeight(), controllers,
                                      "ryGT - Gran Turismo Sport");
    window.SetIcon({}); // shadGT's Gran Turismo Sport icon
    {
        std::scoped_lock lock{g_window_mutex};
        g_window = &window;
        g_window_ready = true;
    }
    g_window_cv.notify_all();
    // Events reach ImGui, which the presenter initializes, so pump only once it exists.
    {
        std::unique_lock lock{g_window_mutex};
        g_window_cv.wait(lock, [] { return g_renderer_ready; });
    }
    window.InitTimers();
    while (window.IsOpen()) {
        window.WaitEvent();
    }
    // The window was closed: end the game without DLL teardown (see Emulator::Shutdown).
    TerminateProcess(GetCurrentProcess(), 0);
}

// Signals the named event Ryty's save data library waits on before GT Sport's first save mount
// once the game has drawn its first frame (the boot screen), so loading cannot finish before the
// boot screen is listening for it.
void StartFirstDrawWatcher() {
    char name[64];
    std::snprintf(name, sizeof(name), "Local\\RytyFirstDraw-%lu", GetCurrentProcessId());
    HANDLE event = CreateEventA(nullptr, TRUE, FALSE, name);
    if (event == nullptr) {
        return;
    }
    std::thread([event] {
        while (Common::GetWorkCounters().draws.load(std::memory_order_relaxed) == 0) {
            Sleep(2);
        }
        SetEvent(event);
        Libraries::SystemService::NoteGameDrew(); // ends the splash screen
    }).detach();
}

// The FPS counter (shadGT's Show FPS Counter setting): the game's frames per second, in small
// type in the window's top left corner, over the game and the splash screen.
void DrawFpsCounter() {
    if (!EmulatorSettings.IsShowFpsCounter()) {
        return;
    }
    using Clock = std::chrono::steady_clock;
    static Clock::time_point window_start = Clock::now();
    static u32 window_frames = DebugState.GetFrameNum();
    static double fps = 0.0;
    const auto now = Clock::now();
    const double seconds = std::chrono::duration<double>(now - window_start).count();
    if (seconds >= 0.5) {
        const u32 frames = DebugState.GetFrameNum();
        fps = (frames - window_frames) / seconds;
        window_frames = frames;
        window_start = now;
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f FPS", fps);
    auto* draw = ImGui::GetForegroundDrawList();
    const ImVec2 origin = ImGui::GetMainViewport()->Pos;
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 at{origin.x + 8.0f, origin.y + 6.0f};
    draw->AddRectFilled({at.x - 4.0f, at.y - 2.0f}, {at.x + size.x + 4.0f, at.y + size.y + 2.0f},
                        IM_COL32(0, 0, 0, 140), 3.0f);
    draw->AddText(at, IM_COL32(235, 235, 235, 255), text);
}

// DIAG-047 (SHADGT_HEAP_REPORT=1): every 20 s, the largest heap's busy blocks by size, with the
// first bytes of one block of each size. The walk holds the heap's lock, so nothing it does may
// allocate: the table is allocated before.
void StartHeapReport() {
    const char* env = std::getenv("SHADGT_HEAP_REPORT");
    if (env == nullptr) {
        return;
    }
    // "1": every 20 s; a larger number: every that many seconds.
    const int seconds = std::atoi(env);
    const DWORD interval_ms = DWORD(seconds > 1 ? seconds : 20) * 1000;
    std::thread([interval_ms] {
        struct Slot {
            u64 size;
            u64 count;
            u8 sample[128];
        };
        constexpr size_t TableSize = 1 << 16;
        std::vector<Slot> table(TableSize);
        std::vector<HANDLE> heaps(256);
        for (;;) {
            Sleep(interval_ms);
            const DWORD number = GetProcessHeaps(static_cast<DWORD>(heaps.size()), heaps.data());
            HANDLE largest = nullptr;
            u64 largest_bytes = 0;
            for (DWORD h = 0; h < std::min<DWORD>(number, heaps.size()); ++h) {
                std::fill(table.begin(), table.end(), Slot{});
                u64 bytes = 0, blocks = 0, dropped = 0;
                if (!HeapLock(heaps[h])) {
                    continue;
                }
                PROCESS_HEAP_ENTRY entry{};
                while (HeapWalk(heaps[h], &entry)) {
                    if (!(entry.wFlags & PROCESS_HEAP_ENTRY_BUSY)) {
                        continue;
                    }
                    bytes += entry.cbData;
                    ++blocks;
                    size_t i = (entry.cbData * 0x9E3779B97F4A7C15ull) >> 48;
                    size_t probes = 0;
                    while (table[i].count != 0 && table[i].size != entry.cbData &&
                           probes++ < 64) {
                        i = (i + 1) % TableSize;
                    }
                    if (table[i].count == 0) {
                        table[i].size = entry.cbData;
                        std::memcpy(table[i].sample, entry.lpData,
                                    std::min<size_t>(entry.cbData, sizeof(table[i].sample)));
                    } else if (table[i].size != entry.cbData) {
                        ++dropped;
                        continue;
                    }
                    ++table[i].count;
                }
                HeapUnlock(heaps[h]);
                if (bytes < (64ull << 20)) {
                    continue;
                }
                std::vector<Slot> top;
                for (const auto& slot : table) {
                    if (slot.count != 0) {
                        top.push_back(slot);
                    }
                }
                std::ranges::sort(top, [](const Slot& a, const Slot& b) {
                    return a.size * a.count > b.size * b.count;
                });
                std::string lines;
                for (size_t t = 0; t < std::min<size_t>(top.size(), 10); ++t) {
                    lines += fmt::format("\n  {} x {} bytes ({} MB):", top[t].count, top[t].size,
                                         top[t].size * top[t].count >> 20);
                    for (size_t b = 0; b < std::min<size_t>(top[t].size, 128); ++b) {
                        lines += fmt::format("{}{:02x}", b % 8 ? "" : " ", top[t].sample[b]);
                    }
                    // Qwords pointing into a module (vtables, code), as module+offset.
                    for (size_t q = 0; q + 8 <= std::min<size_t>(top[t].size, 128); q += 8) {
                        u64 value;
                        std::memcpy(&value, top[t].sample + q, 8);
                        HMODULE module = nullptr;
                        char name[MAX_PATH];
                        if (value > 0x10000 &&
                            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                               reinterpret_cast<LPCSTR>(value), &module) &&
                            GetModuleFileNameA(module, name, sizeof(name))) {
                            const char* base = std::strrchr(name, '\\');
                            lines += fmt::format(" [+{}: {}+{:#x}]", q, base ? base + 1 : name,
                                                 value - reinterpret_cast<u64>(module));
                        }
                    }
                }
                LOG_WARNING(Lib_GnmDriver,
                            "DIAG-047: heap {} ({} of {}): {} MB in {} busy blocks{}{}",
                            fmt::ptr(heaps[h]), h, number, bytes >> 20, blocks,
                            dropped ? fmt::format(" ({} not counted by size)", dropped) : "",
                            lines);
            }
        }
    }).detach();
}

} // namespace

void Ryty::RytyLibraryInitialize() {
    InitializeCommon("shadgt_ryty.log");
    LOG_INFO(Lib_GnmDriver, "Ryty: starting shadGT renderer");
    // shadGT's main() loads the player list (user/users.json, created with defaults when
    // missing); a gamepad is only assigned to a player that exists ("User 1 not found").
    UserSettings.Load();
    if (std::getenv("RYTY_SELFTEST_GUARD") != nullptr) {
        // A guarded read of an inaccessible page must come back false, not end the process.
        void* page = VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
        u8 byte = 0;
        const bool copied = Core::RytyMemory::GuardedCopy(&byte, page, 1);
        LOG_WARNING(Lib_GnmDriver, "Ryty: guarded read self-test: {}",
                    copied ? "read an inaccessible page?" : "fault caught");
        VirtualFree(page, 0, MEM_RELEASE);
    }

    std::thread(RunWindowThread).detach();
    {
        std::unique_lock lock{g_window_mutex};
        g_window_cv.wait(lock, [] { return g_window_ready; });
    }

    Core::Loader::SymbolsResolver resolver;
    Libraries::GnmDriver::RegisterLib(&resolver);
    Libraries::VideoOut::RegisterLib(&resolver);
    BindExports();
    StartInputBridge();
    StartFirstDrawWatcher();
    StartHeapReport();
    Ryty::SetOverlayDraw(&DrawFpsCounter);
#ifdef ENABLE_DISCORD_RPC
    if (EmulatorSettings.IsDiscordRPCEnabled()) {
        auto* rpc = Common::Singleton<DiscordRPCHandler::RPC>::Instance();
        rpc->init();
        rpc->setStatusPlaying("Gran Turismo Sport", "CUSA03220");
    }
#endif
    {
        std::scoped_lock lock{g_window_mutex};
        g_renderer_ready = true;
    }
    g_window_cv.notify_all();
}

// Ryty's libkernel maps and unmaps guest memory, so it reports those changes here; shadGT's own
// memory manager would otherwise do this. Without them the renderer keeps cached GPU copies and
// page tracking for memory the game has freed and reused, and its watcher counts go wrong.
extern "C" __declspec(dllexport) void RytyGpuUnmapMemory(u64 address, u64 size) {
    Core::RytyMemory::NoteMappingChange(address, size);
    if (auto* rasterizer = Core::Memory::Instance()->GetRasterizer()) {
        rasterizer->UnmapMemory(address, size);
    }
}

// Ryty calls this after any change to guest mappings or protections (after an unmap completes,
// for example), so lookups cached in between are not kept.
extern "C" __declspec(dllexport) void RytyGpuMappingChanged(u64 address, u64 size) {
    Core::RytyMemory::NoteMappingChange(address, size);
}

// Ryty calls this after the game changed the protection of guest memory (sceKernelMprotect),
// which replaced the protections the renderer's page tracking had set there.
extern "C" __declspec(dllexport) void RytyGpuProtectionChanged(u64 address, u64 size) {
    Core::RytyMemory::NoteMappingChange(address, size);
    if (auto* rasterizer = Core::Memory::Instance()->GetRasterizer()) {
        rasterizer->GetPageManager().ReapplyProtection(address, size);
    }
}

extern "C" __declspec(dllexport) void RytyGpuMapMemory(u64 address, u64 size) {
    Core::RytyMemory::NoteMappingChange(address, size);
    if (auto* rasterizer = Core::Memory::Instance()->GetRasterizer()) {
        rasterizer->MapMemory(address, size);
        rasterizer->RegisterMemory(address, size);
    }
}

// The renderer write-protects guest pages it caches on the GPU. Ryty's exception handler offers
// each guest write fault here first; true means the renderer owned the page, dropped its cached
// copy and unprotected it, so the write can be retried.
extern "C" __declspec(dllexport) int RytyGpuHandleWriteFault(void* exception_pointers) {
    auto* pointers = static_cast<EXCEPTION_POINTERS*>(exception_pointers);
    if (pointers == nullptr || pointers->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        pointers->ExceptionRecord->NumberParameters < 2) {
        return 0;
    }
    const auto address = reinterpret_cast<void*>(pointers->ExceptionRecord->ExceptionInformation[1]);
    if (Core::Memory::Instance()->GetRasterizer() == nullptr) {
        return Core::RytyMemory::InGuardedRead() ? 2 : 0;
    }
    // 2: a guarded read hit memory the game released or protected; its structured handler
    // takes it and the read falls back to the safe copy (Ryty then does not treat it as fatal).
    if (Core::RytyMemory::InGuardedRead() &&
        !Core::RytyMemory::TrackingMadeUnreadable(reinterpret_cast<VAddr>(address))) {
        return 2;
    }
    if (Core::Signals::Instance()->DispatchAccessViolation(pointers, address)) {
        return 1;
    }
    return Core::RytyMemory::InGuardedRead() ? 2 : 0;
}

// Ryty calls this before the kernel writes guest memory directly (a file read into a guest
// buffer), which would fail on a write-protected page instead of faulting.
extern "C" __declspec(dllexport) void RytyGpuInvalidateMemory(u64 address, u64 size) {
    if (auto* rasterizer = Core::Memory::Instance()->GetRasterizer()) {
        rasterizer->InvalidateMemory(address, size);
    }
}
