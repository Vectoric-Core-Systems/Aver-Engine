# Wave 9: Game accessibility (design, frozen contract)

2026-10-17. Design only. The implementing agent follows this without the author. Reference: Unreal Engine 5 accessibility tooling and the Xbox Accessibility Guidelines (XAG: 101 text display, 102 contrast, 103 captions, 107 text-to-speech, 109 input, 112 reduce motion, 113 colour filters; numbers are advisory).

Rules in force: upgrade the existing systems (settings screen, UI tree, post chain, audio ABI, Enhanced Input, localization, sequencer events), never a parallel one; no new third-party code; lean comments; D3D12 and Vulkan parity (the only GPU work is one post-chain branch and one UI-shader branch, both compiled from shared HLSL); Aver Node nodes and C# for every runtime surface; module boundaries hold (`docs/MODULES.md`); no new DLL and no new module (so `scripts/game.allowlist`, `scripts/payload.allowlist`, `cmake/modules.catalog.json` and the module manifests do not change).

Patent check (done live 2026-10-17, see open question 1): the searches found filed art on "daltonization" for games (US 10118097, US 20140066196; claims not read) and nothing on the Machado 2009 simulation matrices. The design uses only published 2005 to 2009 academic methods. Captions, UI scaling, toggles and TTS are decades-old public art.

## 0. Decisions in one screen

1. **One native state, one key table.** Every option is a float under a stable key (`access.*`, plus the existing `graphics.uiScale`) in a table in a new header-only `AccessKeys.hpp` (kind, min, max, step, default). Native hosts load it from `Settings.ini` text at start (the `savedCulture` arrangement, `GameLocalization.cpp:96-111`), so the first frame is right with or without scripting. C# sets keys through `aver_fw_access_set`; the native side tracks dirty keys; a C# tick drains them into `Settings.SetFloat` and a debounced `Settings.Flush`. Floats because the existing settings screen stores everything as floats (`UiGameSettings.cs:14-21`) and `aver_settings_get_bool` would reject `"1.000000"` (`Settings.cpp:144-153`).
2. **No new DLL, no new module.** Native code lives in `Aver.Framework` (header-only engines like `EventBus.hpp`, one `AccessAbi.cpp`, a new `framework_access_abi.h` with its own version, `AVER_FW_ABI_VERSION_MINOR` untouched). OS speech lives in `Aver.Platform` beside the other per-OS files. Colour maths lives in `Aver.RHI`.
3. **Captions are data plus the audio ABI, not a second audio system.** A new text format `.occaptions` (cues, lines, speakers; text and names are localization references). Audio ABI gets one additive hook, `aver_audio_set_play_observer`, fired after every successful `aver_audio_play/_play_at/_stream_play/_stream_play_at/_music_play`, so scripts, graph nodes and sequencer audio tracks all produce captions with no per-caller change. The sequencer can also fire `Caption.Show` from an Event track key (existing bus route, nothing in the sequencer changes).
4. **Captions draw through Aver.UI** as a passive Overlay root (`UiCaptionView`), pushed lines each frame from the host. Size (4 presets), background opacity, top or bottom, speaker names and colours, direction hints, line cap.
5. **Colour vision = two branches, one include.** `aver_colour_vision.hlsli` (Machado 2009 severity-1 matrices, plus the classic Fidaner error shift for correction) is `#include`d by `PSComposite` (scene) and by `UiPS` (HUD, which draws after the tonemap and would otherwise be unfiltered). `PostSettings` gains two fields; `fillPostLens` packs one new `PostCB` row for both backends; the UI renderer gets one root-constant dword. Defaults leave every frame bit-identical.
6. **UI scale, text scale, high contrast are properties of the tree, not of widgets.** `graphics.uiScale` (exists) widens to 0.75 to 2.0; new `UiTree::setAccess({textScale, highContrast})` multiplies `fontPx` and swaps to the existing `contrast` theme while ignoring per-widget colour overrides.
7. **Reduce motion needs engine primitives that do not exist.** Grep finds no camera shake and no head bob anywhere (`docs/SEQUENCER.md:86` lists shake as not covered). Motion blur, chromatic aberration, grain and lens flare exist as `PostSettings` fields. v1 adds a tiny `CameraFx` (trauma shake and a speed-driven bob, `CameraFx.hpp`) so the toggles gate something real, plus a master "Reduce motion" that zeroes all of it.
8. **Input assist rides the Enhanced Input layer.** Per-action *toggle instead of hold* and two global timing scales (shorter holds, longer tap and combo windows) are applied inside `actionEval` and `stepTrigger` through two small hooks. Games that read raw keys (`Input.IsKeyDown`) are not assisted; the docs say so.
9. **Speech = the game talking, via the OS TTS.** `aver::platform::SpeechSynth` (Windows SAPI `ISpVoice` on a worker thread; POSIX no-op stub). Narrates focused UI (a UiTree focus listener in `ui.abi` calls an installed announcer pointer), optional captions, and script `Announce`. It is not a UI Automation provider, so NVDA and Narrator do not see the widgets (risk 3).
10. **Persisted and exposed.** Settings screen gains three tabs (Accessibility, Captions and Speech, Input Assist) built from the *default settings model*, so any host gets them. Aver Node: category "Accessibility", 11 nodes. C#: `Accessibility`, `Captions`, `Speech`, `CameraFx`. Bus events `Access.Changed` and `Caption.Show`.

## 1. What exists today (verified against the tree, 2026-10-17)

| Piece | Where | State |
|---|---|---|
| Nothing named accessibility, subtitle, caption, colour-blind, screen reader, SAPI, head bob or camera shake | grep over `modules Runtime sandbox scripting docs` | One unrelated comment about colour-blind readers (`sandbox/src/GraphEditor.cpp:2408`). Greenfield. |
| Settings store: float/int/bool/string by key, atomic flush, unknown keys kept | `modules/settings/include/aver/settings/settings_abi.h`, `src/Settings.cpp:81-196` | Only managed code calls it (`scripts/game.allowlist:56-85`); opened by `HostBridge.ConfigureInput` (`HostBridge.cs:408-421`) at `<user data>\<Project>\Settings.ini` (`GameApp.cpp:1241`, `gameSettingsPath`) or `<project>\Saved\Settings.ini` in the editor (`SandboxPlay.cpp:340`). `set_f32` writes `std::to_string` (`Settings.cpp:161`). |
| Settings screen: tabs, Toggle/Slider/Choice rows, `setting:<key>` Command events, scrollable pages, window 780x640 | `modules/ui/include/aver/ui/UiScreens.hpp:15-45`, `UiScreens.cpp:68-85` (default model), `:118` (window), `:261-282` (`applyItem`, special-cases `graphics.uiScale`) | Graphics + Audio (+ Controls). `graphics.uiScale` slider 0.75 to 1.5. |
| C# wiring of that screen | `Aver.UI/SettingsScreen.cs:54-58` (`s_defaultKeys`), `:131` (`SetUserScale`); `Aver.Framework/UiGameSettings.cs:14-21` (`FrameworkSettingsStore`), `:105-125` (`LanguageSettingsStore`: a key answered by something other than Settings), `:127-140` (`Apply`) | Pattern for `AccessSettingsStore`. |
| User UI scale | `UiDpi::userScale` `UiLayout.hpp:14`, applied `UiLayout.cpp:27`; `aver_ui_set_dpi` `ui_widget_abi.h:104`, `UiWidgetAbi.cpp:130-137`; `UiSystem.SetUserScale` `Widgets.cs:342` | Works. No text-only scale. |
| Font px | `UiTree::fontPx` = `dp * scale_` (`UiTreeLayout.cpp:45-48`) | One choke point for text scale. Fonts are baked bitmaps (`UiFont.hpp`), so large scales soften until the UI-text feature lands. |
| `contrast` theme (black panels, white borders, yellow accent) and `aver_ui_set_theme` | `UiStyle.cpp:141-146`, `UiStyle.hpp:46`; `ui_widget_abi.h:101`, `UiWidgetAbi.cpp:128` | Exists, nothing exposes it as a player option, and per-widget `bgColor/textColor/accentColor` override it (`UiTreeDraw.cpp:101-115`). |
| Focus events | `UiEventType::FocusGained` raised from `setFocusInternal` (`UiTreeInput.cpp:119-125`); `UiTree::addListener` (non-destructive, `UiTree.hpp:222`) | The queue is drained by C# (`UiSystem.Pump`), so narration must listen, not poll. |
| Widget property table (file, ABI, inspector from one table) | `UiProps.cpp:280-315` | Add `accessName` here. |
| Game UI hosts | `GameApp.cpp:2062-2086` (begin frame, HUD, `uiFeedInput`, `aver_ui_widgets_frame`), `:2220-2224` (`gameUi_->submit`); `SandboxApp.cpp:2204`, `:771-773` | One insertion point per host. |
| UI GPU pass: draws after the tonemap onto the backbuffer, premultiplied blend, `b3` = 4 root dwords | `UiRenderer.cpp:99-111,205`, `shaders/ui.hlsl:9-12,41-44`, `render.ui/README.md` "Where it runs in the frame" | HUD is not in the post chain. Vulkan push block = 128 B object block + b3 (`VulkanResourceFactory.cpp:556-575`, `VulkanCommon.hpp:748`). |
| Post chain: `PostSettings` (172 B asserted) with motion blur, DOF, chromatic aberration, grain, lens flare | `RHI.hpp:152-216,221`; `FrameConstants.hpp:129-181` (`PostCB` 272 B), `:217-262` (`fillPostLens`); both backends call it (`D3D12Device.cpp:5626`, `VulkanDevice.cpp:4520`); `post.hlsl:658-770` (`PSComposite`, ends `toGamma(averTonemap(...))`, LUT, grain) | Single shared pack, single shader. Vulkan has no G-buffer so DOF and motion blur are already off there (`RHI.hpp:203-207`). |
| Where hosts push post settings | `GameApp.cpp:1778-1780`, `SandboxApp.cpp:2605-2608` (blend then `setPostProcess`) | Where the player override is applied. |
| Shader includes through both backends' DXC handler | `DxcShaderInclude.hpp`; `#include "aver_visibility.hlsli"` in `render.sr/shaders/sr_neuraa.hlsl:37`; flat deploy dir (`cmake/AvModule.cmake:101-162`) | `#include` is a proven pattern. Post source is `colorHlsl() + post.hlsl` (`RHIShaders.cpp:168`). Engine gamma is `pow(x, 2.2)` (`color.hlsli:150-152`). |
| Audio ABI: every play funnels through six functions; a sound-handle to path table already exists | `audio_abi.h:60-137`, `AudioAbi.cpp:20` (`g_paths`), `:174-203` (play, play_at), `:247-297` (stream, music) | No observer. Silent when no device (`g_started` false returns 0 voices). `aver_audio_playing(voice)` is the liveness probe (`:205`). No seek position. |
| Sequencer audio and event tracks | `SequenceHost.hpp:36-62` (hooks call `aver_audio_load/play/_play_at`; `fireEvent` posts the bus event `name` with float args and one string arg), `SEQUENCER_TRACKS.md` route 0/1/2 | Audio tracks reach the observer unchanged. Event key `Caption.Show` + string arg reaches a bus subscriber with no sequencer edit. |
| Event bus (header-only, strings in payload) | `framework/EventBus.hpp:12-60` | Caption engine subscribes in-DLL. |
| Localization refs `@Table:Key=Default`, native resolver, epoch, gatherer | `LOCALIZATION.md`; `framework_loc_abi.h:49-56` (`aver_fw_loc_resolve`), `LocGather.hpp:26-33`, `aver_ui_set_text_resolver` installs (`GameApp.cpp:1175`, `SandboxProject.cpp:145`) | Caption text is a ref, resolved by the tree for free and by `aver_fw_loc_resolve` for speech. |
| Enhanced Input: triggers/modifiers, `aver_fw_action_update(dt)` once a frame, ABI minor 11 | `framework_abi.h:63,487-537`; `FrameworkAbi.cpp:1263-1281` (`ActionDef`), `:1459-1480` (`actionHasSmooth`, `actionValueNow`), `:1552-1603` (`actionEval`), `:1655-1669` (`aver_fw_action_update`); `ActionEngine.cpp:92-191` (`stepTrigger`), `:193-` (`combineTriggers`) | Plain queries are on-demand from raw state unless the action smooths (then cached). Time-based triggers: Hold, HoldRelease, Tap, Pulse, Combo. |
| Per-frame C# pump | `HostBridge.cs:1488-1498` (`DispTickAll` group 0 runs `UiSystem.Pump`, `ContentPump.Tick`, `GameplayAbilities.Update`, `EnhancedInput.PumpEvents`) | Where `Accessibility.Tick(dt)` goes. |
| Node tables: per-feature `XxxTable()` in its own file, `.Concat(...)` in one shared line; palette mirror `GraphNodeDefsXxx.hpp` | `GraphGameSystemNodeTable.cs:120-121`, `GraphSequenceNodeTable.cs`, `GraphNodeDefsSequencer.hpp`, include `GraphNodeDefs.hpp:116`, call `:1109`; tests `NodeRowKit` rows, `Program.cs:272-281` | Pattern for section 7. |
| Platform per-OS source lists, ole32 already linked | `platform/CMakeLists.txt:1-31,56` | Speech slots in beside `Win32Console.cpp` / `PosixConsole.cpp`. |
| Camera build and override points | `GameCamera.hpp` (`pushCamera` fixed 60 degree FOV, `drivePlayCamera`); `GameApp.cpp:2172-2183` (pawn camera then sequencer camera override); `SandboxApp.cpp:2398` | Insertion point for `CameraFx`. No roll in the camera model (forward + world-up). |

Missing, to be built: everything in sections 2 to 8.

## 2. Design

### 2.1 Options: `AccessKeys.hpp` and the native state

Header-only, `namespace aver::fw::access`. Pure functions, no I/O:

```cpp
enum class AccessKind : u8 { Toggle, Slider, Choice };
struct AccessKey { const char* key; AccessKind kind; f32 lo, hi, step, def; const char* label; const char* tab; const char* choices; };
extern const AccessKey kAccessKeys[];  usize accessKeyCount();   // constexpr table in the header
const AccessKey* accessFind(std::string_view key);
f32 accessClamp(const AccessKey&, f32 v);        // clamp, snap to step, Toggle -> 0/1, Choice -> integer
usize accessParseIni(std::string_view text, AccessState&);   // key=value lines; strtof; bool words true/false/yes/no/on/off; unknown keys ignored; returns count read
```

The table (default in brackets; labels are loc refs `@UI:...=English`, tab refs `@UI:settings.tab.*`):

| Key | Kind | Range | Tab |
|---|---|---|---|
| `graphics.uiScale` [1] | slider | 0.75 to 2.0 step 0.05 | Accessibility (moved from Graphics) |
| `access.ui.textScale` [1] | slider | 0.75 to 2.0 step 0.05 | Accessibility |
| `access.ui.highContrast` [0] | toggle | | Accessibility |
| `access.colour.mode` [0] | choice Off/Protanopia/Deuteranopia/Tritanopia | 0 to 3 | Accessibility |
| `access.colour.strength` [1] | slider | 0 to 1 step 0.05 | Accessibility |
| `access.colour.simulate` [0] | toggle, **not in the screen** (developer preview, editor and C#) | | |
| `access.motion.reduce` [0] | toggle (master) | | Accessibility |
| `access.motion.shake` [1] | slider | 0 to 1 | Accessibility |
| `access.motion.bob` [1] | slider | 0 to 1 | Accessibility |
| `access.motion.blur` [1] | toggle (1 = allowed) | | Accessibility |
| `access.motion.lens` [1] | toggle (chromatic aberration, grain, lens flare allowed) | | Accessibility |
| `access.captions.mode` [1] | choice Off/Subtitles/Captions | 0 to 2 | Captions and Speech |
| `access.captions.size` [1] | choice Small/Medium/Large/Extra large = 0.8/1.0/1.4/1.9 | | Captions and Speech |
| `access.captions.bgOpacity` [0.6] | slider | 0 to 1 step 0.05 | Captions and Speech |
| `access.captions.position` [0] | choice Bottom/Top | | Captions and Speech |
| `access.captions.lines` [3] | slider | 1 to 4 step 1 | Captions and Speech |
| `access.captions.speaker` [1] | toggle (speaker names) | | Captions and Speech |
| `access.captions.colours` [1] | toggle (speaker colours) | | Captions and Speech |
| `access.captions.direction` [0] | toggle (< > v hints) | | Captions and Speech |
| `access.speech.enabled` [OS: `osScreenReaderActive()` at first start if the key is absent, never persisted by that default] | toggle | | Captions and Speech |
| `access.speech.ui` [1] / `access.speech.captions` [0] | toggles | | Captions and Speech |
| `access.speech.rate` [0] | slider | -10 to 10 step 1 (SAPI native range) | Captions and Speech |
| `access.speech.volume` [1] | slider | 0 to 1 step 0.05 | Captions and Speech |
| `access.input.holdScale` [1] | slider (hold time multiplier, shorter holds) | 0.25 to 1 step 0.05 | Input Assist |
| `access.input.windowScale` [1] | slider (tap and combo window multiplier, longer) | 1 to 4 step 0.25 | Input Assist |
| `access.input.toggle.<Action>` [0] | dynamic toggle per registered Digital action (not in the static table; prefix handled by `accessFind`) | | Input Assist (added by C#) |

`AccessState` is a flat `std::array<f32, N>` plus `std::unordered_map<std::string, bool> actionToggle`, `revision` (u32, bumps on any accepted change) and a dirty-key list. The ABI (section 2.8) is the only writer. `access.motion.reduce` is a *gate*, not a preset: effective shake = `reduce ? 0 : shake`, same for bob, blur and lens, so un-ticking restores the sliders.

### 2.2 Captions

**Format `.occaptions`** (parser/writer in `Aver.Formats`, `OcCaptions.hpp/.cpp`, same OC text grammar as `.ocstrings`: `#` comments, quoted strings with `\"` `\\`, unknown records kept and re-emitted by a text-merging writer like `writeOcinput`):

```
OCCAPTIONS 1
SPEAKER guard name "@Cap:spk.guard=Guard" colour #7DD3FC
CUE intro_01 sound "Audio/VO/intro_01.ocaudio" kind dialogue speaker guard priority 8
  LINE 0.0 2.4 "@Cap:intro_01_a=Welcome to the station."
  LINE 2.6 0   "@Cap:intro_01_b=Keep your voice down."        # dur 0 = until the voice ends
CUE boom sound "Audio/SFX/explosion_*.ocaudio" kind sound priority 5 radius 4000 cooldown 1.5 hold 1.8 tag "@Cap:fx.explosion=[explosion]"
```
- `CUE <id>` keywords: `sound <pattern>` (repeatable; content-relative, `/` separators, case-insensitive, `*` and `?` inside a segment; matched against the tail of the played path), `kind dialogue|sound|music` (default `sound`), `speaker <id>`, `priority <int>` (default 0), `radius <cm>` (0 = always; positional voices farther than this from the listener are skipped), `cooldown <sec>` (default 1.0, per cue id), `hold <sec>` (minimum on-screen time, default 1.5 for sound and music, 0.8 for dialogue), `tag <text>` (shorthand: one LINE `0 0 <text>`).
- `LINE <start> <dur> <text> [speaker <id>]`: seconds from the cue start; `dur 0` = until the voice ends or `hold`, whichever is later. A cue with no LINE and no `tag` is an error (skipped with a diagnostic naming the line).
- Header missing or version newer: fails. Any other malformed record: skipped with a diagnostic (a typo never blanks a file; the `.ocstrings` rule).
- Files: every `Content/Captions/*.occaptions`, merged in name order; a later duplicate cue id replaces the earlier one and logs once. Hot reload via the same file-watch route as localization (`onContentFilesChanged`, `GameLocalization.cpp:140-154`).
- `LocGather.hpp` gains `gatherOccaptions(text, path, out)` so Gather and the missing-string report see caption refs.

**Runtime `CaptionEngine`** (header-only, `framework/CaptionEngine.hpp`, no I/O, no audio, injected `std::function<bool(i32 voice)> probe` and `std::function<bool(raw,out)> resolve`):
- `load(const OcCaptionData&)`, `clear()`, `unload(cueIds)`.
- `onSound(const SoundStart& s)`: `{path, voice, bus, positional, pos, looping}`. Picks the matching cue: exact path first, then patterns, highest `priority`, ties by file order. Drops it if the mode excludes its kind (mode 1 Subtitles = `dialogue` only; mode 2 Captions = all; 0 = nothing), if positional and beyond `radius`, or inside `cooldown`. Creates an `Active` (cue, voice, `t = 0`, position, positional).
- `show(cueId, optional pos)`: same with `voice = 0` (script, graph, bus event); `showText(speakerRef, textRef, seconds, rgba, kind)`: an ad-hoc line with no cue.
- `update(dt, listener{pos, forward, right})`: `dt` is the **unscaled** frame time (captions track audio, which keeps playing while the game is paused). Each active advances; if `voice != 0` and `!probe(voice)` it is marked ended at its current `t`. A line is visible when `start <= t < start + dur` (`dur 0`: until the voice ended plus 0.35 s, never less than `hold`). A cue with no voice ends at its last line end or `hold`. Dialogue cut short by a stopped voice keeps its current line for at least 0.8 s (min dwell). Expired actives are removed.
- `lines(out)`: visible lines sorted by priority desc then start; capped at `access.captions.lines` (the lowest priority or oldest is evicted; a dialogue line is never evicted by a sound caption). Each output line: `{speakerRef, textRef, rgba, kind, dir, alpha}`. Speaker shown only if `speaker` is on and the previous visible line is by someone else; colour is the speaker colour if `colours` is on, white otherwise. `dir` (only if `direction` is on and the source is positional): azimuth of the source from the listener in the xy plane (engine axes +X forward, +Y right); `-1` left when below -35 degrees, `+1` right above +35, `2` behind beyond 135 degrees, else `0`. `alpha` fades over the last 0.25 s (1 when reduce motion is on).
- Captions never mutate audio and never allocate on the audio thread: `onSound` runs on the game thread because the observer is called from the `aver_audio_*` entry points, which are game-thread calls by the ABI contract (`AUDIO.md` section 3).

**Audio hook** (`audio_abi.h`, `AudioAbi.cpp`; additive, optional, null by default):
```c
typedef void (*AverAudioPlayObserver)(void* user, const char* utf8Path /*null for PCM*/, int32_t voice, int32_t bus,
                                      int32_t positional, float x, float y, float z, int32_t looping);
AVER_AUDIO_API void aver_audio_set_play_observer(AverAudioPlayObserver fn, void* user);   /* null clears */
```
Fired after a successful start (`voice != 0`) in `aver_audio_play`, `_play_at`, `_stream_play`, `_stream_play_at`, `_music_play` (path from `g_paths` or the stream path). Synthesised sounds (`aver_audio_load_pcm`, `.ocsnd`) have no path and caption through `Caption.Show` / `Captions.Show(cue)`. No device means no voices means no observer calls; manual `show` still works.

**Wiring** (hosts, section 4): `aver_audio_set_play_observer(&aver_fw_access_audio_observer, nullptr)` and `aver_fw_access_set_voice_probe(&aver_audio_playing_shim)` (a 2-line adapter because the ABI returns `int32_t` and the probe wants `int32_t` in, `int32_t` out: `aver_audio_playing` fits directly). The framework subscribes in `aver_fw_access_init` to bus event `Caption.Show` (string arg = cue id, optional float args x y z) and `Caption.Stop`.

**Rendering** (`Aver.UI`, `UiAccess.hpp`, `UiCaptionView`): `struct UiCaptionLine { std::string speaker, text; u32 rgba; i32 dir; f32 alpha; }`, `struct UiCaptionStyle { f32 sizeDp; f32 bgOpacity; bool top; bool highContrast; }`, `void UiCaptionView::update(UiTree&, const std::vector<UiCaptionLine>&, const UiCaptionStyle&)`. It owns one root named `Aver.Captions` (`inputMode Passive`, `hit Never`, `layer Overlay`, anchored to the bottom or top centre, 70 percent width, bottom margin 6 percent), a VStack of one Panel row per line (`bgColor` = black at `bgOpacity`, text `wrap`, `fontSize = 24 * sizeDp-scale`, `textColor` = line colour; a leading `<` or `>` or `v` for `dir`). Under high contrast the background opacity floors at 0.85 and text is white. Widget text holds the raw refs, so the tree's resolver localizes them and a culture switch re-reads them. Rows are recycled (no per-frame widget churn when the line set is unchanged).

### 2.3 Colour-vision modes (D3D12 and Vulkan)

**Maths** (`aver_colour_vision.hlsli`, CPU twin `rhi/ColourVision.hpp`; matrices are Machado, Oliveira, Fernandes, IEEE TVCG 15(6), 2009, severity 1.0, linear RGB, row-vector form `out = M * in`):

| Mode | M (rows) |
|---|---|
| 1 Protanopia | `[0.152286 1.052583 -0.204868]` `[0.114503 0.786281 0.099216]` `[-0.003882 -0.048116 1.051998]` |
| 2 Deuteranopia | `[0.367322 0.860646 -0.227968]` `[0.280085 0.672501 0.047413]` `[-0.011820 0.042940 0.968881]` |
| 3 Tritanopia | `[1.255528 -0.076749 -0.178779]` `[-0.078411 0.930809 0.147602]` `[0.004733 0.691367 0.303900]` |

Every row sums to 1 (grey is preserved; pinned by a test, which also catches a mistyped digit). Correction (daltonize, Fidaner, Lin and Ashburner 2005): `err = lin - M*lin`; protan and deutan shift `S = [0 0 0; 0.7 1 0; 0.7 0 1]` (red error into green and blue), tritan `S = [1 0 0.7; 0 1 0.7; 0 0 0]`; `corrected = lin + S*err`. `strength` lerps from `lin`. The implementer compares the matrices with the paper and the `colour` library's `machado2009` module before landing; the test pins grey-axis and clamp behaviour, not the paper digits beyond the row sums.

```hlsl
float3 averColourVision(float3 disp, float packed)   // disp: display-space [0,1], packed = mode + 4*simulate + 8*round(strength*100)
{
    int p = (int)(packed + 0.5); int mode = p & 3; bool sim = (p & 4) != 0; float k = (float)(p >> 3) * 0.01;
    float3 lin = pow(saturate(disp), 2.2);                      // matches color.hlsli toGamma/srgbToLin
    float3 s = mul(averCvMatrix(mode), lin);
    float3 o = sim ? s : lin + mul(averCvShift(mode), lin - s);
    return pow(saturate(lerp(lin, o, k)), 1.0 / 2.2);
}
```
The file defines its own `pow 2.2` constants so it needs neither `color.hlsli` nor the prelude (it is included by two differently-composed shaders). `mode 0` never reaches it (callers branch on `packed > 0.5`).

**Scene**: `PSComposite` calls it on `outc` after the LUT and before grain (`post.hlsl:758-768`), guarded by `if (gPostAccess.x > 0.5)`. New cbuffer row `float4 gPostAccess; // x packed colour vision, yzw unused` at the end of `AverPost`.

**HUD**: `UiPS` becomes `float4 s = i.col * tex; if (gUiCv > 0.5 && s.a > 0.0) s.rgb = averColourVision(s.rgb / s.a, gUiCv) * s.a; return s;` with `cbuffer UiConstants { float4 gUiProj; float gUiCv; }`. `UiRenderer` pipeline `constantDwords[3] = 5` (was 4; Vulkan push block 144 B to 148 B, already above the 128 B guaranteed minimum, so no new class of risk) and `setColourVision(u32 packed)` stored and passed with `setConstants(3, {proj, cv}, 5)`.

**C++ side**: `PostSettings` += `u32 colourVision = 0; f32 colourVisionStrength = 1;` (`sizeof` 172 to 180 given the 4-byte `TextureHandle` the current 172 implies; update the `static_assert` and `postSettingsEqual`); `PostCB` += `f32 access[4]` (272 to 288; D3D12's 256-rounded ring slot is 512 either way, `D3D12Device.cpp:401`); `fillPostLens` writes `cb.access[0] = packColourVision(p)` (`rhi/ColourVision.hpp`). **No backend file changes**: both already call `fillPostLens`. The Vulkan HLSL is the same file.

**Player override, not a level setting**: `GameAccessibility.hpp::applyAccessibilityToPost(PostSettings&, const AccessSnapshot&)` runs on the blended settings right before `setPostProcess` in both hosts. It sets `colourVision`, and for reduce motion zeroes `motionBlur` (when `blur` is off), `chromaticAberration`, `grain`, `lensFlare` (when `lens` is off). A level's post volumes cannot undo it because it runs after the blend. Not applied to the PT reference view or to frame-generation interpolated frames beyond what the composite already covers (risk 4).

**Editor**: a "Colour vision preview" combo (Off, Protanopia, Deuteranopia, Tritanopia, each Simulation or Correction) in the viewport menu writes `access.colour.*` only into editor prefs (never the project's `Settings.ini`) and goes through the same `applyAccessibilityToPost` on the scene (not the editor chrome, which draws after the tonemap).

### 2.4 UI scale, text scale, high contrast

- `UiAccess { f32 textScale = 1; bool highContrast = false; }` in `UiAccess.hpp`; `UiTree::setAccess/access()` (members in `UiTree.hpp`, default = today's behaviour).
- `fontPx` = `dp * scale_ * access_.textScale`. Row heights, padding and widths stay on `scale_` (text scale is text only; UI scale is the whole layout).
- High contrast: `styleOf` resolves against a lazily built `uiBuiltinTheme("contrast")` instead of `theme_`; `UiTreeDraw` ignores `bgColor/textColor/accentColor` (images and tints are untouched), uses the contrast theme's `focusColor`, `dim`, and `focusWidth * 2`, and the scrollbar uses full white. The game's own theme choice (`aver_ui_set_theme`) is kept and restored when the option turns off.
- `graphics.uiScale` moves to the Accessibility tab (same key, so saved values keep working) and its range becomes 0.75 to 2.0 (`UiScreens.cpp:83`).
- Settings window reflows so 2.0 scales stay usable: the window is anchored 5 to 95 percent of the viewport, `maxWidth 980`, `maxHeight 720` (design px), row labels `wrap = true` (replaces the fixed 780x640 at `UiScreens.cpp:118`).
- `accessName` (string, `UiProps.cpp` row, group "Accessibility", all kinds): the spoken label; empty = derive (2.6).
- Host: `aver_ui_set_access(float textScale, int32_t highContrast)` (new, `ui_widget_abi.h`, `UiWidgetAbi.cpp`), called by `GameAccessibility` when the revision changes; `graphics.uiScale` goes through the existing `aver_ui_set_dpi(-1, 0, 0, userScale)`.

### 2.5 Reduce motion and camera feel

`framework/CameraFx.hpp` (header-only, deterministic, no allocation):
- Shake: `trauma` in [0,1] decaying at 1.5/s; `shake = trauma^2`; yaw, pitch and a small xyz offset from seeded 1-D value noise (sum of two octaves, 17 Hz), amplitudes 1.5 degrees yaw, 1.2 degrees pitch, 1.5 cm offset at full shake. `addShake(trauma)` saturates at 1. (Squirrel Eiserloh's trauma model from his GDC 2016 talk; a technique, not a codebase.)
- Bob: `setBob(amount01, speedCmPerSec)`; phase advances `speed/320` cycles per second; vertical `sin(2*pi*phase) * 2.2 cm * amount`, lateral `sin(pi*phase) * 1.1 cm * amount`. A character script calls it each frame from its ground speed; nothing in the engine calls it by itself.
- `applyCameraFx(Vec3& pos, f32& yaw, f32& pitch, f32 dt, f32 shakeScale, f32 bobScale)`: both scales come from `reduce ? 0 : slider`. With both zero the function returns before touching the state (so a game that never uses it is bit-identical).
- Host call: right after the pawn and sequencer cameras are final and before `pushCamera` (`GameApp.cpp:2172-2183`, `SandboxApp.cpp:2398` play path). The editor's free-fly camera is untouched.
- Photosensitivity (flash limiting, strobe detection) is **out of v1**; bloom, lens flare and exposure adaption remain governed by level authoring.

### 2.6 Input assist

- **Toggle instead of hold**: per action, key `access.input.toggle.<Action>`. For a Digital action, a rising actuation of the *raw* summed value flips a latch; the action's output value `V` becomes (latch ? last-held `V` : 0). Axis actions ignore it. A game can mark actions exempt (`Accessibility.Input.SetToggleAllowed("Pause", false)`) and the settings page lists only registered Digital actions that are allowed. Latch resets on `aver_fw_action_reset_state`, on context change (`aver_fw_action_clear_bindings`) and when the pawn changes (hosts call reset at Play start already).
- **Timing assistance (global, applied per trigger kind)**: `holdScale` multiplies `Hold` and `HoldRelease` `time`; `windowScale` multiplies `Tap` max time and `Combo` window. `Pulse` (auto-repeat) and `Chord` are untouched. A per-action override is deliberately out of v1 (the global scales are what XAG asks for); the hook takes a `Timing` so a per-action table is a later addition.
- **Engine hooks (small, additive, in the shared `FrameworkAbi.cpp`/`ActionEngine`)**:
  - `ActionEngine.hpp`: `struct Timing { float hold = 1, window = 1; }`; `stepTrigger` and `combineTriggers` gain a trailing `const Timing* t = nullptr` (null = today's behaviour, existing callers and tests compile unchanged); Hold/HoldRelease compare `rt.t >= spec.time * t->hold`, Tap compares `rt.t > spec.time * t->window`, Combo `rt.t > spec.time * t->window`.
  - `FrameworkAbi.cpp`: `actionEval` passes `accessTiming()` to both `combineTriggers` calls (`:1578`, `:1586`), and after computing `V` applies `V = fwaccess::filterToggle(handle, name, V)`; `actionHasSmooth` (`:1459`) also returns true when `fwaccess::toggleOn(action)` so plain `held/pressed/released` read the cached values (the existing smooth-action route, `:1473-1480`).
  - `AccessAssist.hpp` (header-only, in `framework/`): the latch state, the exempt set and the `Timing`, so `FrameworkAbi.cpp` adds only three call sites and one include.
- Assisted: named actions (`.ocinput`, `InputAction`, graph `InputAction` nodes). Not assisted: raw `Input.IsKeyDown`, gamepad reads outside the action layer.

### 2.7 Speech (OS TTS) and narration

**Platform** (`Aver.Platform`, `aver/platform/Speech.hpp`):
```cpp
class SpeechSynth { public: virtual ~SpeechSynth() = default;
  virtual bool available() const = 0;                       // a usable voice exists
  virtual void speak(std::string_view utf8, bool interrupt) = 0;   // non-blocking enqueue
  virtual void stop() = 0;  virtual bool speaking() const = 0;
  virtual void setRate(int sapiRate) = 0;  virtual void setVolume(float v01) = 0; };
std::unique_ptr<SpeechSynth> createSpeechSynth();           // never null
bool osScreenReaderActive();                                 // Win: SystemParametersInfoW(SPI_GETSCREENREADER); else false
```
- Windows (`src/win32/Win32Speech.cpp`): one worker thread (`CoInitializeEx` multithreaded), owns an `ISpVoice` (`CoCreateInstance(CLSID_SpVoice)`, created lazily on the first `speak` so startup never waits on COM), a mutex and condition-variable command queue, `Speak(wide, SPF_ASYNC | SPF_IS_NOT_XML | (interrupt ? SPF_PURGEBEFORESPEAK : 0))`, `SetRate`, `SetVolume(0..100)`, `speaking()` from an atomic updated by `GetStatus`. UTF-8 to wide with `MultiByteToWideChar`. Links `sapi` (Windows SDK) beside the existing `ole32`.
- POSIX (`src/posix/PosixSpeech.cpp`): a stub, `available() == false`, every call a no-op, `osScreenReaderActive() == false`. macOS `AVSpeechSynthesizer` (needs `.mm`) and Linux speech-dispatcher are follow-ups behind the same interface (open question 3).

**Framework** (`SpeechQueue.hpp`, header-only, injected `SpeechSynth&` and a resolver): `announce(raw, interrupt, source)` resolves a localization ref, trims, drops text identical to the last spoken within 1 s, interrupts on `interrupt` (focus changes interrupt; captions and script announcements queue, max 3 pending, oldest dropped), applies per-source gating (`ui`, `captions`, `script` against `access.speech.*`), and pushes rate and volume on a settings revision change. Master gate `access.speech.enabled`. `aver_fw_speech_*` ABI below.

**UI narration**: `ui.abi` registers a `UiTree` listener at first frame; on `FocusGained` it computes `uiAccessibleText(tree, widget)` and, if an announcer is installed, calls it with `interrupt = 1`. Hosts install `aver_ui_set_announcer(&aver_fw_speech_announce_ui)` beside the text-resolver install (`GameApp.cpp:1175`, `SandboxProject.cpp:145`). `uiAccessibleText` (pure, in `Aver.UI`, tested headless):
- name: `accessName` (resolved via `shown`); else the widget's own `text` for Button/Toggle/KeyBind; for Slider/Choice/List/TextInput the nearest preceding `Text` sibling (the settings rows are `Text` + control in one HStack, `UiScreens.cpp:155-175`), else `placeholder`;
- role, state and value parts as loc refs with English defaults so they localize: `@Access:role.button=button`, `role.checkbox` + `state.checked|unchecked`, `role.slider` + the value (`showValue` number, else percent of range), `role.choice` + "item, N of M", `role.keybind` + the bound key text, `role.edit` (never reads a `masked` field's content), `role.list` + selected row;
- joined with ", ". Disabled widgets append `@Access:state.disabled=unavailable`.

Captions narration: when `access.speech.captions` is on, the host's caption frame announces each *newly visible* line (`speaker: text`, resolved) with `interrupt = 0`.

### 2.8 ABI: `framework_access_abi.h` (+ `AccessAbi.cpp`), version 1

Conventions of `framework_loc_abi.h`: `int32_t/float/const char*` only, UTF-8, single game thread, returned strings valid until the next call that returns a string, own `AVER_FW_ACCESS_ABI_VERSION`, `AVER_FW_ABI_VERSION_MINOR` untouched.
```c
#define AVER_FW_ACCESS_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_access_abi_version(void);

/* options */
AVER_FW_ABI int32_t     aver_fw_access_load_ini(const char* utf8Text);         /* keys applied, defaults for the rest; count read */
AVER_FW_ABI int32_t     aver_fw_access_key_count(void);
AVER_FW_ABI const char* aver_fw_access_key_at(int32_t i);
/* out[0..5] = kind(0 toggle,1 slider,2 choice), min, max, step, default, current. 0 for an unknown key. */
AVER_FW_ABI int32_t     aver_fw_access_key_info(const char* key, float* out6);
AVER_FW_ABI const char* aver_fw_access_key_choices(const char* key);           /* newline-separated loc refs, "" if none */
AVER_FW_ABI float       aver_fw_access_get(const char* key);                   /* default for an unknown key... */
AVER_FW_ABI int32_t     aver_fw_access_set(const char* key, float value);      /* 1 accepted (clamped), 0 unknown key; marks dirty, bumps revision, posts "Access.Changed" (string key, float value) */
AVER_FW_ABI void        aver_fw_access_reset_defaults(void);
AVER_FW_ABI int32_t     aver_fw_access_revision(void);
AVER_FW_ABI int32_t     aver_fw_access_take_dirty(int32_t i, char* keyOut, int32_t cap, float* valueOut);  /* drain: 1 per call until 0 */
AVER_FW_ABI void        aver_fw_access_set_toggle_allowed(const char* utf8Action, int32_t allowed);
AVER_FW_ABI int32_t     aver_fw_access_effective(float* out8);   /* snapshot for hosts: colourVisionPacked, uiScale, textScale, highContrast,
                                                                    shake, bob, motionBlurAllowed, lensAllowed (reduce already applied) */
/* captions */
AVER_FW_ABI int32_t     aver_fw_caption_load_dir(const char* utf8Dir);         /* *.occaptions, returns cue count, replaces */
AVER_FW_ABI int32_t     aver_fw_caption_reload_path(const char* utf8AbsPath);  /* 1 if it was a caption file (hot reload) */
AVER_FW_ABI void        aver_fw_caption_clear(void);
AVER_FW_ABI int32_t     aver_fw_caption_show(const char* cue, int32_t positional, float x, float y, float z);   /* 1 shown */
AVER_FW_ABI int32_t     aver_fw_caption_show_text(const char* speakerRef, const char* textRef, float seconds, uint32_t rgba, int32_t kind);
AVER_FW_ABI void        aver_fw_caption_stop(const char* cue);                 /* null = all */
AVER_FW_ABI void        aver_fw_access_audio_observer(void* user, const char* path, int32_t voice, int32_t bus, int32_t positional,
                                                      float x, float y, float z, int32_t looping);   /* AverAudioPlayObserver */
AVER_FW_ABI void        aver_fw_caption_set_voice_probe(int32_t (*alive)(int32_t voice));
AVER_FW_ABI void        aver_fw_caption_update(float dt, const float* listenerPos3, const float* listenerFwd3, const float* listenerRight3);
typedef struct AverFwCaptionLine { const char* speaker; const char* text; uint32_t rgba; int32_t kind; int32_t dir; float alpha; } AverFwCaptionLine;
AVER_FW_ABI int32_t     aver_fw_caption_line_count(void);
AVER_FW_ABI int32_t     aver_fw_caption_line_at(int32_t i, AverFwCaptionLine* out);   /* raw refs; valid until the next update */
AVER_FW_ABI int32_t     aver_fw_caption_new_line_at(int32_t i, AverFwCaptionLine* out); /* lines that appeared this update (for speech) */
/* speech */
AVER_FW_ABI int32_t     aver_fw_speech_available(void);
AVER_FW_ABI int32_t     aver_fw_speech_say(const char* utf8OrRef, int32_t interrupt);        /* source "script", gated by speech.enabled */
AVER_FW_ABI int32_t     aver_fw_speech_announce_ui(const char* utf8, int32_t interrupt);     /* AverUiAnnounceFn: source "ui" */
AVER_FW_ABI void        aver_fw_speech_stop(void);
AVER_FW_ABI int32_t     aver_fw_speech_speaking(void);
AVER_FW_ABI void        aver_fw_speech_tick(void);                                            /* applies rate/volume on revision change */
/* camera feel */
AVER_FW_ABI void        aver_fw_camera_shake_add(float trauma);
AVER_FW_ABI void        aver_fw_camera_bob_set(float amount01, float speedCmPerSec);
AVER_FW_ABI void        aver_fw_camera_fx_apply(float dt, float* inoutPos3, float* inoutYawPitchRad2);   /* uses the effective scales */
```
`aver_fw_access_effective` and the caption/speech/camera functions are what hosts call each frame; C# uses everything. No provider relay is needed (all state is in this DLL), unlike the anim/sequence ABIs.

### 2.9 Editor

- **Captions tab** (`CaptionEditor`, an `AssetEditor` for `.occaptions`, modelled on `InputSchemeEditor`: `SnapshotUndo<fmt::OcCaptionData>`, text-merging save, headless-compilable with `AVER_WITH_IMGUI` undefined): speakers table (id, name ref, colour picker), cue list (filter), cue details (id, sound patterns, kind, speaker, priority, radius, cooldown, hold), line table (start, dur, text with a resolved preview under it, per-line speaker), **Add cue from sound** (button and a Content Browser right-click on an audio asset), a pattern tester ("matches N sounds in this project"), and **Preview** (shows the cue in the Game UI preview and fires the real engine for the open Play session). Registered with `assetEditors_.registerFactory(&editor::makeCaptionEditor)` (`SandboxApp.cpp:488` neighbourhood); Content Browser create entry and type label `{".occaptions", "Captions"}` (`SandboxContentBrowser.cpp:2043` neighbourhood).
- **Viewport "Accessibility" menu**: colour vision preview (2.3). Reduce-motion and text-scale preview are Play-time only (the player's settings page).
- **Sequencer**: nothing changes in the Sequencer window; docs show an Event key named `Caption.Show` with arg `intro_01` and a Voice sound track whose file has a cue. Gather (Localization) picks up caption refs.
- **PIE** reads the project's `Saved/Settings.ini` accessibility keys (the editor passes that path to `ConfigureInput`, `SandboxPlay.cpp:340`), unlike the localization preview that deliberately ignores saved culture, because testing the player's own accessibility choices is the point.

### 2.10 C# API and Aver Node

**C#** (`scripting/csharp/Aver.Framework`; new files only, DllImports in `AccessNative.cs` as `internal static class FwAccess`, `Lib = "Aver.Framework"`, the `SequenceNative.cs` pattern; every call is wrapped in `try/catch DllNotFoundException` like `Audio`):
```csharp
public enum ColourVisionMode { Off = 0, Protanopia = 1, Deuteranopia = 2, Tritanopia = 3 }
public enum CaptionMode { Off = 0, Subtitles = 1, Captions = 2 }
public enum CaptionKind { Dialogue = 0, Sound = 1, Music = 2 }

public static class Accessibility
{
    public static float Get(string key, float fallback = 0f);          // any key in the table
    public static bool  Set(string key, float value);                  // clamped; false for an unknown key
    public static void  ResetToDefaults();
    public static event Action<string, float>? Changed;                // raised from Tick, once per accepted change
    public static void  Tick(float dt);                                // HostBridge group 0: drains dirty keys into Settings (SetFloat, Flush after 0.5 s idle), raises Changed, runs speech tick
    // typed views (get/set over the keys)
    public static float UiScale { get; set; }            public static float TextScale { get; set; }
    public static bool  HighContrast { get; set; }       public static ColourVisionMode ColourVision { get; set; }
    public static float ColourStrength { get; set; }     public static bool SimulateColourVision { get; set; }   // developer preview
    public static bool  ReduceMotion { get; set; }       public static float ShakeScale { get; set; }  public static float BobScale { get; set; }
    public static bool  MotionBlurAllowed { get; set; }  public static bool LensEffectsAllowed { get; set; }
    public static class Input
    {
        public static bool  IsToggleEnabled(string action);   public static void SetToggle(string action, bool on);
        public static void  SetToggleAllowed(string action, bool allowed);        // false hides it from the settings page and ignores a saved toggle
        public static float HoldScale { get; set; }            public static float WindowScale { get; set; }
    }
}
public static class Captions
{
    public static CaptionMode Mode { get; set; }  public static int SizePreset { get; set; }  public static float BackgroundOpacity { get; set; }
    public static bool TopOfScreen { get; set; }  public static int MaxLines { get; set; }
    public static bool ShowSpeakerNames { get; set; }  public static bool SpeakerColours { get; set; }  public static bool DirectionHints { get; set; }
    public static int  Load(string contentRelativeDir = "Captions");           // cue count; the host already loaded the default folder
    public static bool Show(string cue);                                       // false: no such cue or mode excludes it
    public static bool ShowAt(string cue, Vec3 position);                      // positional: radius and direction apply
    public static bool ShowText(string speaker, string text, float seconds = 3f, CaptionKind kind = CaptionKind.Dialogue, uint rgba = 0xFFFFFFFF);   // refs allowed
    public static void Stop(string? cue = null);
}
public static class Speech
{
    public static bool Available { get; }  public static bool Enabled { get; set; }  public static bool NarrateUi { get; set; }
    public static bool NarrateCaptions { get; set; }  public static int Rate { get; set; }  public static float Volume { get; set; }
    public static bool Announce(string textOrRef, bool interrupt = false);    // false when disabled or unavailable
    public static void Stop();  public static bool IsSpeaking { get; }
}
public static class CameraFx
{
    public static void AddShake(float trauma);                                  // 0..1, saturating
    public static void SetBob(float amount01, float speedCmPerSec);             // call each frame; 0 stops it
}
internal static class AccessibilityGraphInterop   // primitive signatures for GraphCompiler, strings empty = "not given"
{
    internal static bool SetOptionForGraph(string key, float value);
    internal static bool GetOptionForGraph(string key, out float value);
    internal static bool ShowCaptionForGraph(string cue, float x, float y, float z, bool positional);
    internal static bool ShowCaptionTextForGraph(string speaker, string text, float seconds, int kind);
    internal static bool StopCaptionForGraph(string cue);
    internal static bool AnnounceForGraph(string text, bool interrupt);
    internal static bool StopSpeakingForGraph();
    internal static bool IsSpeakingForGraph();
    internal static bool AddCameraShakeForGraph(float trauma);
    internal static bool SetCameraBobForGraph(float amount, float speed);
    internal static bool SetActionToggleAllowedForGraph(string action, bool allowed);
}
```
`AccessSettingsStore : ISettingsStore` answers every `access.*` key and `graphics.uiScale` from `Accessibility` (the `LanguageSettingsStore` arrangement, `UiGameSettings.cs:105-125`); `UiGameSettings.Create` uses it, and appends one Toggle per registered Digital action to the Input Assist tab (`aver_fw_action_count/name/value_type`, skipping exempt ones). `Accessibility.Changed` is also posted as the bus event `Access.Changed` (string key, float value), so an `AN_OnEvent` entry named `Access.Changed` reacts without a node. Captions raised by a script are the same engine as audio-triggered ones.

**Aver Node** (category `"Accessibility"`; rows in the new `GraphAccessibilityNodeTable.cs` as `AccessibilityTable()`, palette mirror `GraphNodeDefsAccessibility.hpp`; string inputs are NODE-line attributes, never pins; exec nodes are push-compiler only, the two pure reads work under either, as for the sequencer rows):

| Node | Kind | Pins (besides exec/then) | Attrs | Wraps |
|---|---|---|---|---|
| `SetAccessibilityOption` | exec | in `value:float`; out `success:bool` | `key` | `SetOptionForGraph` |
| `GetAccessibilityOption` | pure | out `value:float`, `found:bool` | `key` | `GetOptionForGraph` |
| `ShowCaption` | exec | in `x,y,z:float`, `positional:bool`; out `success` | `cue` | `ShowCaptionForGraph` |
| `ShowCaptionText` | exec | in `seconds:float=3`, `kind:int`; out `success` | `speaker?`, `text` | `ShowCaptionTextForGraph` |
| `StopCaption` | exec | out `success` | `cue?` (empty = all) | `StopCaptionForGraph` |
| `Announce` | exec | in `interrupt:bool`; out `success` | `text` | `AnnounceForGraph` |
| `StopSpeaking` | exec | out `success` | | `StopSpeakingForGraph` |
| `IsSpeaking` | pure | out `result:bool` | | `IsSpeakingForGraph` |
| `AddCameraShake` | exec | in `trauma:float=0.5`; out `success` | | `AddCameraShakeForGraph` |
| `SetCameraBob` | exec | in `amount:float`, `speed:float`; out `success` | | `SetCameraBobForGraph` |
| `SetActionToggleAllowed` | exec | in `allowed:bool=true`; out `success` | `action` | `SetActionToggleAllowedForGraph` |

Table rows (exact shape of `GraphSequenceNodeTable.cs`), e.g. `Reg("showcaption", GenericKind.Exec, typeof(AccessibilityGraphInterop), "ShowCaptionForGraph", "x:float,y:float,z:float,positional:bool", "success:bool", "@cue,p:x,p:y,p:z,p:positional", "success")` and `Reg("getaccessibilityoption", GenericKind.Pure, typeof(AccessibilityGraphInterop), "GetOptionForGraph", "", "value:float,found:bool", "@key,>value", "found")`. Option changes and caption lifecycle arrive as `AN_OnEvent` entries (`Access.Changed`); nothing else needs a node.

## 3. FILE OWNERSHIP

### 3.1 Created (only this feature touches these)

**Formats**
- `modules/formats/include/aver/formats/OcCaptions.hpp`, `modules/formats/src/OcCaptions.cpp`

**Framework (native, `modules/framework`)**
- `include/aver/framework/framework_access_abi.h`
- `include/aver/framework/AccessKeys.hpp`, `CaptionEngine.hpp`, `SpeechQueue.hpp`, `CameraFx.hpp`, `AccessAssist.hpp` (all header-only, no I/O)
- `src/AccessAbi.cpp`

**Platform**
- `modules/platform/include/aver/platform/Speech.hpp`, `src/win32/Win32Speech.cpp`, `src/posix/PosixSpeech.cpp`

**RHI**
- `modules/rhi/include/aver/rhi/ColourVision.hpp` (matrices, `packColourVision`, CPU reference)
- `modules/rhi/shaders/aver_colour_vision.hlsli`

**UI**
- `modules/ui/include/aver/ui/UiAccess.hpp`, `modules/ui/src/UiAccess.cpp` (`UiAccess`, `uiAccessibleText`), `modules/ui/src/UiCaptionView.cpp`

**Runtime**
- `Runtime/include/aver/game/GameAccessibility.hpp`, `Runtime/src/GameAccessibility.cpp` (`initAccessibility`, `accessibilityFrame`, `applyAccessibilityToPost`, `applyAccessibilityCamera`; shared by both hosts like `GameLocalization`)

**C# (`scripting/csharp`)**
- `Aver.Framework/AccessNative.cs` (`FwAccess`, DllImports), `Aver.Framework/Accessibility.cs` (`Accessibility`, `Captions`, `Speech`, `CameraFx`, `AccessSettingsStore`, `AccessibilityGraphInterop`)
- `Aver.Graph/GraphAccessibilityNodeTable.cs`
- `Aver.Graph.Tests/AccessibilityNodeTests.cs`

**Sandbox**
- `sandbox/src/GraphNodeDefsAccessibility.hpp`, `sandbox/src/CaptionEditor.hpp/.cpp`, `sandbox/src/AccessibilityPreview.hpp/.cpp` (viewport menu + editor pref)

**Docs**
- `docs/ACCESSIBILITY.md` (user guide: options table, captions format and authoring, colour modes, narration limits, input assist, API, nodes, events)

**Tests** (section 5)
- `tests/formats/src/OcCaptionsTest.cpp`
- `tests/framework_access/CMakeLists.txt` + `src/{AccessKeysTest,CaptionEngineTest,SpeechQueueTest,CameraFxTest,AccessAssistTest,FrameworkAccessAbiTest}.cpp`
- `tests/rhi/src/ColourVisionTest.cpp`, `tests/rhi/src/ColourVisionShaderTest.cpp`
- `tests/ui/src/UiAccessTest.cpp`, `tests/ui/src/UiCaptionViewTest.cpp`
- `tests/platform/src/SpeechTest.cpp`
- `tests/game/src/GameAccessibilityTest.cpp`
- `tests/editor/src/CaptionEditorTest.cpp`

### 3.2 Modified, hunks owned by this feature (nothing else should touch these hunks)

- `modules/framework/include/aver/framework/framework_abi.h`: **none** (the new header carries the version).
- `modules/framework/src/ActionEngine.hpp/.cpp`: `Timing` struct, trailing parameter on `stepTrigger`/`combineTriggers`, three scaled comparisons (`:92-191`).
- `modules/ui/src/UiTreeLayout.cpp` (`styleOf` `:43`, `fontPx` `:45-48`), `UiTreeDraw.cpp` (colour overrides `:101-115`, `theme_` reads `:60,:71,:289-290`).
- `modules/ui/src/UiProps.cpp`: one `sprop("accessName", ...)` row near `:308`; `UiWidget.hpp`: one `std::string accessName` in `UiWidgetProps`.
- `modules/ui/src/UiScreens.cpp`: default model gains the access items (`UiScreens.cpp:68-85`), the `graphics.uiScale` item moves tab and range, window reflow (`:118`), label `wrap`.
- `modules/render.ui/shaders/ui.hlsl`, `modules/render.ui/src/UiRenderer.cpp` and `include/aver/render/ui/UiRenderer.hpp` (`setColourVision`, 5 root dwords).
- `modules/rhi/shaders/post.hlsl` (cbuffer row, one include, one `PSComposite` branch), `modules/rhi/include/aver/rhi/FrameConstants.hpp` (`PostCB::access`, `fillPostLens` line, assert 288), `modules/rhi/include/aver/rhi/RHI.hpp` (`PostSettings` two fields, `postSettingsEqual`, assert 180).
- `modules/audio.abi/include/aver/audio/audio_abi.h` + `src/AudioAbi.cpp`: the observer typedef, setter and five call sites.
- `modules/formats/include/aver/formats/LocGather.hpp` + `src/LocGather.cpp`: `gatherOccaptions`.
- `docs/formats/FORMAT_SPECS.md` (new `.occaptions` subsection), `docs/GAME_UI.md` (settings tabs, `accessName`, high contrast, `UiCaptionView`), `docs/AUDIO.md` (observer), `docs/LOCALIZATION.md` (captions in Gather), `docs/AVER_NODE_NODES.md` (Accessibility section), `docs/SCRIPTING_API.md` (new numbered section), `docs/VISUAL_SCRIPTING.md` (one paragraph: accessibility nodes and events).

### 3.3 SHARED files (other wave-9 features may also touch; keep edits small and additive; the second to land keeps both blocks)

| File | This feature's edit |
|---|---|
| `modules/framework/CMakeLists.txt` | add `src/AccessAbi.cpp` to the source list |
| `modules/framework/src/FrameworkAbi.cpp` | three hook call sites in `actionEval` (`:1578,:1586` pass timing; after `V` computed apply toggle) and the `actionHasSmooth` extension (`:1459`); one `#include "AccessAssist.hpp"` |
| `modules/platform/CMakeLists.txt` | `src/win32/Win32Speech.cpp` / `src/posix/PosixSpeech.cpp` in the OS lists; `sapi` in the WIN32 `target_link_libraries` |
| `modules/formats/CMakeLists.txt` | `src/OcCaptions.cpp` |
| `modules/ui.abi/include/aver/ui/ui_widget_abi.h`, `src/UiWidgetAbi.cpp` | `aver_ui_set_access`, `aver_ui_captions_set` (+ `AverUiCaptionLine`), `aver_ui_set_announcer`, `aver_ui_widget_accessible_text`; the focus listener; the `UiCaptionView` instance beside `g_tree` |
| `modules/ui/include/aver/ui/UiTree.hpp` | `setAccess/access()` + two private members (the UI-text feature also edits this file: merge both) |
| `modules/ui/CMakeLists.txt` | `src/UiAccess.cpp`, `src/UiCaptionView.cpp` |
| `modules/ui/include/aver/ui/UiLayout.hpp` | **none** (`userScale` is reused) |
| `Runtime/include/aver/game/GameCamera.hpp` | append `applyCameraFx` wrapper (header-only, after `pushCamera`) |
| `Runtime/src/GameApp.cpp` / `GameApp.hpp` | init (after the localization block `:1170-1177`: `initAccessibility`, `aver_audio_set_play_observer`, `aver_ui_set_announcer`), frame (`:2062-2086` before `aver_ui_widgets_frame`: `accessibilityFrame`), post (`:1778` wrap), camera (after `:2183`), `gameUi_` colour vision per frame; hot reload (`onContentFilesChanged` caller) |
| `Runtime/CMakeLists.txt` | `src/GameAccessibility.cpp` |
| `sandbox/src/SandboxApp.cpp` / `.hpp` | same five insertions (`:2204`, `:2605-2608`, `:2398`, `:488` registration, `SandboxProject.cpp:145` announcer), plus the preview member |
| `sandbox/src/SandboxPlay.cpp` | none expected (PIE reuses `SandboxApp`'s frame); `SandboxProject.cpp` one announcer line |
| `sandbox/src/SandboxContentBrowser.cpp`, `SandboxPanels.cpp` | `.occaptions` create entry and label; viewport menu call |
| `sandbox/src/GraphNodeDefs.hpp` | `#include "GraphNodeDefsAccessibility.hpp"` (near `:116`) and `appendAccessibilityNodeDefs(t);` (near `:1109`) |
| `sandbox/CMakeLists.txt` | `CaptionEditor.cpp`, `AccessibilityPreview.cpp` |
| `scripting/csharp/Aver.Graph/GraphGameSystemNodeTable.cs` | `.Concat(AccessibilityTable())` on `:121` |
| `scripting/csharp/Aver.Scripting.Bridge/HostBridge.cs` | `DispTickAll` group 0: `Accessibility.Tick(dt)` (one line near `:1498`); nothing in `ConfigureInput` (native loaded the INI already; C# only needs the store open to drain into) |
| `scripting/csharp/Aver.UI/SettingsScreen.cs` | add the new keys to `s_defaultKeys` (`:54-58`); `scripting/csharp/Aver.Framework/UiGameSettings.cs` route `access.*` and `graphics.uiScale` through `AccessSettingsStore` in `Create` |
| `scripting/csharp/Aver.Graph.Tests/Program.cs` | `failures += AccessibilityNodeTests.RunAll();` |
| root `CMakeLists.txt` | `add_subdirectory(tests/framework_access)` beside `tests/framework_sequence` (`:577`) |
| `tests/formats/CMakeLists.txt`, `tests/rhi/CMakeLists.txt`, `tests/ui/CMakeLists.txt`, `tests/platform/CMakeLists.txt`, `tests/game/CMakeLists.txt`, `tests/editor/CMakeLists.txt` | the `add_executable` blocks for the new tests |
| root `CMakeLists.txt` `AVER_CTEST_VULKAN_TARGETS` (`:649`) | add `ColourVisionShaderTest` |
| `CHANGELOG.md` | one entry |

Not edited by anyone here: `scene_abi.h`, `Components.hpp`, `audio` mixer sources, `modules/rhi.d3d12/**`, `modules/rhi.vulkan/**`, `scripts/*.allowlist`, `cmake/modules.catalog.json`, every `module.json`. Run `scripts/module-guard-audit.py` after.

## 4. Phased implementation

Each phase ends with its tests green and the defaults bit-identical (`PostSettings{}` frames, empty caption set, no toggles).

**Phase 0: options spine.** `AccessKeys.hpp`, `AccessAbi.cpp` (options part, `framework_access_abi.h`), `GameAccessibility` (`initAccessibility` reads the INI text of `gameSettingsPath` / `Saved/Settings.ini` once; `accessibilityFrame` stub), `Accessibility.cs` (typed properties, `Changed`, `Tick`, `AccessSettingsStore`, debounced persistence), `HostBridge` tick line, `Access.Changed` bus event. Tests: AccessKeysTest, FrameworkAccessAbiTest (options part), GameAccessibilityTest.

**Phase 1: UI scale, text scale, high contrast, settings screen.** `UiAccess`, `UiTree::setAccess`, `fontPx`/`styleOf`/draw hunks, `accessName`, `aver_ui_set_access`, default settings model + tabs + window reflow, `graphics.uiScale` move, `AccessSettingsStore` wired in `UiGameSettings.Create`, `SettingsScreen.s_defaultKeys`. Tests: UiAccessTest, extend UiWidgetTest.

**Phase 2: colour vision.** `ColourVision.hpp`, `aver_colour_vision.hlsli`, `post.hlsl`, `FrameConstants.hpp`, `RHI.hpp`, `ui.hlsl`, `UiRenderer`, `applyAccessibilityToPost`, editor preview menu. Tests: ColourVisionTest, ColourVisionShaderTest (both backends), extend PostLensTest. **Headed D3D12 and Vulkan check before moving on.**

**Phase 3: captions.** `OcCaptions`, `LocGather`, `CaptionEngine`, audio observer, `UiCaptionView`, `aver_ui_captions_set`, host frame wiring, hot reload, `Caption.Show` bus subscriber, `CaptionEditor` and Content Browser entries. Tests: OcCaptionsTest, CaptionEngineTest, UiCaptionViewTest, CaptionEditorTest, extend audio ABI test for the observer.

**Phase 4: reduce motion and camera feel.** `CameraFx.hpp`, ABI part, `applyAccessibilityCamera`, host camera hooks, post gating of blur and lens effects. Tests: CameraFxTest, GameAccessibilityTest (post override).

**Phase 5: input assist.** `AccessAssist.hpp`, `ActionEngine` timing, `FrameworkAbi.cpp` hooks, per-action toggle rows in the Input Assist tab (C# lists `aver_fw_action_*` registry). Tests: AccessAssistTest, extend `FrameworkTest` untouched-behaviour cases.

**Phase 6: speech and narration.** `Speech.hpp` + both implementations + CMake, `SpeechQueue`, `uiAccessibleText`, the focus listener and announcer, captions narration. Tests: SpeechTest, SpeechQueueTest, extend UiAccessTest.

**Phase 7: nodes, C# surface, docs.** `GraphAccessibilityNodeTable.cs`, `GraphNodeDefsAccessibility.hpp`, `AccessibilityNodeTests.cs`, docs. Then the headed pass (section 6).

Phase order is a dependency order only for 0 (everything reads the spine); 1 to 6 are independent after it and can land separately. Phases 2 and 3 are the large ones.

## 5. Tests (ctest-registered, no GPU unless stated; any `*Test` under `tests/` is auto-registered by `aver_register_ctest_in`)

- **`OcCaptionsTest`** (`tests/formats`, links `Aver.Formats Aver.Core`): round trip of speakers, cues, lines, quoting with `\"` and `\\`; `tag` shorthand; repeated `sound`; unknown record and comment preserved by the text-merging writer; header missing or newer fails; cue with no line and no tag skipped with a diagnostic naming the line; bad number skipped, rest parsed; duplicate cue ids kept as authored by the parser (replacement is the loader's rule).
- **`AccessKeysTest`** (`tests/framework_access`): every key's default lies in its range; `accessClamp` snaps to step, toggles to 0/1, choices to integers; `accessParseIni` reads `1.000000`, `true`, `off`, ignores unknown keys and junk, keeps `graphics.uiScale`; dynamic `access.input.toggle.X` accepted; no two keys share a name; every static key with a tab has a label ref.
- **`CaptionEngineTest`**: pattern matching (exact, `*`, `?`, case, tail match, no match); mode filtering (Subtitles drops `sound` and `music`); radius skip; cooldown with an injected clock; timed lines appear and expire at `start`/`dur`; `dur 0` follows the voice via the probe (voice ends, +0.35 s, min `hold`); dialogue cut short keeps 0.8 s; no-voice cue ends at its last line; line cap and priority eviction (dialogue never evicted by sound); speaker shown only on change; colours on/off; direction hints at -90, -20, 0, 20, 90, 170 degrees (left, none, none, none, right, behind); alpha fade and `reduce motion` forcing 1; `show`/`showText`/`stop`; `clear` and reload keep actives that still resolve; deterministic (no wall clock).
- **`SpeechQueueTest`** (fake `SpeechSynth`): interrupt vs queue, 3-deep drop-oldest, duplicate suppression within 1 s, per-source gating, loc-ref resolution, rate/volume pushed once per revision, `available() == false` is silent, a `masked` marker never spoken.
- **`CameraFxTest`**: zero scales return before touching state (bit-identical), trauma decay, saturation, deterministic for a seed and time, amplitudes within the stated bounds, bob period equals speed/320, `reduce` zeroes both.
- **`AccessAssistTest`**: toggle flips on rising edge only, holds `V`, axis actions ignored, exempt action ignored, reset clears; `Timing` scales Hold/HoldRelease/Tap/Combo and leaves Pulse/Chord/Down alone; null timing equals the old results exactly (replays the existing `ActionEngine` cases); a toggled action's `held/pressed/released` come from the cached values.
- **`FrameworkAccessAbiTest`** (links `Aver.Framework`): version 1; set/get/clamp/unknown key; revision and dirty drain order; `Access.Changed` posted once per accepted change; `load_ini` count; `effective` applies `reduce`; caption load from a temp directory, `show`/`line_at`/`new_line_at` after `update`, observer path with a fake probe, `Caption.Show` bus event; speech with the null/stub synth; `camera_fx_apply` with and without scales.
- **`ColourVisionTest`** (`tests/rhi`, `Aver.RHI Aver.Core`): each matrix row sums to 1 within 1e-5; grey in = grey out for all modes and strengths; strength 0 is identity; correction of a pure-red and a red/green pair moves toward blue/green separation (sign checks); simulate is idempotent in linear space; clamp to [0,1]; `packColourVision` round trips mode, simulate and strength to 1/100; `fillPostLens` default leaves `cb.access` zero; `PostSettings` equality sees the new fields.
- **`ColourVisionShaderTest`** (`tests/rhi`, opts into `AVER_CTEST_VULKAN_TARGETS`, WARP or hardware, SKIPPED with exit 77 when absent): compiles `post.hlsl` composite and `ui.hlsl` with `AVER_POST_*` defines through each backend's include handler, then draws a 6-patch swatch through a tiny pipeline using the same `.hlsli` into an RGBA8 target and compares the read-back against `ColourVision.hpp` within 2/255. Catches include-resolution and `pow` drift between the two backends.
- **`UiAccessTest`** (`tests/ui`): `textScale` multiplies `fontPx` and wraps differently, not layout rects; default `UiAccess` is byte-identical to today (draw-list hash on the existing fixtures); high contrast ignores `bgColor/textColor/accentColor`, uses the contrast palette and a thicker ring, restores on off; `uiAccessibleText` for each widget kind (name, role, state, value, the settings-row label sibling, masked field, disabled, `accessName` override, loc ref); the focus listener announces once per focus change and not on `setFocus` of the same widget; the settings screen has the new tabs, 2.0 scale leaves the Back button inside the viewport, and `setting:<key>` events carry the new keys.
- **`UiCaptionViewTest`**: row recycling, top/bottom anchor, bg opacity, high-contrast floor, `dir` glyphs, passive (never takes hit-test or focus), localized text follows a culture epoch change, empty list hides the root.
- **`SpeechTest`** (`tests/platform`): `createSpeechSynth()` never null; `stop()`/`speaking()`/`setVolume(0)` safe; `speak("")` no-op; on POSIX `available() == false`; on Windows the test sets volume 0 before speaking so CI makes no sound, and skips (77) when no voice is installed.
- **`GameAccessibilityTest`** (`tests/game`): `applyAccessibilityToPost` is a no-op at defaults and wins over volume-blended values; reduce motion zeroes blur, chromatic, grain and lens flare and nothing else; camera wrapper identity at defaults; `initAccessibility` from INI text.
- **`CaptionEditorTest`** (`tests/editor`, compiles the editor source with `AVER_WITH_IMGUI` undefined, the `AnimGraphEditorTest` arrangement): load/save text-merge keeps comments, undo/redo, add cue from a sound path, pattern tester count, speaker colour edit, rename cue.
- **Managed** (`Aver.Graph.Tests`, run by `GraphManagedTests`): `AccessibilityNodeTests` in the `SequenceNodeTests` mould: exact row set (11), pins and defaults, IL valid, exec nodes refused by the pure compiler, pure reads work, missing attribute fails by name; every row names a real `AccessibilityGraphInterop` method (`GameSystemNodes.Validate`). Managed tests cannot see the native ABI (repo convention), so the C# store/property layer is covered by `FrameworkAccessAbiTest` through the C ABI instead.
- Existing suites that must stay untouched and green: `PostLensTest`, `UiWidgetTest`, `FrameworkTest` (`testActions`, `testInputScheme`), `SettingsTest`, the audio ABI tests, `OcStringsTest`.

## 6. Headed manual verification (the main loop runs these; D3D12 first, then `--backend vulkan`)

1. **Defaults identical**: capture the same camera on a lit level before and after with all options at defaults; image hash equal on both backends (`Linear metric needs --tonemap 0`, `--frames` capture).
2. **Colour vision**: a level with a red/green/blue swatch board plus a HUD with coloured bars. Cycle Off, Protanopia, Deuteranopia, Tritanopia in the Accessibility tab with correction, then the editor's Simulation preview. Check the scene and the HUD both change, together, on D3D12 and Vulkan; strength slider moves smoothly; grey ramp unchanged; a screenshot grab includes the filter.
3. **UI scale and text scale**: UI scale 0.75, 1.0, 1.5, 2.0 and text scale 0.75, 1.0, 2.0 on the settings screen and a HUD; nothing unreachable at 2.0 on 1280x720; Back button visible; text soft at 2.0 is expected until the UI-text feature lands.
4. **High contrast**: on a menu and a HUD with authored colours; panels become black/white/yellow, authored colours ignored, focus ring thick; off restores the game's theme.
5. **Captions**: a level with a VO file cue (two timed lines, two speakers), an explosion sound cue and a music cue. Check Subtitles shows only dialogue, Captions shows all, size presets, background opacity, top/bottom, speaker colours and names, direction hints with the source left, right and behind, line cap with a burst of sounds, cooldown on repeated explosions, a stopped VO cutting its line, pause (captions keep time with audio), culture switch re-localizes, hot reload of the `.occaptions`. A Sequencer cutscene: an audio track VO produces captions with no extra work; an Event key `Caption.Show` shows a cue.
6. **Reduce motion**: a test actor calls `CameraFx.AddShake(1)` and `SetBob(1, 600)` each frame while walking; shake and bob visible; "Reduce motion" removes both and motion blur and chromatic aberration (D3D12; Vulkan has no motion blur by design); individual sliders work when the master is off.
7. **Input assist**: bind Sprint (hold) and a `hold:0.5` Interact. Toggle Sprint on: tap latches, tap again releases; `holdScale 0.5` makes Interact fire at 0.25 s; `windowScale 3` lets a slow double tap count as a tap/combo; raw-key code is unaffected (documented).
8. **Speech (Windows)**: enable narration, tab through the settings screen: each focus change speaks name, role and state and interrupts the previous; slider adjusts speak the new value; rate and volume sliders take effect live; `Announce` from a graph node; captions narration on; with no SAPI voice the option greys out and nothing throws. With Narrator running, confirm the game does not crash and note that Narrator does not read the widgets (risk 3).
9. **Persistence**: change every option, quit, relaunch: all restored before the first frame; delete `Settings.ini`: defaults; edit it by hand with `access.captions.size=3`: honoured; a newer key left in the file survives a save.
10. **Frame generation, SR and PT**: toggle NeuraFI and AverSR with a colour filter on; the filtered image is stable (no double application); the PT reference view is unfiltered.
11. **Perf**: with every option at default, GPU frame time and CPU `accessibilityFrame` within noise (one revision compare per frame).

## 7. Open questions (defaults chosen; say if you disagree)

1. **Daltonization patents.** Filed art exists for "daltonization in games" (US 10118097, US 20140066196; I did not read the claims). Default: ship the published 2005 Fidaner error shift on the published 2009 Machado simulation, plus the Simulation mode, which is the safe core. If counsel objects, the correction branch is one `sim ? s : ...` ternary; deleting it leaves simulation and Windows-style plain matrices.
2. **New camera shake and head bob primitives.** Default: yes, minimal (`CameraFx`, 3 ABI calls, 2 nodes), because "reduce motion" must gate something real and the engine has none. Nothing calls them automatically.
3. **macOS and Linux speech.** Default: stub now, same interface. `AVSpeechSynthesizer` (a `.mm` file the platform module has no precedent for) and speech-dispatcher are follow-ups.
4. **Real screen-reader support** (UI Automation, NVDA, Narrator, VoiceOver) is not in v1; narration is the game speaking. A custom-drawn tree needs a UIA provider per widget, a separate feature.
5. **Camera/UI limits.** UI scale max 2.0 and text scale max 2.0; larger needs the UI-text feature's scalable font and a reflowing settings screen. Caption `lines` max 4.

## 8. Risks

1. Large text on the baked bitmap font looks soft; no blocker, improves with the UI-text feature (`UiTree.hpp` is a shared file between the two: merge both hunks).
2. Hosts need five coordinated insertions each (init, UI frame, post, camera, hot reload). They are listed in 3.3; `GameAccessibility` exists so each is one call.
3. The caption observer only sees audio started through the ABI (all of it today). A future second audio entry would bypass it; `AbiEnumTest`-style check that `AudioAbi.cpp` calls the observer from each play function is in `FrameworkAccessAbiTest`'s audio part.
4. Frame-generation and SR paths: the filter is in the composite so interpolated frames inherit it only if they derive from the post-composite image (headed check 10).
5. Vulkan push block grows 4 bytes for the UI pipeline (148 B total). Already above 128 B; if a target device reports `maxPushConstantsSize` below 148 the existing 144 B would already fail, so this is not a new constraint.
6. `access.speech.enabled` first-run default from the OS query is read once and not persisted, so changing the OS setting later is honoured until the player touches the option.
