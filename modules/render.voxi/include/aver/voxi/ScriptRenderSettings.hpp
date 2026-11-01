// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
#pragma once
#include "aver/voxi/Voxi.hpp"

#include <algorithm>
#include <cmath>

// SCRIPT RENDER SETTINGS ONTO A voxi::Settings, SHARED BY BOTH HOSTS (framework_render_abi.h, P17). A script publishes render keys; each frame, before the scene is
// built, the host hands the live Settings to applyScriptRenderSettings, which writes every overridden key into it and puts the project's value back for a key that was
// released. `Api` supplies the reads (FrameworkRenderApi below is the real one; a test passes a fake), so this header does not link the framework: a TU that can
// see framework_render_abi.h defines AVER_SCRIPTRENDER_HAS_ABI before including it.
//
// Only the keys that land on a Settings field are here. The render scale, the LOD threshold and the occlusion switch are host state and are read by the hosts
// through the same Api; SHARPNESS and TAA are stored for scripts and not applied (the upscaler is chosen when the project starts).
namespace aver::voxi {

// The framework's AVER_FW_RENDER_* numbers, restated so the mapping reads without the header (the framework test compares them).
inline constexpr int kScriptRenderScale = 1, kScriptRenderSharpness = 2, kScriptRenderTaa = 3, kScriptRenderMsaa = 4, kScriptRenderLodThreshold = 5,
                     kScriptRenderOcclusion = 6, kScriptRenderShadowCascades = 7, kScriptRenderShadowSplit = 8, kScriptRenderShadowDistance = 9,
                     kScriptRenderShadowFilter = 10, kScriptRenderVolFogAniso = 11, kScriptRenderVolFogLength = 12, kScriptRenderVolFogTemporal = 13,
                     kScriptRenderVolFogIntensity = 14, kScriptRenderKeyCount = 15;

#ifdef AVER_SCRIPTRENDER_HAS_ABI
static_assert(kScriptRenderScale == AVER_FW_RENDER_SCALE && kScriptRenderTaa == AVER_FW_RENDER_TAA && kScriptRenderMsaa == AVER_FW_RENDER_MSAA &&
                  kScriptRenderShadowCascades == AVER_FW_RENDER_SHADOW_CASCADES && kScriptRenderShadowFilter == AVER_FW_RENDER_SHADOW_FILTER &&
                  kScriptRenderVolFogIntensity == AVER_FW_RENDER_VOLFOG_INTENSITY && kScriptRenderKeyCount == AVER_FW_RENDER_KEY_COUNT,
              "ScriptRenderSettings.hpp's keys drifted from framework_render_abi.h");

struct FrameworkRenderApi {
    static bool get(int key, float* out) { return aver_fw_render_get(key, out) != 0; }
    static int revision() { return aver_fw_render_revision(); }
};
#endif

// What the project had before a script took a key, so releasing the key can restore it. Held by the host across frames.
struct ScriptRenderState {
    bool taken[kScriptRenderKeyCount] = {};
    float saved[kScriptRenderKeyCount] = {};
    int revision = -1;   // the last ABI revision applied; -1 forces the first pass
};

namespace detail {

// One key: write `value` into `field` when the script holds the key, restore the saved value when it let go. `read`/`write` convert the field to and from the
// float the ABI speaks. Returns true when the field changed.
template <class Api, class Read, class Write>
inline bool applyScriptKey(int key, ScriptRenderState& st, Read&& read, Write&& write) {
    float v = 0.0f;
    const bool on = Api::get(key, &v);
    if (on) {
        if (!st.taken[key]) { st.taken[key] = true; st.saved[key] = read(); }
        if (read() == v) return false;
        write(v);
        return true;
    }
    if (st.taken[key]) {
        st.taken[key] = false;
        write(st.saved[key]);
        return true;
    }
    return false;
}

}  // namespace detail

// Applies every script key that lands on Settings. True when `s` changed (the caller then calls Renderer::setSettings). A cheap early-out when the ABI
// revision has not moved since the last call.
template <class Api>
inline bool applyScriptRenderSettings(Settings& s, ScriptRenderState& st) {
    const int rev = Api::revision();
    if (rev == st.revision) return false;
    st.revision = rev;
    bool changed = false;
    using detail::applyScriptKey;
    changed |= applyScriptKey<Api>(kScriptRenderMsaa, st, [&] { return static_cast<float>(static_cast<u32>(s.msaa)); },
                                   [&](float v) { s.msaa = static_cast<Msaa>(static_cast<u32>(v)); });
    changed |= applyScriptKey<Api>(kScriptRenderShadowCascades, st, [&] { return static_cast<float>(s.shadowCascades); },
                                   [&](float v) { s.shadowCascades = static_cast<u32>(std::clamp(v, 1.0f, 4.0f)); });
    changed |= applyScriptKey<Api>(kScriptRenderShadowSplit, st, [&] { return s.shadowSplitLambda; }, [&](float v) { s.shadowSplitLambda = v; });
    changed |= applyScriptKey<Api>(kScriptRenderShadowDistance, st, [&] { return s.shadowMaxDistance; }, [&](float v) { s.shadowMaxDistance = v; });
    changed |= applyScriptKey<Api>(kScriptRenderShadowFilter, st, [&] { return s.shadowFilterScale; }, [&](float v) { s.shadowFilterScale = v; });
    changed |= applyScriptKey<Api>(kScriptRenderVolFogAniso, st, [&] { return s.volumetricFogAnisotropy; }, [&](float v) { s.volumetricFogAnisotropy = v; });
    changed |= applyScriptKey<Api>(kScriptRenderVolFogLength, st, [&] { return s.volumetricFogMaxDistance; }, [&](float v) { s.volumetricFogMaxDistance = v; });
    changed |= applyScriptKey<Api>(kScriptRenderVolFogTemporal, st, [&] { return s.volumetricFogTemporal; }, [&](float v) { s.volumetricFogTemporal = v; });
    changed |= applyScriptKey<Api>(kScriptRenderVolFogIntensity, st, [&] { return s.volumetricFogIntensity; }, [&](float v) { s.volumetricFogIntensity = v; });
    return changed;
}

}  // namespace aver::voxi
