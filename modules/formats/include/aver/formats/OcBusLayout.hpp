#pragma once
// .ocbus -- an audio bus layout (the engine's bus layout form, which an importer can write). Text, one record per line:
//
//   OCBUS 1
//   BUS "Master" volume_db=0 mute=0 solo=0 bypass=0
//   BUS "Music" send="Master" volume_db=-6 mute=0 solo=0 bypass=0
//   EFFECT "AudioEffectReverb" enabled=1 sidechain="" room_size=0.3 wet=0.2
//
// An EFFECT belongs to the BUS above it and lists the effect's property names as name=value (any it does not know are ignored when the layout
// is applied). The first BUS is Master whatever it is called. Lines starting with '#' are comments. Names are quoted and may not contain
// a double quote.
#include "aver/audio/BusLayout.hpp"

#include <string>
#include <string_view>

namespace aver::fmt {

// False with the line number in *why on a malformed file; `out` then holds what was read before it.
bool parseOcBusLayout(std::string_view text, audio::BusLayout& out, std::string* why = nullptr);
std::string writeOcBusLayout(const audio::BusLayout& layout);
bool loadOcBusLayout(const std::string& path, audio::BusLayout& out, std::string* why = nullptr);

} // namespace aver::fmt
