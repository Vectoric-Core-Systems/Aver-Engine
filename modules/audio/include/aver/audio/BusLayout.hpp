#pragma once
// A bus layout as plain data: what an imported bus layout converts to, what BusGraph applies and exports, and what .ocbus
// (aver/formats/OcBusLayout.hpp) reads and writes. Effects are named by their class names ("AudioEffectReverb" ...), parameters by property name.
#include "aver/core/Types.hpp"

#include <string>
#include <utility>
#include <vector>

namespace aver::audio {

struct BusLayoutEffect {
    std::string className;                                  // "AudioEffectReverb"
    bool enabled = true;
    std::string sidechain;                                  // a compressor's detector bus
    std::vector<std::pair<std::string, f32>> params;        // property names
};

struct BusLayoutBus {
    std::string name;
    std::string send = "Master";
    f32 volumeDb = 0.0f;
    bool mute = false, solo = false, bypass = false;
    std::vector<BusLayoutEffect> effects;
};

struct BusLayout {
    std::vector<BusLayoutBus> buses;                        // [0] is Master
};

} // namespace aver::audio
