#pragma once
// GameContent.cpp and its per-feature parts share these includes and the collision-build seams.
#include "aver/game/GameContent.hpp"
#include "aver/core/Types.hpp"   // AVER_PATH_SEP

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/HitchMarks.hpp"
#include "aver/core/SurfaceId.hpp"

#include <algorithm>
#include <unordered_set>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <future>
#include <limits>
#include <mutex>
#include <system_error>
#include <thread>

// Always included: AssetType/assetTypeFromPath are used by both loadProjectMeshes (scene-guarded)
// and loadProjectParticleEffects (particles-guarded); scoping to either guard breaks the other.
#include "aver/assets/AssetId.hpp"
#if AVER_MODULE_FRAMEWORK
#include "aver/framework/framework_customenv_abi.h"   // a particles effect's custom program is registered when the effect loads
#include "aver/framework/framework_rtex_abi.h"        // syncRuntimeTextures
#include "aver/formats/Texture.hpp"                   // ...and the mip chain of a script-built texture
#endif
#include "aver/rhi/PrimitiveMeshes.hpp"
#include "aver/platform/FileSystem.hpp"   // toNativeSeparators

#if AVER_MODULE_PBR
#  include "aver/assets/TextureAtlas.hpp"
#  include "aver/assets/TextureUpload.hpp"
#  include "aver/formats/OcGraph.hpp"
#  include "aver/formats/OcMat.hpp"
#  include "aver/formats/MaterialInstance.hpp"
#  include "aver/pbr/MaterialGraphRegistry.hpp"
#endif

#if AVER_MODULE_SCENE
#  include "aver/anim/AnimSystem.hpp"
#  include "aver/formats/OcMesh.hpp"
#  include "aver/scene/scene_abi.h"
#  if AVER_MODULE_FRAMEWORK
#    include "aver/framework/framework_rtmesh_abi.h"   // G4 R7: script-built meshes
#  endif
#  if AVER_MODULE_PHYSICS
#    include "aver/physics/physics_abi.h"
#    include "aver/physics/physics_shape_builder_abi.h"   // aver_phys_shape_mesh: a collision shape with a surface palette
#    include "aver/world/PhysicsShapeCache.hpp"
#  endif
#  include "GameMath.hpp"
#  if AVER_MODULE_TRIFACTOR
#    include "aver/trifactor/ClusterAdapt.hpp"
#  endif
// Vendored meshoptimizer include, exposed by third_party/meshoptimizer CMakeLists.txt.
#  include <meshoptimizer.h>
#endif

#if AVER_MODULE_PARTICLES
#  include "aver/formats/OcParticle.hpp"
#  include "aver/particles/ParticleEffectLibrary.hpp"
#  include "aver/particles/ParticleRenderer.hpp"   // ParticleMeshData
#endif

namespace aver::game::content_detail {

#if AVER_MODULE_PBR
// Slot decides colour space: normal maps sRGB-decoded give subtle shading bugs. Layer1 slots too:
// Layer1BaseColor is sRGB, Layer1Normal is NormalMap.
inline assets::TextureUsage textureUsageFor(pbr::TextureSlot slot) {
    switch (slot) {
        case pbr::TextureSlot::BaseColor:
        case pbr::TextureSlot::Layer1BaseColor:
        case pbr::TextureSlot::Emissive:      return assets::TextureUsage::Colour;
        case pbr::TextureSlot::Normal:
        case pbr::TextureSlot::Layer1Normal:  return assets::TextureUsage::NormalMap;
        default:                              return assets::TextureUsage::Data;
    }
}
#endif

#if AVER_MODULE_SCENE
// What a material slot says NOW: does it collide, and which physical surface does it carry (0 = none). An empty function is
// "not asked" (collides, no surface). The build uses it to filter and group; a cache read uses it to decide whether the entry
// still describes the mesh, since the answers live in .ocmat files the cache key does not cover.
struct SlotQuery {
    std::function<bool(const std::string&)> collides;
    std::function<i32(const std::string&)> surface;
};

std::string collisionCachePathFor(const std::string& projectDir, const std::string& meshPath,
                                  u64 fileSize, i64 mtimeTicks);
u64 shapeSourceKey(const std::string& meshPath, u64 fileSize, i64 mtimeTicks, const GameContent::CollisionMesh& m);
bool readCollisionCacheFile(const std::string& cachePath, const std::string& meshPath, u64 fileSize,
                            i64 mtimeTicks, GameContent::CollisionMesh& out, const SlotQuery& slots);
bool writeCollisionCacheFile(const std::string& cachePath, const std::string& meshPath, u64 fileSize,
                             i64 mtimeTicks, const GameContent::CollisionMesh& mesh, std::string* why);
std::unique_ptr<GameContent::CollisionMesh> buildCollisionMesh(const fmt::OcMeshData& md, const std::string& path,
                                                              const SlotQuery& slots);
#  if AVER_MODULE_PBR
bool slotCollidesFromFiles(const std::string& binariesDir, const std::string& contentDir, const std::string& name);
i32 slotSurfaceFromFiles(const std::string& binariesDir, const std::string& contentDir, const std::string& name);
std::vector<std::pair<std::string, assets::TextureUsage>> slotTexturesFromFiles(
        const std::string& binariesDir, const std::string& contentDir, const std::string& name);
#  endif
#endif

} // namespace aver::game::content_detail

namespace aver::game { using namespace content_detail; }
