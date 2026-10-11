// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <xxhash.h>
#include "common/types.h"

#ifdef RYTY_GPU
namespace Core::RytyMemory {
u64 HashGuestMemory(VAddr address, u64 size);
}
#endif

namespace Core {

/// Hash of guest bytes the caches compare against their copies. In the Ryty build the game can
/// release pool memory (or make it inaccessible) while the caches still watch it; reading it
/// directly then crashed a GPU thread at the start of a race, so it is read safely there.
inline u64 HashGuestBytes(VAddr address, u64 size) {
#ifdef RYTY_GPU
    return RytyMemory::HashGuestMemory(address, size);
#else
    return XXH3_64bits(reinterpret_cast<const void*>(address), size);
#endif
}

} // namespace Core
