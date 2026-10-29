#pragma once
// A bus layout as plain data: what a Godot AudioBusLayout converts to, what BusGraph applies and exports, and what .ocbus
// (aver/formats/OcBusLayout.hpp) reads and writes. Effects are named by Godot's class names and parameters by Godot's property names.
#include "aver/core/Types.hpp"

#include <string>
#include <utility>
#include <vector>

namespace aver::audio {

struct BusLayoutEffect {
    std::string className;                                  // "AudioEffectReverb"
    bool enabled = true;
    std::string sidechain;                                  // a compressor's detector bus
    std::vector<std::pair<std::string, f32>> params;        // Godot property names
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
