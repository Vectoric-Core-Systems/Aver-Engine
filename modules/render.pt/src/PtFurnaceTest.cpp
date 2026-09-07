// The white furnace: build the scenes, trace them, and say what the numbers were.
#include "aver/pt/PtFurnaceTest.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <cstring>

namespace aver::pt {

namespace {

constexpr f64 kPi = 3.14159265358979323846;

// THE TWO IORS THE DIELECTRIC FURNACE CHECKS ARE RUN AT. Not one, because the energy-conservation
// claim is "whatever the IOR" (PtFurnaceTest.hpp), and a single IOR could not tell a correct
// implementation apart from one that merely happens to balance at that particular value. Glass and
// diamond span a wide F0 range (0.04 vs ~0.17) while both staying physically ordinary numbers.
constexpr f32 kGlassIor   = 1.5f;
constexpr f32 kDiamondIor = 2.42f;

// The flat quad the single-bounce configurations stand on. Large enough that no primary ray from
// the probe camera comes near an edge, so "every path bounces once and leaves" is a property of the
// geometry rather than of where the camera happens to point.
constexpr f32 kQuadHalf = 4000.0f;

// The enclosure: 400 cm square, 300 cm tall, OPEN AT THE TOP. Open because a fully closed albedo-1
// furnace never terminates -- every path would run to the bounce limit, the escaped fraction would
// be zero and the reading would be zero, which is a true statement about a truncated estimator and
// a useless one about energy. Open at the top specifically so a camera above can look in.
constexpr f32 kCaveX = 2000.0f;   // pushed clear of the quad; they are separate scenes regardless
constexpr f32 kCaveHalf = 200.0f;
constexpr f32 kCaveHeight = 300.0f;

// Builds one two-triangle quad in WORLD space, so every instance can carry an identity transform.
// Winding is not load-bearing: nothing back-face culls here and the shader turns the normal to face
// the ray, which is what makes the furnace a statement about energy and not about authoring.
rhi::MeshHandle makeQuad(rhi::IDevice& dev, const f32 c[4][3], f32 nx, f32 ny, f32 nz) {
    rhi::MeshVertex v[4]{};
    for (u32 i = 0; i < 4; ++i) {
        v[i].px = c[i][0]; v[i].py = c[i][1]; v[i].pz = c[i][2];
        v[i].nx = nx; v[i].ny = ny; v[i].nz = nz;
        v[i].u = (i == 1 || i == 2) ? 1.0f : 0.0f;
        v[i].v = (i >= 2) ? 1.0f : 0.0f;
    }
    const u32 idx[6] = {0, 1, 2, 0, 2, 3};
    return dev.createMesh(v, 4, idx, 6);
}

} // namespace

f64 PtFurnaceTest::Result::channelSpread() const {
    const f64 a = mean(0);
    return std::fmax(std::fabs(mean(1) - a), std::fabs(mean(2) - a));
}

PtFurnaceTest::~PtFurnaceTest() { shutdown(); }

bool PtFurnaceTest::init(rhi::IDevice& dev) {
    shutdown();
    dev_ = &dev;
    res_ = dev.resources();
    if (!res_) return false;
    if (!pt_.init(dev)) { shutdown(); return false; }
    if (!buildGeometry()) { shutdown(); return false; }

    // Straight down at the quad from a metre up, with a narrow cone: every primary ray lands well
    // inside the quad, so nothing here depends on an edge.
    cameras_[0] = {};
    cameras_[0].origin[0] = 0.0f; cameras_[0].origin[1] = 0.0f; cameras_[0].origin[2] = 1000.0f;
    cameras_[0].tanHalfFov = 0.1f;
    // Down through the cave's open top onto its floor: 900 cm of travel at tan 0.05 lands every ray
    // within 45 cm of the centre of a floor 200 cm in half-extent.
    cameras_[1] = {};
    cameras_[1].origin[0] = kCaveX; cameras_[1].origin[1] = 0.0f; cameras_[1].origin[2] = 900.0f;
    cameras_[1].tanHalfFov = 0.05f;

    // maxBounces is FOUR in the single-bounce configurations, not one. The claim being made is that
    // those paths bounce exactly once because nothing else is reachable, and a bound of one would
    // enforce that rather than measure it -- the mean bounce count reported below would then be
    // true by construction and would prove nothing.
    configs_[kOpenCorrect]    = {"open/albedo-1/correct",   0, 0,  4, PtDefect::None,     0};
    configs_[kOpenTimesPi]    = {"open/albedo-1/xPI",       0, 0,  4, PtDefect::TimesPi,  0};
    configs_[kOpenNoCosine]   = {"open/albedo-1/no-cos",    0, 0,  4, PtDefect::NoCosine, 0};
    configs_[kOpenHalfAlbedo] = {"open/albedo-0.5/correct", 1, 0,  4, PtDefect::None,     0};
    // SCENES 3 AND 4 ARE THE SAME QUAD MESH AS SCENE 0, made dielectric instead of Lambertian --
    // see buildGeometry() below. Same camera (0), same 4-bounce budget: a dielectric hit here always
    // resolves and escapes on its first attempt (see the single-bounce check in evaluate()), so the
    // bound is generous headroom, not a load-bearing choice.
    configs_[kOpenDielectricGlass]   = {"open/dielectric-1.5/correct",         3, 0, 4, PtDefect::None, 0};
    configs_[kOpenDielectricDiamond] = {"open/dielectric-2.42/correct",        4, 0, 4, PtDefect::None, 0};
    configs_[kOpenDielectricDefect]  = {"open/dielectric-1.5/no-pdf-cancel",   3, 0, 4,
                                        PtDefect::DielectricNoPdfCancel,       0};
    configs_[kOpenSpecDielectric]  = {"open/spec-r1.0-m0/correct",   5, 0, 4, PtDefect::None, 0};
    configs_[kOpenSpecMetalRough]  = {"open/spec-r1.0-m1/correct",   6, 0, 4, PtDefect::None, 0};
    configs_[kOpenSpecMetalSmooth] = {"open/spec-r0.3-m1/correct",   7, 0, 4, PtDefect::None, 0};
    configs_[kCaveShallow]    = {"cave/4-bounce/correct",   2, 1,  4, PtDefect::None,     0};
    configs_[kCaveDeep]       = {"cave/32-bounce/correct",  2, 1, 32, PtDefect::None,     0};
    // THE DETERMINISM PAIR RUNS IN THE CAVE, NOT ON THE QUAD, AND THAT IS THE SECOND ATTEMPT.
    //
    // The first version ran it on the open quad, and it was VACUOUS: a correct albedo-1 furnace has
    // exactly zero variance -- every path carries precisely L whatever random numbers it drew -- so
    // the accumulator holds no trace of the sample stream at all. Deliberately reseeding the replay
    // to a different block of sample indices changed nothing and the check stayed green, which is
    // the exact failure it exists to catch.
    //
    // In the cave the answer DOES depend on the stream: whether a path finds the opening within its
    // bounce budget is what each pixel's escaped count and radiance are made of. evaluate() also
    // refuses to believe the comparison unless that variation is actually present, so the vacuity
    // cannot come back quietly.
    configs_[kSeedFirst]      = {"seed/first-block",        2, 1,  4, PtDefect::None,     1};
    configs_[kSeedReplay]     = {"seed/replayed-block",     2, 1,  4, PtDefect::None, kSteps};

    for (u32 r = 0; r < kRuns; ++r) {
        if (!pt_.createTarget(configs_[r].scene, kProbeDim, kProbeDim, targets_[r])) {
            AVER_ERROR("[PT] furnace: could not allocate the accumulator for '{}'", configs_[r].label);
            shutdown();
            return false;
        }
    }

    rhi::BufferDesc rb;
    rb.bytes = targets_[0].bytes() * kRuns;
    rb.kind  = rhi::BufferKind::Readback;
    rb.debugName = "pt furnace readback";
    readback_ = res_->createBuffer(rb);
    if (!readback_) { AVER_ERROR("[PT] furnace: no readback buffer"); shutdown(); return false; }

    AVER_INFO("[PT] furnace armed: {} configurations, {}x{} probes, {} samples each",
              static_cast<u32>(kRuns), kProbeDim, kProbeDim, kSamplesPerStep * kSteps);
    return true;
}

void PtFurnaceTest::shutdown() {
    if (res_) {
        for (PtTarget& t : targets_) pt_.destroyTarget(t);
        if (readback_) res_->destroyBuffer(readback_);
    }
    readback_ = 0;
    pt_.shutdown();
    meshes_.clear();
    seedFirstRaw_.clear();
    seedReplayRaw_.clear();
    for (Result& r : results_) r = {};
    stage_ = 0;
    finished_ = false;
    passed_ = false;
    failures_ = 0;
    furnaceL_ = 0.0f;
    dev_ = nullptr;
    res_ = nullptr;
}

bool PtFurnaceTest::buildGeometry() {
    const f32 q[4][3] = {{-kQuadHalf, -kQuadHalf, 0.0f}, { kQuadHalf, -kQuadHalf, 0.0f},
                         { kQuadHalf,  kQuadHalf, 0.0f}, {-kQuadHalf,  kQuadHalf, 0.0f}};
    const rhi::MeshHandle quad = makeQuad(*dev_, q, 0.0f, 0.0f, 1.0f);

    const f32 x0 = kCaveX - kCaveHalf, x1 = kCaveX + kCaveHalf;
    const f32 y0 = -kCaveHalf, y1 = kCaveHalf, z1 = kCaveHeight;
    const f32 floor[4][3] = {{x0, y0, 0}, {x1, y0, 0}, {x1, y1, 0}, {x0, y1, 0}};
    const f32 wxm[4][3]   = {{x0, y0, 0}, {x0, y1, 0}, {x0, y1, z1}, {x0, y0, z1}};
    const f32 wxp[4][3]   = {{x1, y0, 0}, {x1, y1, 0}, {x1, y1, z1}, {x1, y0, z1}};
    const f32 wym[4][3]   = {{x0, y0, 0}, {x1, y0, 0}, {x1, y0, z1}, {x0, y0, z1}};
    const f32 wyp[4][3]   = {{x0, y1, 0}, {x1, y1, 0}, {x1, y1, z1}, {x0, y1, z1}};

    meshes_ = {quad,
               makeQuad(*dev_, floor, 0.0f, 0.0f, 1.0f),
               makeQuad(*dev_, wxm,  1.0f, 0.0f, 0.0f),
               makeQuad(*dev_, wxp, -1.0f, 0.0f, 0.0f),
               makeQuad(*dev_, wym,  0.0f, 1.0f, 0.0f),
               makeQuad(*dev_, wyp,  0.0f,-1.0f, 0.0f)};
    for (rhi::MeshHandle m : meshes_) if (!m) {
        AVER_ERROR("[PT] furnace: the device would not take the test geometry");
        return false;
    }

    PtSurface s;
    s.mesh = quad;
    const u32 open = pt_.addSurface(s);              // albedo 1, the default
    s.albedo[0] = s.albedo[1] = s.albedo[2] = 0.5f;
    const u32 half = pt_.addSurface(s);
    s.albedo[0] = s.albedo[1] = s.albedo[2] = 1.0f;

    u32 cave[5];
    for (u32 i = 0; i < 5; ++i) { s.mesh = meshes_[1 + i]; cave[i] = pt_.addSurface(s); }

    // THE DIELECTRIC SURFACES, on the SAME quad mesh `open` already stands on. An energy check about
    // reflection and refraction needs no new geometry, only a different material on geometry this
    // file already built -- see PtFurnaceTest.hpp for why two IORs and not one.
    s.mesh = quad;
    s.ior = kGlassIor;
    const u32 glass = pt_.addSurface(s);
    s.ior = kDiamondIor;
    const u32 diamond = pt_.addSurface(s);

    // THE METAL/ROUGH SURFACES, again on the same quad. `ior` goes back to 0 -- these are not
    // dielectric-interface surfaces, they are opaque PBR ones, and leaving it set would send them
    // down ptScatterDielectric instead. Setting `roughness` at all is what opts a surface into the
    // specular lobe (PtSurface::roughness); everything above this line leaves it negative and is
    // therefore still the pure Lambertian these checks have always measured.
    s.ior = 0.0f;
    s.roughness = 1.0f; s.metallic = 0.0f;
    const u32 specDielectric = pt_.addSurface(s);
    s.roughness = 1.0f; s.metallic = 1.0f;
    const u32 specMetalRough = pt_.addSurface(s);
    s.roughness = 0.3f; s.metallic = 1.0f;
    const u32 specMetalSmooth = pt_.addSurface(s);
    s.roughness = -1.0f; s.metallic = 0.0f;   // leave the shared PtSurface as it was found

    // FIVE ACCELERATION STRUCTURES, not one scene with everything spread out. "The quad is far
    // enough from the cave that a bounce off it never reaches" is a probability argument, and the
    // single-bounce claim below must not rest on one: in scenes 0, 3 and 4 there is literally
    // nothing else for a ray to hit.
    pt_.addScene(&open, 1);      // scene 0
    pt_.addScene(&half, 1);      // scene 1
    pt_.addScene(cave, 5);       // scene 2
    pt_.addScene(&glass, 1);     // scene 3
    pt_.addScene(&diamond, 1);   // scene 4
    pt_.addScene(&specDielectric, 1);    // scene 5
    pt_.addScene(&specMetalRough, 1);    // scene 6
    pt_.addScene(&specMetalSmooth, 1);   // scene 7
    return pt_.prepare();
}

void PtFurnaceTest::prePass(rhi::IRenderContext& ctx) {
    if (finished_ || !dev_ || !res_ || !pt_.available()) return;

    if (stage_ == 0) {
        if (!pt_.buildScenes(ctx)) {
            AVER_ERROR("[PT] furnace: the acceleration structures would not build");
            finished_ = true;
        }
        ++stage_;
        return;
    }

    if (stage_ <= kSteps) {
        const u32 step = stage_ - 1;
        if (step == 0) {
            // Read from the DEVICE, not from a private constant: the furnace this measures is the
            // one the engine's own shader prelude hands every shading path.
            furnaceL_ = dev_->skyAtmosphere().furnaceRadiance;
            if (!(furnaceL_ > 0.0f)) {
                AVER_ERROR("[PT] furnace INCONCLUSIVE: SkyAtmosphere::furnaceRadiance is {}, so the "
                           "environment is the ordinary sky and there is no known number to compare "
                           "against. Run with --furnace-test / --pt-furnace, which set it",
                           furnaceL_);
                finished_ = true;
                return;
            }
            // THE SUN-ON VARIANT STILL DOES NOT APPLY TO THIS HARNESS -- but no longer because the
            // integrator cannot reach the sun at all. It now can: PtShaders.hpp's ptDirectSun() fires
            // a next-event shadow ray at the sun on every hit, precisely so a delta light with no
            // angular size does not need a BRDF-sampled ray to land on it by chance the way a sampled
            // bounce would have to. That closed the "reads 0 for a reason that has nothing to do with
            // energy" hole this comment used to describe.
            //
            // WHAT STILL MAKES IT INCONCLUSIVE is a different, more fundamental mismatch: every check
            // below is an IDENTITY over a UNIFORM environment -- "albedo 1 reads L, REGARDLESS OF
            // GEOMETRY" -- and that invariance is what a spatially uniform environment gives for free
            // (whichever way a surface points, the same radiance L arrives from every direction). A
            // single directional light has no such invariance: the correct reading of a sun-lit quad
            // is albedo/PI * dot(N,L) * sunRadiance, which DEPENDS on the surface's orientation to the
            // light -- there is no single number "L" this harness's open/cave scenes could compare
            // against the way they compare against the uniform-environment furnaceRadiance today.
            // Checking the sun path for real needs a new expected-value formula that accounts for
            // that orientation dependence, not just this early return deleted -- real, separable
            // follow-up work, not a consequence of anything this change touched.
            if (dev_->skyAtmosphere().furnaceSun) {
                AVER_ERROR("[PT] furnace INCONCLUSIVE: the SUN-ON furnace's correct reading depends on "
                           "surface orientation to the light (albedo/PI * N.L * sunRadiance), unlike "
                           "the uniform-environment identity every other check here rests on -- this "
                           "harness has no matching expected-value formula for that yet, even though "
                           "the integrator itself now reaches the sun via next-event estimation "
                           "(PtShaders.hpp's ptDirectSun). Run --pt-furnace without --furnace-sun");
                finished_ = true;
                return;
            }
            AVER_INFO("[PT] furnace on: environment radiance L = {:.6f} (linear)", furnaceL_);
        }

        for (u32 r = 0; r < kRuns; ++r) {
            const Config& c = configs_[r];
            PtDispatch d;
            d.maxBounces = c.maxBounces;
            d.defect     = c.defect;
            d.samples    = kSamplesPerStep;
            if (c.onlyStep != 0) {
                // The determinism pair: the SAME sample block, dispatched on two different frames.
                if (step + 1 != c.onlyStep) continue;
                d.firstSample = 0;
                d.reset = true;
            } else {
                d.firstSample = step * kSamplesPerStep;
                d.reset = (step == 0);   // the buffer's contents are undefined until it is written
            }
            pt_.accumulate(ctx, targets_[r], cameras_[c.camera], d);
        }
        ++stage_;
        return;
    }

    if (stage_ == kSteps + 1) {
        for (u32 r = 0; r < kRuns; ++r)
            pt_.copyForReadback(ctx, targets_[r], readback_, static_cast<u64>(r) * targets_[r].bytes());
        ++stage_;
        return;
    }

    // The copies were recorded LAST frame and that frame has been submitted. waitIdle is what makes
    // the readback mean anything: readBuffer synchronises nothing by contract.
    res_->waitIdle();
    evaluate();
    finished_ = true;
}

bool PtFurnaceTest::check(const char* label, f64 measured, f64 expected, f64 relTolerance,
                          const char* why) {
    const f64 scale = std::fabs(expected) > 1e-12 ? std::fabs(expected) : 1.0;
    const f64 err = std::fabs(measured - expected) / scale;
    if (err <= relTolerance) {
        AVER_INFO("[PT]   {} = {:.6f}, expected {:.6f} ({:.2e} relative)",
                  label, measured, expected, err);
        return true;
    }
    ++failures_;
    AVER_ERROR("[PT]   {} FAIL = {:.6f}, expected {:.6f} ({:.2e} relative, tolerance {:.0e}). {}",
               label, measured, expected, err, relTolerance, why);
    return false;
}

void PtFurnaceTest::evaluate() {
    const usize floats = static_cast<usize>(kProbeDim) * kProbeDim * kPtAccumElementsPerPixel * 4;
    std::vector<f32> buf(floats);

    for (u32 r = 0; r < kRuns; ++r) {
        if (!res_->readBuffer(readback_, buf.data(), floats * sizeof(f32),
                              static_cast<u64>(r) * targets_[r].bytes())) {
            AVER_ERROR("[PT] furnace INCONCLUSIVE: readback failed for '{}'", configs_[r].label);
            ++failures_;
            continue;
        }
        Result& out = results_[r];
        for (u32 p = 0; p < kProbeDim * kProbeDim; ++p) {
            const f32* rad = &buf[static_cast<usize>(p) * 8];
            const f32* st  = &buf[static_cast<usize>(p) * 8 + 4];
            for (u32 c = 0; c < 3; ++c) out.radiance[c] += rad[c];
            out.escaped      += st[0];
            out.bounceEvents += st[1];
            out.traced       += st[2];
        }
        if (r == kSeedFirst)  seedFirstRaw_  = buf;
        if (r == kSeedReplay) seedReplayRaw_ = buf;
    }

    const f64 L = furnaceL_;
    for (u32 r = 0; r < kRuns; ++r) {
        const Result& x = results_[r];
        AVER_INFO("[PT] {:<24} mean radiance {:.6f}  (L = {:.6f}, ratio {:.6f})  escaped {:.5f}  "
                  "mean bounces {:.4f}  paths {:.0f}",
                  configs_[r].label, x.mean(0), L, L > 0.0 ? x.mean(0) / L : 0.0,
                  x.escapeFraction(), x.meanBounces(), x.traced);
        if (x.channelSpread() > 1e-6) {
            ++failures_;
            AVER_ERROR("[PT]   '{}' came back non-grey: channels {:.6f}/{:.6f}/{:.6f}. A grey "
                       "environment cannot produce that, so this is a swizzle, not an energy error",
                       configs_[r].label, x.mean(0), x.mean(1), x.mean(2));
        }
    }

    // ---- 1. is the single-bounce configuration actually single-bounce? ----
    // Everything below rests on this. If a path in scene 0 bounced twice, a defect that multiplies
    // the estimator by PI would read PI^2*L and the numbers this test asserts would be wrong for a
    // reason that has nothing to do with the integrator.
    const Result& open = results_[kOpenCorrect];
    if (open.escapeFraction() != 1.0 || open.meanBounces() != 1.0) {
        ++failures_;
        AVER_ERROR("[PT] furnace INCONCLUSIVE: the open scene escaped on {:.6f} of paths at {:.6f} "
                   "bounces each, and both must be exactly 1. The PI and 2L expectations below are "
                   "single-bounce claims and mean nothing otherwise",
                   open.escapeFraction(), open.meanBounces());
    }

    // ---- 2. the furnace itself ----
    check("open/albedo-1/correct   ", open.mean(0), L, 1e-4,
          "An albedo-1 surface in a uniform environment of radiance L must be exactly as bright as "
          "the environment. A reading of PI*L is a missing 1/PI, 2L a missing cosine, and anything "
          "lower is energy being lost per bounce.");

    // ---- 3. ...shown FAILING on purpose, which is the only thing that makes point 2 worth having ----
    check("open/albedo-1/xPI      ", results_[kOpenTimesPi].mean(0), kPi * L, 1e-4,
          "The estimator was deliberately scaled by PI and the furnace did NOT read PI*L, so the "
          "oracle cannot see a global scale -- which is the single most likely error in a "
          "Lambertian estimator and the only class of error a relational check can never find.");
    check("open/albedo-1/no-cos   ", results_[kOpenNoCosine].mean(0), 2.0 * L, 0.10,
          "The cosine was deliberately dropped from the numerator and the furnace did NOT read 2L, "
          "so the oracle cannot see a missing cosine either. The tolerance is 10% and not 1e-4 "
          "because that estimator is albedo/cos, whose spread is wide by construction; the correct "
          "one has ZERO variance, which is why it is held to 1e-4.");

    // ---- 4. the albedo multiplies once, not twice and not never ----
    check("open/albedo-0.5/correct", results_[kOpenHalfAlbedo].mean(0), 0.5 * L, 1e-4,
          "Halving the albedo must halve the reading exactly. L means the albedo was ignored, "
          "L/4 means it was applied twice.");

    // ---- 5. the dielectric: a different BSDF, the same energy claim ----
    // Everything below rests on the SAME single-bounce premise as check 1 above, re-verified for
    // this material kind rather than assumed to carry over: a smooth dielectric only has two
    // outgoing directions (reflect, refract), and on a lone flat quad BOTH of them leave the scene
    // immediately, so this should ALSO read exactly 1 escape and 1 bounce per path. If it did not --
    // say, a refracted ray re-hitting the same quad from underneath because the bias offset pushed
    // it the wrong way -- the checks below would be measuring a multi-bounce scene while believing
    // it was single-bounce, the same failure mode check 1 exists to rule out for the Lambertian case.
    const Result& glassR = results_[kOpenDielectricGlass];
    if (glassR.escapeFraction() != 1.0 || glassR.meanBounces() != 1.0) {
        ++failures_;
        AVER_ERROR("[PT] furnace INCONCLUSIVE: the dielectric open scene escaped on {:.6f} of paths "
                   "at {:.6f} bounces each, and both must be exactly 1 for the identity below to mean "
                   "what it claims", glassR.escapeFraction(), glassR.meanBounces());
    }

    // THE CLAIM ITSELF: for a NON-ABSORBING dielectric, energy in equals energy out regardless of
    // IOR. Every path either reflects or refracts with weight exactly 1 (the Fresnel probability of
    // whichever branch was taken and the Fresnel weight applied on it are the SAME number, and they
    // cancel), and a uniform environment returns the same L down either escape direction -- so the
    // reading needs no reflectance value to be known, only that reflect+refract are weighted to
    // conserve energy between them. Checked at two IORs (see kGlassIor/kDiamondIor) because a single
    // one could not distinguish a correct implementation from one that happens to balance only there.
    check("open/dielectric-1.5/correct ", glassR.mean(0), L, 1e-4,
          "A non-absorbing dielectric must return exactly the environment radiance, whatever its "
          "IOR: reflection and refraction are chosen with the exact probabilities their own Fresnel "
          "weights need to cancel to 1. A reading away from L means energy is being lost or invented "
          "at the interface.");
    check("open/dielectric-2.42/correct", results_[kOpenDielectricDiamond].mean(0), L, 1e-4,
          "The identical claim at a much higher IOR (diamond, F0 ~0.17 against glass's 0.04) -- "
          "this is what backs the word 'whatever' in the claim above rather than leaving it "
          "asserted at one convenient value.");

    // ---- 6. ...and shown FAILING, the same way checks 2/3 justify check 1 above ----
    // THE DEFECT: the Fresnel term applied a SECOND time as a multiplicative weight, on top of
    // already having been the branch probability. Per PATH the estimator becomes reflectance^2 (if
    // the reflect branch, chosen w.p. reflectance, was taken) or (1-reflectance)^2 (if refract,
    // chosen w.p. 1-reflectance) times L; since the environment is uniform this holds regardless of
    // which direction was actually sampled, so the EXPECTED reading is exactly
    // L * (reflectance^2 + (1-reflectance)^2) -- strictly less than L for any reflectance strictly
    // between 0 and 1 (the parabola's minimum, 0.5, is at reflectance=0.5; it approaches 1 only at
    // the endpoints). The probe camera looks almost straight down (tanHalfFov 0.1, ~5.7 degrees of
    // spread), so reflectance is Schlick's F0 to within about 1e-10 relative across the whole probe,
    // which is what makes an EXACT expected value possible here rather than only a "must not equal
    // L" statement.
    //
    // THE TOLERANCE IS 5e-3 AND NOT 1e-4, AND THAT IS ARITHMETIC RATHER THAN A CLIMBDOWN. It first
    // shipped at 1e-4 by analogy with the correct-dielectric checks above, and failed at 5.2e-4
    // relative -- which is not a bug in the estimator, it is the estimator's own noise floor.
    //
    // The distinction the 1e-4 checks rely on is VARIANCE, not exactness. A CORRECT dielectric path
    // carries weight exactly 1 down whichever branch it takes, and a uniform environment returns L
    // down both, so every path reads precisely L and the accumulator has ZERO variance -- 1e-4 there
    // is really just a float-rounding allowance. THIS estimator is the opposite: the two branches
    // return DIFFERENT values (F*L when it reflects, (1-F)*L when it refracts), so the mean is
    // correct but each path is a coin flip and the accumulator has genuine Monte Carlo spread.
    //
    //     F0 = 0.04, so per path X is 0.04L with p = 0.04 and 0.96L with p = 0.96
    //     E[X]   = F0^2 + (1-F0)^2                  = 0.9232 L
    //     E[X^2] = F0^3 + (1-F0)^3                  = 0.8848 L^2
    //     Var    = 0.8848 - 0.9232^2               = 0.03250 L^2   ->  SD = 0.1803 L
    //     SE     = SD / sqrt(131072 paths)         = 4.98e-4 L     ->  5.4e-4 relative to the mean
    //
    // The observed 5.2e-4 was ONE standard error. A tolerance below an estimator's standard error is
    // not a strict check, it is a check that fails at random, and the sample count here is fixed by
    // the probe grid rather than raisable to shrink it. 5e-3 is about nine sigma: still two orders
    // tighter than the 0.10 the no-cosine check needs, and nowhere near loose enough to accept the
    // reading this exists to reject (a working double-count guard reads L, which is 8.3% away --
    // 150 sigma).
    const f64 r0 = ((1.0 - static_cast<f64>(kGlassIor)) / (1.0 + static_cast<f64>(kGlassIor))) *
                   ((1.0 - static_cast<f64>(kGlassIor)) / (1.0 + static_cast<f64>(kGlassIor)));
    const f64 expectedDefect = (r0 * r0 + (1.0 - r0) * (1.0 - r0)) * L;
    check("open/dielectric-1.5/no-pdf-cancel", results_[kOpenDielectricDefect].mean(0), expectedDefect,
          5e-3,
          "The defect applies the Fresnel term a second time as a multiplicative weight instead of "
          "letting it cancel against the branch probability it already was -- the discrete-choice "
          "counterpart of PT_DEFECT_NO_COSINE forgetting the pdf it must divide back out. A reading "
          "at L means the oracle cannot see a double-counted Fresnel weight either.");

    // ---- 6b. the metal/rough lobe: a third BSDF, the same absolute energy claim ----
    //
    // WHY THE SAME NUMBER FOR ALL THREE. The furnace's claim does not care which BSDF a surface
    // wears: an albedo-1 surface in a uniform environment of radiance L reflects all of it and must
    // read exactly L. A diffuse-plus-GGX split has to conserve energy BETWEEN its two lobes -- take
    // too much for specular and the diffuse lobe is short, take too little and they sum past one --
    // and a lobe-selection probability has to cancel exactly against the weight it divides. Both are
    // invisible to any relational test, which is why they are checked against an absolute here.
    //
    // THE ROUGH CONDUCTOR IS THE ONE THAT MATTERS. F0 is 1 for an albedo-1 metal, so Fresnel takes
    // the whole path and the reading is a direct measurement of the geometry term: whatever Smith
    // shadowing removes at roughness 1 is energy a real surface would have scattered again between
    // facets, and a single-scattering model has no mechanism to return it. That is exactly the shape
    // of the loss the RASTER furnace already caught once (a white metal reading 45%), so this file
    // asserts it rather than trusting that the tracer happens not to have it.
    check("open/spec-r1.0-m0/correct", results_[kOpenSpecDielectric].mean(0), L, 5e-3,
          "An albedo-1 dielectric with a specular lobe must still reflect all of L. Reading high "
          "means the specular lobe was added on top of a full-strength diffuse one instead of "
          "sharing the energy with it; reading low means the (1 - F) the diffuse lobe is scaled by "
          "does not match the F the specular lobe actually returns.");
    check("open/spec-r1.0-m1/correct", results_[kOpenSpecMetalRough].mean(0), L, 5e-3,
          "An albedo-1 conductor has F0 = 1, so a correct lobe returns every photon regardless of "
          "roughness. Reading low here is single-scattering energy loss -- the Smith term removing "
          "inter-facet scattering the model never puts back -- and is the same defect a white metal "
          "at 45% was in the rasteriser.");
    check("open/spec-r0.3-m1/correct", results_[kOpenSpecMetalSmooth].mean(0), L, 5e-3,
          "The same conductor near-smooth, where microfacet shadowing is slight. This one passing "
          "while the roughness-1 case fails is the signature of missing multiple-scattering "
          "compensation rather than an error in the lobe itself -- which is why both are here.");

    // ---- 7. more than one bounce, where the answer is L times what escaped ----
    // The identity is exact and needs NO baseline: both sides come out of the same paths. A path
    // that ran out of bounces contributes nothing, so a finite bounce count reads low BY EXACTLY
    // the truncated fraction -- and a stray PI would break the identity rather than shifting both
    // sides together.
    for (u32 r : {static_cast<u32>(kCaveShallow), static_cast<u32>(kCaveDeep)}) {
        const Result& c = results_[r];
        check(configs_[r].label, c.mean(0), L * c.escapeFraction(), 1e-4,
              "Inside an enclosure the furnace must read L times the fraction of paths that "
              "escaped, because a truncated path carries nothing. Both sides are measured from the "
              "same paths, so a mismatch is the estimator and never the geometry.");
    }

    const f64 shallow = results_[kCaveShallow].escapeFraction();
    const f64 deep    = results_[kCaveDeep].escapeFraction();
    if (!(shallow < 0.99)) {
        ++failures_;
        AVER_ERROR("[PT] furnace INCONCLUSIVE: {:.5f} of the 4-bounce cave paths escaped, so almost "
                   "nothing was truncated and the multi-bounce identity above was tested against "
                   "essentially the same case as the open scene", shallow);
    }
    if (!(deep > shallow)) {
        ++failures_;
        AVER_ERROR("[PT] furnace FAIL: raising the bounce limit from 4 to 32 did not raise the "
                   "escaped fraction ({:.5f} -> {:.5f}). The bounce limit is not being applied",
                   shallow, deep);
    }
    AVER_INFO("[PT] convergence: the cave reads {:.6f} of L at 4 bounces and {:.6f} at 32, as {:.5f} "
              "of its paths escape and then {:.5f} do",
              L > 0.0 ? results_[kCaveShallow].mean(0) / L : 0.0,
              L > 0.0 ? results_[kCaveDeep].mean(0) / L : 0.0, shallow, deep);

    // ---- 8. determinism ----
    // Whether the block being compared can DISTINGUISH two sample streams at all. It has to be
    // asked: on the open quad it cannot -- a correct albedo-1 furnace has zero variance and every
    // pixel reads exactly L no matter what was drawn -- and a bit-exact comparison of a constant
    // block is a check that can never fail. That is why the pair traces the cave.
    f32 lo = 0.0f, hi = 0.0f;
    for (usize p = 0; p * 8 < seedFirstRaw_.size(); ++p) {
        const f32 v = seedFirstRaw_[p * 8];
        if (p == 0) { lo = hi = v; }
        lo = std::fmin(lo, v);
        hi = std::fmax(hi, v);
    }

    if (seedFirstRaw_.size() != seedReplayRaw_.size() || seedFirstRaw_.empty()) {
        ++failures_;
        AVER_ERROR("[PT] furnace INCONCLUSIVE: the determinism pair did not read back");
    } else if (hi <= lo) {
        ++failures_;
        AVER_ERROR("[PT] furnace INCONCLUSIVE: every pixel of the determinism block reads the same "
                   "{:.6f}, so it carries no information about the sample stream and comparing two "
                   "copies of it could never fail. Trace it somewhere the answer depends on the "
                   "random numbers", lo);
    } else if (std::memcmp(seedFirstRaw_.data(), seedReplayRaw_.data(),
                           seedFirstRaw_.size() * sizeof(f32)) != 0) {
        ++failures_;
        u32 differing = 0;
        for (usize i = 0; i < seedFirstRaw_.size(); ++i)
            if (seedFirstRaw_[i] != seedReplayRaw_[i]) ++differing;
        AVER_ERROR("[PT] furnace FAIL (not deterministic): sample block 0 traced on the first "
                   "accumulation frame and again on the last differs in {} of {} floats. The seed "
                   "must come from (pixel, sampleIndex) and nothing else -- a frame counter or a "
                   "wall clock reads plausibly and is not reproducible",
                   differing, static_cast<u32>(seedFirstRaw_.size()));
    } else {
        AVER_INFO("[PT]   seed/determinism = bit-identical over {} floats spanning {:.6f}..{:.6f}: "
                  "the same sample block traced {} frames apart produced the same bits",
                  static_cast<u32>(seedFirstRaw_.size()), lo, hi, kSteps - 1);
    }

    passed_ = failures_ == 0;
    if (passed_)
        AVER_INFO("[PT] FURNACE PASS: the integrator reads L to 1e-4 in an albedo-1 furnace, L/2 at "
                  "albedo 0.5, L times the escaped fraction inside an enclosure, and L again from a "
                  "non-absorbing dielectric at two different IORs -- and the three deliberately "
                  "broken estimators beside them read PI*L, 2L and L*(F0^2+(1-F0)^2), so the oracle "
                  "is known to be able to see all three");
    else
        AVER_ERROR("[PT] FURNACE FAIL: {} of the checks above did not hold", failures_);
}

} // namespace aver::pt
