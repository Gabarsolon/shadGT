// FIX-074: protects the game code's red zones from Windows exception dispatch.
//
// PS4 code follows the System V ABI, where a function may keep data in the 128 bytes below its
// stack pointer (the red zone) without moving the stack pointer. On Windows an exception frame is
// written right below the stack pointer, so when such a function writes memory the renderer has
// write-protected (to notice CPU changes to GPU data), the fault overwrites that data. GT Sport's
// photo conversion keeps its row stride there; the fault zeroes it and every row of a strip
// repeats the first one (the striped Brand Central and Scapes photos).
//
// Standalone shadGT solves this with its static red-zone patching (core/cpu_patches.cpp): every
// memory access made while red-zone data is live moves the stack pointer below the red zone for
// that one instruction. This applies the same patching to the converted executable's code before
// any game thread starts (from DllMain, while only the loader's threads exist). The function list
// comes from the game's own EH frame header, which Ryty keeps in the executable and records in
// its .ehmeta section.

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "common/types.h"
#include "core/cpu_patches.h"
#include "core/loader/dwarf.h"

namespace {

struct Section {
    u8* start = nullptr;
    u64 size = 0;
};

Section FindSection(u8* base, const char* name) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char section_name[9] = {};
        std::memcpy(section_name, sections[i].Name, 8);
        if (std::strcmp(section_name, name) == 0) {
            return {base + sections[i].VirtualAddress, sections[i].Misc.VirtualSize};
        }
    }
    return {};
}

// Reserves executable memory for the trampolines within reach of a rel32 jump from the code.
u8* AllocateTrampolines(u8* image_base, u64 image_size, u64 size) {
    constexpr u64 Step = 0x10000;
    const u64 first = (reinterpret_cast<u64>(image_base) + image_size + Step - 1) & ~(Step - 1);
    for (u64 address = first; address < first + 0x40000000; address += Step) {
        if (void* memory = VirtualAlloc(reinterpret_cast<void*>(address), size,
                                        MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) {
            return static_cast<u8*>(memory);
        }
    }
    return nullptr;
}

} // namespace

void RytyProtectGuestRedZones() {
    if (const char* env = std::getenv("RYTY_RED_ZONE_PATCH"); env && env[0] == '0') {
        std::fprintf(stderr, "[ryty_gpu] FIX-074: red-zone patching is off (RYTY_RED_ZONE_PATCH=0)\n");
        return;
    }
    auto* base = reinterpret_cast<u8*>(GetModuleHandleA(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const Section code = FindSection(base, ".elf0");
    const Section ehmeta = FindSection(base, ".ehmeta");
    if (!code.start || !ehmeta.start || ehmeta.size < 4) {
        std::fprintf(stderr, "[ryty_gpu] FIX-074: not a Ryty executable with an EH frame header\n");
        return;
    }
    u32 eh_hdr_rva = 0;
    std::memcpy(&eh_hdr_rva, ehmeta.start, sizeof(eh_hdr_rva));
    const auto eh_hdr = reinterpret_cast<uintptr_t>(base + eh_hdr_rva);
    const auto code_end = reinterpret_cast<uintptr_t>(code.start + code.size);
    if (eh_hdr_rva == 0 || eh_hdr < reinterpret_cast<uintptr_t>(code.start) || eh_hdr >= code_end) {
        std::fprintf(stderr, "[ryty_gpu] FIX-074: EH frame header %#x is outside the code\n",
                     eh_hdr_rva);
        return;
    }

    const DWORD start_ms = GetTickCount();
    Dwarf::EHHeaderInfo info{};
    std::vector<uintptr_t> function_starts;
    if (!Dwarf::DecodeEHHdr(eh_hdr, code_end, info) ||
        !Dwarf::DecodeEHHdrTable(info, code_end, function_starts) || function_starts.empty()) {
        std::fprintf(stderr, "[ryty_gpu] FIX-074: could not read the function list\n");
        return;
    }

    constexpr u64 TrampolineSize = 64ull << 20;
    u8* trampolines = AllocateTrampolines(base, nt->OptionalHeader.SizeOfImage, TrampolineSize);
    if (!trampolines) {
        std::fprintf(stderr, "[ryty_gpu] FIX-074: no memory for trampolines near the code\n");
        return;
    }
    DWORD old_protect = 0;
    if (!VirtualProtect(code.start, code.size, PAGE_EXECUTE_READWRITE, &old_protect)) {
        std::fprintf(stderr, "[ryty_gpu] FIX-074: could not make the code writable (%lu)\n",
                     GetLastError());
        return;
    }
    Core::RegisterPatchModule(code.start, code.size, trampolines, TrampolineSize);
    const auto result = Core::PatchRedZoneMemoryInstructions(
        reinterpret_cast<u64>(code.start), code.size, function_starts);
    DWORD ignored = 0;
    VirtualProtect(code.start, code.size, old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), code.start, code.size);
    std::fprintf(stderr,
                 "[ryty_gpu] FIX-074: red-zone patching in %lu ms: %llu functions, %llu use the red "
                 "zone, %llu/%llu memory instructions protected (%llu stack-dependent, %llu "
                 "control-flow, %llu unrelocatable)\n",
                 GetTickCount() - start_ms, static_cast<unsigned long long>(result.function_count),
                 static_cast<unsigned long long>(result.red_zone_function_count),
                 static_cast<unsigned long long>(result.patched_memory_instruction_count),
                 static_cast<unsigned long long>(result.memory_instruction_count),
                 static_cast<unsigned long long>(result.stack_dependent_memory_instruction_count),
                 static_cast<unsigned long long>(result.control_flow_memory_instruction_count),
                 static_cast<unsigned long long>(result.unrelocatable_memory_instruction_count));
    std::fflush(stderr);
}
