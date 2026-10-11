// shadGT's Audio3d mixes the game's 3D audio objects and beds into AudioOut ports. In the Ryty
// build the ports belong to Ryty's libSceAudioOut, so the AudioOut calls forward to it by NID.

#include <windows.h>

#include "common/logging/log.h"
#include "core/libraries/audio/audioout.h"
#include "core/libraries/audio/audioout_error.h"

namespace {

template <typename T>
T RytyAudioOut(const char* nid) {
    HMODULE handle = GetModuleHandleA("libSceAudioOut.prx");
    auto* address = handle ? reinterpret_cast<void*>(GetProcAddress(handle, nid)) : nullptr;
    if (address == nullptr) {
        LOG_CRITICAL(Lib_AudioOut, "Ryty export {} not found in libSceAudioOut.prx", nid);
    }
    return reinterpret_cast<T>(address);
}

// Calls a guest-ABI function that Ryty's libSceAudioOut exports under the given NID.
#define RYTY_AUDIO_OUT(nid, ...)                                                                   \
    static const auto fn = RytyAudioOut<s32(PS4_SYSV_ABI*)(__VA_ARGS__)>(nid);                     \
    if (fn == nullptr) {                                                                           \
        return ORBIS_AUDIO_OUT_ERROR_NOT_INIT;                                                     \
    }                                                                                              \
    return fn

} // namespace

namespace Libraries::AudioOut {

s32 PS4_SYSV_ABI sceAudioOutInit() {
    RYTY_AUDIO_OUT("JfEPXVxhFqA")();
}

s32 PS4_SYSV_ABI sceAudioOutOpen(UserService::OrbisUserServiceUserId user_id,
                                 OrbisAudioOutPort port_type, s32 index, u32 length,
                                 u32 sample_rate, OrbisAudioOutParamExtendedInformation param_type) {
    RYTY_AUDIO_OUT("ekNvsT22rsY", s32, s32, s32, u32, u32, u32)
    (user_id, static_cast<s32>(port_type), index, length, sample_rate, param_type.Unpack());
}

s32 PS4_SYSV_ABI sceAudioOutOutput(s32 handle, void* ptr) {
    RYTY_AUDIO_OUT("QOQtbeDqsT4", s32, void*)(handle, ptr);
}

s32 PS4_SYSV_ABI sceAudioOutClose(s32 handle) {
    RYTY_AUDIO_OUT("s1--uE9mBFw", s32)(handle);
}

} // namespace Libraries::AudioOut
