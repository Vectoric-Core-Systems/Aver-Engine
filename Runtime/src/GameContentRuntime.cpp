#include "GameContentInternal.hpp"

namespace aver::game {

#if AVER_MODULE_SCENE

// G4 R7: script-built meshes (framework_rtmesh_abi.h). They go through uploadMesh like a streamed mesh (lazyLoading_ makes the acquired hook fire, which is what
// builds the ray-traced geometry for a mesh that appears after load), and an update replaces the mesh: the old handles wait in the release queue.
u32 GameContent::syncRuntimeMeshes(rhi::IDevice& device) {
    u32 uploaded = 0;
#if AVER_MODULE_FRAMEWORK
    aver_fw_rtmesh_pump();
    u64 removed[32];
    while (const i32 n = aver_fw_rtmesh_take_removed(removed, 32)) {
        for (i32 i = 0; i < n; ++i)
            if (sceneMeshes_.count(removed[i])) unloadToPending(removed[i]);
    }
    i32 ids[32];
    while (const i32 n = aver_fw_rtmesh_take_dirty(ids, 32)) {
        for (i32 k = 0; k < n; ++k) {
            const u64 oid = aver_fw_rtmesh_object_id(ids[k]);
            if (oid == 0) continue;
            AverFwRtMeshView v{};
            if (!aver_fw_rtmesh_view(ids[k], &v)) {   // emptied: nothing to draw
                if (sceneMeshes_.count(oid)) unloadToPending(oid);
                continue;
            }
            fmt::OcMeshData md;
            const usize vc = static_cast<usize>(v.vertexCount);
            md.positions.assign(v.pos, v.pos + vc * 3);
            md.normals.assign(v.nrm, v.nrm + vc * 3);
            md.uvs.assign(v.uv, v.uv + vc * 2);
            if (v.col) md.colors.assign(v.col, v.col + vc * 4);
            if (v.uv1) md.uvs1.assign(v.uv1, v.uv1 + vc * 2);
            if (v.tan) md.tangents.assign(v.tan, v.tan + vc * 4);
            md.indices.assign(v.idx, v.idx + v.indexCount);
            for (i32 s = 0; s < v.submeshCount; ++s) {
                const std::string mat = v.subMaterial[s] ? v.subMaterial[s] : "M_Default";
                u32 slot = 0;
                for (; slot < md.materialSlots.size(); ++slot) if (md.materialSlots[slot] == mat) break;
                if (slot == md.materialSlots.size()) md.materialSlots.push_back(mat);
                fmt::OcMeshSubmesh sm;
                sm.name = mat;
                sm.materialSlot = slot;
                sm.indexStart = static_cast<u32>(v.subStart[s]);
                sm.indexCount = static_cast<u32>(v.subCount[s]);
                sm.baseVertex = 0;
                sm.vertexCount = static_cast<u32>(vc);
                md.submeshes.push_back(std::move(sm));
            }
            md.computeBounds();
            if (!md.valid()) { AVER_WARN("[RuntimeMesh] id {} is not a valid mesh; not uploaded", ids[k]); continue; }
            char key[128] = {};
            aver_fw_rtmesh_key(ids[k], key, sizeof key);
            const std::string rel = std::string(AVER_FW_RTMESH_PATH_PREFIX) + key;
            if (sceneMeshes_.count(oid)) unloadToPending(oid);
            lazyLoading_ = true;
            if (uploadMesh(device, oid, md, rel)) ++uploaded;
            lazyLoading_ = false;
        }
    }
#else
    (void)device;
#endif
    return uploaded;
}

// E8: script-built textures (framework_rtex_abi.h). A changed texture is uploaded as a NEW GPU texture (with its mip chain when it asked for one) and the old one
// retires after kRtexRetireFrames, longer than any frame in flight and than the material binding sets still pointing at it. Materials name a texture by the
// runtime-texture name "rtex.<key>" (their slot path is "@rt:rtex.<key>").
namespace {
constexpr u64 kRtexRetireFrames = 6;

#if AVER_MODULE_FRAMEWORK && AVER_MODULE_PBR
// Rewrites (on the next MaterialSystem::update) every material that names the runtime texture `path` in a slot.
void touchMaterialsNaming(const std::string& path) {
    pbr::MaterialLibrary& lib = pbr::MaterialLibrary::get();
    std::vector<pbr::MaterialHandle> hit;
    for (u32 i = 0, n = lib.count(); i < n; ++i) {
        const pbr::MaterialHandle h = lib.at(i);
        const pbr::MaterialDesc* d = h ? lib.desc(h) : nullptr;
        if (!d) continue;
        for (u32 s = 0; s < pbr::kTextureSlotCount; ++s)
            if (d->textures[s].path == path) { hit.push_back(h); break; }
    }
    for (pbr::MaterialHandle h : hit) lib.touch(h);
}
#endif
}  // namespace

u32 GameContent::syncRuntimeTextures(rhi::IDevice& device, pbr::MaterialSystem* materials) {
    u32 uploaded = 0;
#if AVER_MODULE_FRAMEWORK
    ++rtexFrame_;
    if (aver_fw_rtex_count() == 0 && rtexGpu_.empty() && rtexRetired_.empty()) return 0;   // the ordinary frame: nothing registered, nothing held
    rhi::IResourceFactory* res = device.resources();
    if (!res) return 0;

    const auto retire = [&](rhi::TextureHandle t) { if (t) rtexRetired_.push_back({t, rtexFrame_}); };
    const auto touch = [&](const std::string& name) {
#  if AVER_MODULE_PBR
        touchMaterialsNaming("@rt:" + name);
#  else
        (void)name;
#  endif
    };

    i32 ids[32];
    while (const i32 n = aver_fw_rtex_take_removed(ids, 32)) {
        for (i32 k = 0; k < n; ++k) {
            const auto it = rtexGpu_.find(ids[k]);
            if (it == rtexGpu_.end()) continue;
#  if AVER_MODULE_PBR
            if (materials) materials->clearRuntimeTexture(it->second.name);
#  endif
            retire(it->second.tex);
            touch(it->second.name);
            rtexGpu_.erase(it);
        }
    }
    while (const i32 n = aver_fw_rtex_take_dirty(ids, 32)) {
        for (i32 k = 0; k < n; ++k) {
            AverFwRtexView v{};
            if (!aver_fw_rtex_view(ids[k], &v) || v.width < 1 || v.height < 1) continue;
            const bool srgb = v.format == AVER_FW_RTEX_FORMAT_RGBA8_SRGB;
            rhi::Format fmtGpu = rhi::Format::RGBA8Unorm;
            switch (v.format) {
                case AVER_FW_RTEX_FORMAT_RGBA8: fmtGpu = rhi::Format::RGBA8Unorm; break;
                case AVER_FW_RTEX_FORMAT_RGBA8_SRGB: fmtGpu = rhi::Format::RGBA8UnormSrgb; break;
                case AVER_FW_RTEX_FORMAT_R8: fmtGpu = rhi::Format::R8Unorm; break;
                case AVER_FW_RTEX_FORMAT_RGBA16F: fmtGpu = rhi::Format::RGBA16F; break;
                default: continue;
            }
            // The levels to upload: level 0 is the script's bytes; a requested chain is built in RGBA8 (R8 rides in all three colour channels) by the same
            // generator textures from files use, then R8 is narrowed back.
            std::vector<std::vector<u8>> owned;
            std::vector<const void*> levelPtrs;
            if (v.mips && v.format != AVER_FW_RTEX_FORMAT_RGBA16F) {
                fmt::TextureData td;
                td.width = static_cast<u32>(v.width);
                td.height = static_cast<u32>(v.height);
                td.srgb = srgb;
                ImageData base;
                base.width = td.width; base.height = td.height; base.channels = v.format == AVER_FW_RTEX_FORMAT_R8 ? 1u : 4u; base.srgb = srgb;
                const usize px = static_cast<usize>(v.width) * static_cast<usize>(v.height);
                base.pixels.resize(px * 4);
                if (v.format == AVER_FW_RTEX_FORMAT_R8) {
                    for (usize i = 0; i < px; ++i) { const u8 r = v.pixels[i]; base.pixels[i * 4 + 0] = base.pixels[i * 4 + 1] = base.pixels[i * 4 + 2] = r; base.pixels[i * 4 + 3] = 255; }
                } else {
                    std::memcpy(base.pixels.data(), v.pixels, px * 4);
                }
                td.levels.push_back(std::move(base));
                fmt::generateMipChain(td, /*normalMap=*/false);
                owned.reserve(td.levels.size());
                for (const ImageData& lvl : td.levels) {
                    if (v.format == AVER_FW_RTEX_FORMAT_R8) {
                        std::vector<u8> one(static_cast<usize>(lvl.width) * lvl.height);
                        for (usize i = 0; i < one.size(); ++i) one[i] = lvl.pixels[i * 4];
                        owned.push_back(std::move(one));
                    } else {
                        owned.push_back(lvl.pixels);
                    }
                }
                for (const auto& lvl : owned) levelPtrs.push_back(lvl.data());
            } else {
                levelPtrs.push_back(v.pixels);
            }
            rhi::TextureDesc d;
            d.width = static_cast<u32>(v.width);
            d.height = static_cast<u32>(v.height);
            d.mips = static_cast<u32>(levelPtrs.size());
            d.format = fmtGpu;
            d.initialState = rhi::ResourceState::ShaderResource;
            d.initialData = levelPtrs.data();
            d.initialDataCount = static_cast<u32>(levelPtrs.size());
            d.debugName = "runtime texture";
            const rhi::TextureHandle t = res->createTexture(d);
            if (!t) { AVER_WARN("[RuntimeTexture] '{}' ({}x{}) was not uploaded: the device refused the texture", v.key, v.width, v.height); continue; }
            RtexGpu& rec = rtexGpu_[ids[k]];
            const bool first = rec.name.empty();
            if (first) rec.name = std::string("rtex.") + v.key;
            retire(rec.tex);
            rec.tex = t;
#  if AVER_MODULE_PBR
            if (materials) materials->setRuntimeTexture(rec.name, t);
#  endif
            touch(rec.name);
            aver_fw_rtex_host_set_gpu_handle(ids[k], static_cast<u64>(t));
            ++uploaded;
        }
    }
    // Destroy what has waited long enough.
    usize w = 0;
    for (usize i = 0; i < rtexRetired_.size(); ++i) {
        if (rtexFrame_ - rtexRetired_[i].frame >= kRtexRetireFrames) res->destroyTexture(rtexRetired_[i].tex);
        else rtexRetired_[w++] = rtexRetired_[i];
    }
    rtexRetired_.resize(w);
#  if !AVER_MODULE_PBR
    (void)materials;
#  endif
#else
    (void)device; (void)materials;
#endif
    return uploaded;
}

#endif // AVER_MODULE_SCENE

} // namespace aver::game
