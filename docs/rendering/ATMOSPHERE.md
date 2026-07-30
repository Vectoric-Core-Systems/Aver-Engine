# The physical atmosphere

Where the sky comes from, why each number is the number it is, and which claims are measured rather
than asserted. The code is `modules/rhi/include/aver/rhi/Atmosphere.hpp` +
`modules/rhi/src/Atmosphere.cpp` (CPU) and the atmosphere block inside `sharedShaderPrelude()` in
`modules/rhi/src/RHIShaders.cpp` (GPU). `tests/rhi/src/AtmosphereTest.cpp` is the oracle.

This closes gaps 1, 2 and 4 of `docs/STATUS.md` §4x. Gap 3 (clouds) and gap 5 (SkyForge) are open.

---

## 1. What it replaces, and what it does not

`SkyAtmosphere::model` picks between two skies:

| | `Authored` (default) | `Physical` |
|---|---|---|
| dome | two sRGB colours, lerped by `pow(dir.z*0.5+0.5, k)` | derived, per frame, from the sun's elevation |
| `k` | authored `atmosphereHeight` | fitted so the dome carries the model's own irradiance |
| sun colour | authored, or a blackbody temperature | authored colour × the air's transmittance at that elevation |
| the visible sky | the same two-colour dome | the full scattering integral, per pixel, in the sky pass |
| distance | a tint on the sky, over an authored density | the atmosphere's own in-scattering and extinction |

**The default is `Authored`, and that is load-bearing.** `scripts/gates.baseline.txt` records 17 gates
across 9 device configurations, and a new model that moved them would report as a regression in all
of them at once. With the default the whole atmosphere block is unreachable and the engine renders
exactly what it did before — verified, 51/51 gates bit-exact across `baseline`, `no-rt` and `all-off`.

`--sky-physical [elevation]` turns it on for a capture run; the Details panel for the sun/sky has a
`Sky Model` combo and an `Air` group behind it.

## 2. The model

Rayleigh + Mie + ozone single scattering along a ray through a spherical shell, plus one isotropic
term standing in for every further bounce. No textures and no LUTs: the sky pass is one fullscreen
march, and everything else in the engine reads the fitted two-colour dome.

Defaults are Earth, in kilometres and per-kilometre coefficients (Hillaire's set):

| | value | note |
|---|---|---|
| planet radius | 6360 km | |
| air depth | 60 km | `exp(-60/8)` is 5.5e-4, so the truncation is under 0.1% |
| Rayleigh β | 5.802 / 13.558 / 33.100 × 10⁻³ km⁻¹ | the λ⁻⁴ that makes zenith blue and horizon pale |
| Rayleigh scale height | 8 km | β·H = 0.109 at 550 nm against a measured 0.097 |
| Mie scatter / extinction | 3.996 / 4.440 × 10⁻³ km⁻¹ | |
| Mie scale height | 1.2 km | |
| Mie anisotropy g | 0.80 | Cornette-Shanks, not plain Henyey-Greenstein |
| ozone β | 0.650 / 1.881 / 0.085 × 10⁻³ km⁻¹ | the Chappuis band, which is why twilight is not brown |
| ozone tent | centre 25 km, half-width 15 km | |
| multi-scatter gain | 1.70 | **fitted — see §5** |
| view steps | 32 | within 3% of converged — see §4 |
| aerial steps | 4 | within 2% of converged |

### Coordinates

The engine's world is centimetres by contract; the model's is kilometres, because 6360 and 8 are not
both representable in a float at any other scale. `gAtmoPlanet.z` is that conversion (1e-5) and the
planet centre sits at world `(0, 0, -planetRadius)`, so world +Z is up and altitude is
`length(P) - planetRadius`.

## 3. Why the sun ray is analytic

A scattering integral's cost is its inner loop: at every sample along the view ray you need the
transmittance from that sample to the sun. Marching it makes the shader O(N·M). This one does not
march it at all.

For an exponential atmosphere the slant column along a ray, as a multiple of the vertical column at
the same altitude, is the **Chapman function**, and for a ray at or above the local horizon it has a
closed form:

    Ch(x, χ) = sqrt(π x / 2) · erfcx( cos χ · sqrt(x / 2) )        χ ≤ 90°

with `x = r / H` and `erfcx(y) = exp(y²)·erfc(y)`. Below the local horizon but still escaping the
planet it reflects about the tangent point:

    Ch(x, χ) = 2·sqrt(π x sinχ / 2)·exp(x(1 - sinχ)) − Ch(x, 180° − χ)

That second branch is a sliver — it is reachable only when `r·sinχ ≥ R`, i.e. only with altitude —
and `x(1 − sinχ)` is bounded there by the altitude in scale heights, so the exponential cannot run
away. Callers test the ground intersection first, and a blocked ray is simply "the sun is down".

`erfcx` is Numerical Recipes' Chebyshev fit with the `exp(-y²)` it carries algebraically cancelled,
leaving **one exp and ten multiply-adds**, fractional error below 1.2e-7.

**Measured** (`AtmosphereTest`, against a 2,000,000-step numeric integral of the same geometry):
worst error 0.125% at H = 8 km and 0.019% at H = 1.2 km, across zenith angles 0-90° and altitudes
0-10 km. The horizontal limit lands on `sqrt(πx/2)` to 5e-8 and the vertical on 1 to 0.13%, the
latter being the true curvature correction rather than an error.

### Ozone has no Chapman form

The tent profile is not exponential, so its slant path is treated as a thin shell at the tent's
centre: `1/sqrt(1 − (r sinχ / r_ozone)²)`, which diverges at tangency and is therefore capped at
`1.2·sqrt(2·r_ozone·w)/w` — the chord a ray actually travels through a tent of half-width `w`,
about 35 airmasses, which is the same order as Rayleigh's horizontal 35.3. The vertical column above
an altitude is piecewise-quadratic and exact. Downward-but-escaping mirrors Chapman's structure.
This is the loosest approximation in the model and it is confined to twilight colour.

## 4. Where the samples go

At these step counts this matters more than how many there are. Density falls exponentially with
altitude, so uniform steps spend most of their samples where there is nothing:

- a **vertical** ray climbs a scale height in 8 km, and 32 uniform steps over a 60 km span were
  **7% off** the converged answer;
- a **horizontal** ray takes 300 km to climb one, and 32 uniform steps over its 875 km span were
  fine.

`t` is therefore sampled as `u^p` with `p = 1 + |cos|`: uniform along the horizon, quadratic straight
up. Segment bounds are exact (`Σ dt = span`), and the sample sits at each segment's midpoint.

**Measured:** 32 view steps land within 3% of 512 across five view/sun geometries; 4 aerial steps
land within 2% of 256 (and 0.5% on transmittance) over distances from 50 m to 40 km.

## 5. The one fitted number

`multiScatterGain` is an isotropic source proportional to `σ_s`, standing in for every bounce after
the first and for the ground returning sunlight to the air. It is a **fit, not a derivation**, and it
is pinned by two independent measurements rather than by taste:

1. **Brightness.** For a clear sky with the sun at 30-60°, diffuse horizontal illuminance is 15-30%
   of direct horizontal. (Typical clear-day figures: ~15 klux diffuse against ~70 klux direct at 48°.
   The photometric ratio runs higher than the radiometric one because skylight is blue-rich.)
   At gain 1.70 the model gives **27.7% / 18.0% / 15.1%** at 30 / 48 / 60°. Single scattering alone
   gives about 7%, i.e. under half the real thing.
2. **Colour.** A real clear zenith runs 15,000-25,000 K, a blue-to-red radiance ratio of about
   2.9-4. At gain 1.70 the model gives **4.09 / 4.06** at 40 / 60°.

**The term is spectrally FLAT, and that was the surprising part.** Second-order scattering formally
goes as `σ_s²`, so shaping the field by the column's own scattering optical depth looks more
principled — and it was tried, and it is wrong here. It puts the zenith's blue/red at 7.2, bluer than
any real sky, and it needs the gain at 3.7 to still carry the right fill light. Flat hits both
targets at once. The reason is saturation: blue is optically thick, so its multiply-scattered field
does not keep growing with `σ_s` the way a thin-medium argument says it should.

Brightness alone could never have caught this — the gain can always be retuned to hit an irradiance
target while the hue goes wherever it likes. Both checks are in `AtmosphereTest` so the term cannot
be quietly "improved" back.

**Independently checked:** with Mie, ozone and this term all switched off, the model's zenith
radiance matches the closed form for a pure Rayleigh atmosphere,

    L/E = P(cosχ) · (1 − exp(−τ₀(1 + 1/μ₀))) / (1 + 1/μ₀)

to 0.1% in red and 1.8% in blue. The residual is the model being spherical and 60 km deep where the
closed form is plane-parallel and unbounded, which is why it scales with optical depth.

## 6. The dome fit — how a physical sky costs nothing per surface

The sky pass marches. **Nothing else does.** Once per frame the CPU evaluates the model at the
zenith, at the horizon and across eight bands between, each averaged over four azimuths, and writes
the result into the same `gSkyZenith` / `gSkyHorizon` / `gSkyParams.x` fields the authored dome used.

Every cheap consumer of the sky then reads a physical one with no change of its own: the ambient
term (`averSkyIrradiance`), environment reflections (`skyColor(R)`), the fog in-scatter target
(`averFogInscatter`), the ground's own radiance (`averGroundRadiance`) and the cloud layer's fill.

### Fitting the exponent

The obvious fit — solve `pow(u, k) = t` through a 45° sample — is wrong, and wrong in a way that
shows up only when the sun moves. With the sun high, the forward-scattered ring at 45° is brighter
than *both* anchors, so no power curve passes through all three and the fit falls back; sweeping the
sun then walked `k` from 0.22 to 2.46 non-monotonically. Since `k` is what every ambient term and
every environment reflection reads, the fill light would visibly breathe with it.

`k` is instead chosen so the dome delivers the **right amount of fill light**, which is the one
property it exists to carry. For an up-facing surface,

    E/π = L_horizon + (L_zenith − L_horizon) · I(k)
    I(k) = 4·[ 2/(k+2) − 1/(k+1) − 0.5^(k+1)/(k+2) + 0.5^(k+1)/(k+1) ]

`I` falls monotonically from 1 at `k = 0`, so a bisection cannot miss. **Measured:** across 6-70° of
elevation `k` never moves more than 0.12 per two degrees; through the first six degrees of sunrise
never more than 0.35, which is the sky genuinely changing rather than the fit slipping.

## 7. Aerial perspective

`averApplyFog` is now two layers that do not compete:

1. the **atmosphere itself**, when the model is on: real extinction and real in-scattering over the
   real distance, from the same `averAtmoScatter` the dome is made of — so the haze and the sky
   cannot disagree, by construction rather than by matching swatches;
2. the **authored height fog** on top, unchanged, which is a level's own weather and still takes the
   sky along the view ray as its target.

Before the physical model existed the second had to stand in for both. This is §4x gap 2.

## 8. The sun as the single input

In `Physical` mode `gLightColor` carries the sun **after** the air: the authored colour times the
transmittance at its own elevation. `gAtmoSunE0` carries the irradiance **above** the air, which is
what the scattering integral needs. Nothing else attenuates, so the attenuation cannot land twice.

The lit pass, the GI injection, the ground's radiance and the sun disk all read `averSunRadiance()`,
so all four redden and dim together as the sun sets — from geometry, with nothing authored to say so.
This is §4x gap 4. **Measured:** transmittance goes 0.94/0.87/0.76 at the zenith to 0.33/0.09/0.01
at 2° and to zero below the planet's edge; the red/blue ratio grows by more than 6× from noon to 2°.

## 9. Two deliberate divergences between the CPU and the GPU copies

The two implementations mirror each other function for function and there is no compiler keeping
them in step, so the places they differ are worth naming:

- **`atmoSkyRadiance` intersects the planet; `averSkyPhysical` does not.** The C++ side is the model
  on its own — what `atmoFitDome` samples and what `AtmosphereTest` checks. The shader draws into a
  scene that brings its **own** ground, and intersecting the model's sphere there drew a hard-edged
  second horizon at a constant elevation, floating above the floor the engine had actually drawn.
  The shader flattens the ray to level and hands over to `averGroundRadiance` across the same
  twenty-degree smoothstep the authored dome uses.
- **The sun's glow.** `PSky` adds `pow(sd, 12)` around the disk in `Authored` mode only. Under the
  physical model the Mie forward lobe already *is* that glow, and keeping both counts the same light
  twice.

## 10. What this does not do

- **Clouds are still ad-hoc** (§4x gap 3): Henyey-Greenstein plus a powder term, lit by the dome
  rather than by the scattering model. They now read a *physical* dome, which is an improvement by
  accident rather than by design.
- **No multiple-scattering LUT.** The isotropic term is a fit; Hillaire's Ψ_ms would be better and
  needs a texture, which needs an SRV in a scene root signature that today declares no descriptor
  table at all. The same constraint that keeps the cloud noise analytic keeps this analytic.
- **The sun disk is evaluated at ground level**, so a camera at altitude sees a disk slightly redder
  than it should. Below a kilometre this is under one 8-bit code.
- **Night is black.** Below the horizon the model delivers nothing, which is correct and is not a
  moon, a star field or an airglow term.
- **No gate covers the physical path.** Adding one means recording new baseline values, which is a
  human's call via `./scripts/record-gates.ps1`. `AtmosphereTest` covers the model; the shader's
  transliteration of it is covered only by the fact that it compiles and renders.
