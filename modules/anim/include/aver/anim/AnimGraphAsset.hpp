// .ocblend (blend space) and .ocasm (animation state machine) assets.
//
// Text JSON with a format tag and a version, so a hand edit or a diff is readable and a newer file
// is refused by tag rather than misread. Parsing does NOT call valid(): an asset half-authored in the
// editor must still round-trip, so the runtime loaders validate before they bind. Both round-trip exactly: floats are written with nine
// significant digits, which is lossless for f32. See docs/ANIM_BLEND_STATE.md for the schema.
#pragma once

#include "aver/anim/AnimStateMachine.hpp"

#include <string>
#include <string_view>

namespace aver::anim {

inline constexpr int kBlendSpaceVersion = 1;
inline constexpr int kStateMachineVersion = 1;

std::string writeBlendSpace(const BlendSpaceAsset& a);
bool parseBlendSpace(std::string_view json, BlendSpaceAsset& out, std::string* why = nullptr);
bool saveBlendSpace(const std::string& path, const BlendSpaceAsset& a, std::string* why = nullptr);
bool loadBlendSpace(const std::string& path, BlendSpaceAsset& out, std::string* why = nullptr);

std::string writeStateMachine(const AnimStateMachineAsset& a);
bool parseStateMachine(std::string_view json, AnimStateMachineAsset& out, std::string* why = nullptr);
bool saveStateMachine(const std::string& path, const AnimStateMachineAsset& a, std::string* why = nullptr);
bool loadStateMachine(const std::string& path, AnimStateMachineAsset& out, std::string* why = nullptr);

} // namespace aver::anim
