// Shared by every shadGT library built for Ryty: the first call into any export initializes the
// library (RytyLibraryInitialize, defined by each library's entry file), then every export jumps
// straight to shadGT's implementation.

#include <cstdlib>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "core/libraries/libs.h"
#include "core/loader/symbols_resolver.h"
#include "emulator.h"
#include "ryty_gpu/ryty_library.h"

extern "C" {
extern void* g_ryty_export_targets[];
extern const char* const g_ryty_export_nids[];
extern const unsigned g_ryty_export_count;
volatile unsigned char g_ryty_library_ready = 0;
void RytyLibraryEnsureInit();
}

namespace {

std::unordered_map<std::string, u64>& Registered() {
    static std::unordered_map<std::string, u64> symbols;
    return symbols;
}

// Exports shadGT does not register still need a target; they report themselves once and return 0.
constexpr unsigned MaxExports = 1024;
std::atomic<bool> g_missing_reported[MaxExports];
template <unsigned Index>
s64 PS4_SYSV_ABI MissingExport() {
    if (!g_missing_reported[Index].exchange(true)) {
        std::fprintf(stderr, "[ryty] unregistered export %s\n", g_ryty_export_nids[Index]);
    }
    return 0;
}

template <unsigned... I>
void* MissingTarget(unsigned index, std::integer_sequence<unsigned, I...>) {
    static void* const table[] = {reinterpret_cast<void*>(&MissingExport<I>)...};
    return table[index];
}

std::once_flag g_init_once;

} // namespace

namespace Ryty {

void InitializeCommon(const char* log_name) {
    Common::Log::Setup(log_name);
    EmulatorSettings.Load("CUSA03220");
    // Setup logs every class at trace level until a filter is applied; Ngs2 alone then writes
    // hundreds of megabytes a minute. RYTY_LOG_FILTER overrides the configured filter.
    const char* filter = std::getenv("RYTY_LOG_FILTER");
    std::string configured = EmulatorSettings.GetLogFilter();
    Common::Log::UpdateLogLevels(filter != nullptr ? std::string_view{filter}
                                 : configured.empty() ? std::string_view{"*:Info"}
                                                      : std::string_view{configured});
    Common::Log::UpdateLogFlushLevel("warning");
    static Core::Emulator game_info_owner; // fills Common::ElfInfo
}

void BindExports() {
    const auto& symbols = Registered();
    unsigned missing = 0;
    for (unsigned i = 0; i < g_ryty_export_count; ++i) {
        const auto it = symbols.find(g_ryty_export_nids[i]);
        if (it != symbols.end()) {
            g_ryty_export_targets[i] = reinterpret_cast<void*>(it->second);
        } else if (i < MaxExports) {
            g_ryty_export_targets[i] =
                MissingTarget(i, std::make_integer_sequence<unsigned, MaxExports>{});
            ++missing;
        } else {
            g_ryty_export_targets[i] = reinterpret_cast<void*>(&MissingExport<MaxExports - 1>);
            ++missing;
        }
    }
    LOG_INFO(Loader, "Ryty: {} exports bound, {} without a registration",
             g_ryty_export_count - missing, missing);
}

} // namespace Ryty

void LinkSymbolImpl(Core::Loader::SymbolsResolver*, char const* nid, char const*, u16, char const*,
                    u64 symbol, Core::Loader::SymbolType) {
    Registered().emplace(nid, symbol);
}

extern "C" void RytyLibraryEnsureInit() {
    std::call_once(g_init_once, [] {
        Ryty::RytyLibraryInitialize();
        __atomic_store_n(&g_ryty_library_ready, 1, __ATOMIC_RELEASE);
    });
}

// Saves the System V argument registers around the (Microsoft ABI) initializer, then continues to
// the export's target. r11 holds the address of the target slot.
__asm__(".att_syntax prefix\n"
        ".text\n"
        ".globl ryty_export_dispatch\n"
        "ryty_export_dispatch:\n"
        "    cmpb $0, g_ryty_library_ready(%rip)\n"
        "    jne 1f\n"
        "    pushq %rdi\n"
        "    pushq %rsi\n"
        "    pushq %rdx\n"
        "    pushq %rcx\n"
        "    pushq %r8\n"
        "    pushq %r9\n"
        "    pushq %r11\n"
        "    pushq %rax\n"
        "    subq $168, %rsp\n"
        "    movdqu %xmm0, 32(%rsp)\n"
        "    movdqu %xmm1, 48(%rsp)\n"
        "    movdqu %xmm2, 64(%rsp)\n"
        "    movdqu %xmm3, 80(%rsp)\n"
        "    movdqu %xmm4, 96(%rsp)\n"
        "    movdqu %xmm5, 112(%rsp)\n"
        "    movdqu %xmm6, 128(%rsp)\n"
        "    movdqu %xmm7, 144(%rsp)\n"
        "    call RytyLibraryEnsureInit\n"
        "    movdqu 32(%rsp), %xmm0\n"
        "    movdqu 48(%rsp), %xmm1\n"
        "    movdqu 64(%rsp), %xmm2\n"
        "    movdqu 80(%rsp), %xmm3\n"
        "    movdqu 96(%rsp), %xmm4\n"
        "    movdqu 112(%rsp), %xmm5\n"
        "    movdqu 128(%rsp), %xmm6\n"
        "    movdqu 144(%rsp), %xmm7\n"
        "    addq $168, %rsp\n"
        "    popq %rax\n"
        "    popq %r11\n"
        "    popq %r9\n"
        "    popq %r8\n"
        "    popq %rcx\n"
        "    popq %rdx\n"
        "    popq %rsi\n"
        "    popq %rdi\n"
        "1:\n"
        "    jmpq *(%r11)\n");
