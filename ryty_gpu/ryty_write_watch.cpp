// DIAG-067: which code writes a guest address, caught with a hardware data breakpoint. GT Sport's
// race summary finds its tessellation constants replaced by vertex data; this watches the
// constants of a draw after it was decoded and lists every write to them (time, thread, code
// address in GTSport.exe, value) for a few milliseconds.

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include <fmt/format.h>
#include "common/logging/log.h"

namespace {

struct Hit {
    std::int64_t ns;
    DWORD tid;
    std::uint64_t rip;
    std::uint64_t value;
};

std::atomic<bool> g_active{false};
std::atomic<std::uint64_t> g_address{0};
std::array<Hit, 64> g_hits{};
std::atomic<std::uint32_t> g_num_hits{0};
std::atomic<std::int64_t> g_start_ns{0};
PVOID g_handler = nullptr;

std::int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* pointers) {
    if (pointers->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    auto* context = pointers->ContextRecord;
    if ((context->Dr6 & 1) == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    context->Dr6 &= ~0xFull;
    if (g_active.load(std::memory_order_acquire)) {
        const std::uint64_t address = g_address.load(std::memory_order_relaxed);
        const std::uint32_t index = g_num_hits.fetch_add(1, std::memory_order_relaxed);
        if (index < g_hits.size()) {
            g_hits[index] = {NowNs(), GetCurrentThreadId(), context->Rip,
                             *reinterpret_cast<const volatile std::uint64_t*>(address)};
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Sets (or clears, with address 0) data breakpoint 0 on every other thread of the process.
void SetBreakpoint(std::uint64_t address) {
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) {
            continue;
        }
        HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                                   FALSE, entry.th32ThreadID);
        if (!thread) {
            continue;
        }
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &context)) {
                // Breakpoint 0: enabled locally, on writes, 8 bytes.
                context.Dr7 &= ~((0xFull << 16) | 3ull);
                if (address != 0) {
                    context.Dr0 = address;
                    context.Dr7 |= 1ull | (1ull << 16) | (3ull << 18);
                } else {
                    context.Dr0 = 0;
                }
                SetThreadContext(thread, &context);
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
}

std::string Describe(std::uint64_t rip) {
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(rip), &module) &&
        module) {
        GetModuleFileNameA(module, name, sizeof(name));
        std::string path{name};
        if (const auto slash = path.find_last_of("\\/"); slash != std::string::npos) {
            path = path.substr(slash + 1);
        }
        return fmt::format("{}+{:#x}", path, rip - reinterpret_cast<std::uint64_t>(module));
    }
    return fmt::format("{:#x}", rip);
}

std::string ThreadName(DWORD tid) {
    std::string out = std::to_string(tid);
    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!thread) {
        return out;
    }
    PWSTR description = nullptr;
    if (SUCCEEDED(GetThreadDescription(thread, &description)) && description && *description) {
        char narrow[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, description, -1, narrow, sizeof(narrow) - 1, nullptr,
                            nullptr);
        out += fmt::format(" ({})", narrow);
    }
    if (description) {
        LocalFree(description);
    }
    CloseHandle(thread);
    return out;
}

} // namespace

namespace Vulkan {
// Watches address for ms milliseconds from now and logs the writes, unless a watch is running.
void RytyWatchWrites(std::uint64_t address, std::uint32_t ms, std::string label) {
    bool expected = false;
    if (!g_active.compare_exchange_strong(expected, true)) {
        return;
    }
    if (!g_handler) {
        g_handler = AddVectoredExceptionHandler(1, OnException);
    }
    g_address.store(address & ~7ull);
    g_num_hits.store(0);
    g_start_ns.store(NowNs());
    const std::uint64_t value = *reinterpret_cast<const volatile std::uint64_t*>(address & ~7ull);
    std::thread([address = address & ~7ull, ms, label = std::move(label), value] {
        SetBreakpoint(address);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        SetBreakpoint(0);
        const std::uint32_t count = std::min<std::uint32_t>(g_num_hits.load(), g_hits.size());
        std::string line;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto& hit = g_hits[i];
            const auto has = [&](std::uint32_t word) {
                return std::uint32_t(hit.value) == word || std::uint32_t(hit.value >> 32) == word;
            };
            // The vertex stream found over GT Sport's frame data: (x, y, z, 0.932, 1.0105, 0.932).
            const bool vertex_like = has(0x3f815729) || has(0x3f6e90f7) || has(0x3f6ea9b5);
            line += fmt::format(" | +{:.2f} ms thread {} at {} wrote {:016x}{}",
                                (hit.ns - g_start_ns.load()) / 1e6, ThreadName(hit.tid),
                                Describe(hit.rip), hit.value, vertex_like ? " VERTEX-LIKE" : "");
        }
        LOG_WARNING(Render_Vulkan, "DIAG-067: {} ({:#x}, was {:016x}): {} writes in {} ms{}",
                    label, address, value, g_num_hits.load(), ms, line);
        g_active.store(false, std::memory_order_release);
    }).detach();
}
} // namespace Vulkan
