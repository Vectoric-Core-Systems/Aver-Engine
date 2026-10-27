#pragma once
// PlacementProps: a placed actor's per-instance [Editable] values (Godot's exported properties) travel as a
// `COMPONENT Props name=value ...` record on its placement. Loading hands them to the framework's per-entity
// store, which the scripting bridge writes into the actor's fields before OnBeginPlay; saving reads the store
// back. The format already round-trips the record untouched; this is the one place that knows its name.
#include "aver/formats/OcWorld.hpp"
#include "aver/framework/framework_abi.h"

#include <cctype>
#include <string_view>

namespace aver::game {

inline constexpr const char* kPropsComponent = "Props";

inline bool isPropsComponent(std::string_view type) {
    constexpr std::string_view k = "props";
    if (type.size() != k.size()) return false;
    for (std::size_t i = 0; i < k.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(type[i])) != k[i]) return false;
    return true;
}

// Hands the placement's Props record, if any, to `entity`'s property store. Returns how many values.
inline int applyPlacementProps(int32_t entity, const fmt::OcWorldPlacement& p) {
    int n = 0;
    for (const fmt::OcPlacedComponent& c : p.components) {
        if (!isPropsComponent(c.type)) continue;
        for (const auto& [k, v] : c.kv) n += aver_fw_props_set(entity, k.c_str(), v.c_str());
    }
    return n;
}

// Replaces the placement's Props record with `entity`'s current store; none when the store is empty.
inline void capturePlacementProps(int32_t entity, fmt::OcWorldPlacement& p) {
    std::erase_if(p.components, [](const fmt::OcPlacedComponent& c) { return isPropsComponent(c.type); });
    const int32_t n = aver_fw_props_count(entity);
    if (n <= 0) return;
    fmt::OcPlacedComponent c;
    c.type = kPropsComponent;
    for (int32_t i = 0; i < n; ++i) c.kv.emplace_back(aver_fw_props_name(entity, i), aver_fw_props_value(entity, i));
    p.components.push_back(std::move(c));
}

}  // namespace aver::game
