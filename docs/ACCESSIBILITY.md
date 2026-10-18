# Game accessibility

What a player can change so a game is playable for them: bigger and higher-contrast UI, subtitles and sound captions, colour-vision filters, reduced motion, easier input timing, and a game that reads its menus aloud. Design (frozen contract): [plans/wave9/ACCESSIBILITY.md](plans/wave9/ACCESSIBILITY.md). Reference points: Unreal's accessibility tooling and the Xbox Accessibility Guidelines (101 text display, 102 contrast, 103 captions, 107 text to speech, 109 input, 112 reduce motion, 113 colour filters).

There is one option store and nothing parallel: the existing settings screen, UI tree, post chain, audio ABI, Enhanced Input and localization were each upgraded in place. No new DLL and no new module.

## The options

Every option is a float under a stable key, kept in `Settings.ini` (so `access.captions.size=3` by hand is honoured) and loaded **before the first frame** by the native host. `Accessibility.Get/Set` in C# and `aver_fw_access_get/set` in C reach the same state; a change is clamped and snapped to the key's step, bumps a revision, raises the bus event `Access.Changed` (string key, float value) and is written back by a debounced flush.

| Key | Kind | Range / choices | Default |
|---|---|---|---|
| `graphics.uiScale` | slider | 0.75 to 2.0, step 0.05 | 1 |
| `access.ui.textScale` | slider | 0.75 to 2.0 | 1 |
| `access.ui.highContrast` | toggle | | off |
| `access.colour.mode` | choice | Off, Protanopia, Deuteranopia, Tritanopia | Off |
| `access.colour.strength` | slider | 0 to 1 | 1 |
| `access.colour.simulate` | toggle | developer preview, not on the screen | off |
| `access.motion.reduce` | toggle | master gate | off |
| `access.motion.shake`, `access.motion.bob` | slider | 0 to 1 | 1 |
| `access.motion.blur` | toggle | 1 = motion blur allowed | on |
| `access.motion.lens` | toggle | 1 = chromatic aberration, grain and lens flare allowed | on |
| `access.captions.mode` | choice | Off, Subtitles (dialogue), Captions (all) | Subtitles |
| `access.captions.size` | choice | Small 0.8, Medium 1.0, Large 1.4, Extra large 1.9 | Medium |
| `access.captions.bgOpacity` | slider | 0 to 1 | 0.6 |
| `access.captions.position` | choice | Bottom, Top | Bottom |
| `access.captions.lines` | slider | 1 to 4 | 3 |
| `access.captions.speaker`, `.colours`, `.direction` | toggle | speaker names, speaker colours, `<` `>` `v` hints | on, on, off |
| `access.speech.enabled` | toggle | the game reads aloud; first run follows the OS screen-reader flag, which is not saved unless the player touches it | off |
| `access.speech.ui`, `access.speech.captions` | toggle | read menus, read new caption lines | on, off |
| `access.speech.rate` | slider | -10 to 10 (SAPI's range) | 0 |
| `access.speech.volume` | slider | 0 to 1 | 1 |
| `access.input.holdScale` | slider | 0.25 to 1 (shorter holds) | 1 |
| `access.input.windowScale` | slider | 1 to 4 (longer tap and combo windows) | 1 |
| `access.input.toggle.<Action>` | toggle | one per Digital action the game lists | off |

**Reduce motion is a gate, not a preset.** While it is on, the effective shake, bob, motion blur and lens effects are zero; the sliders keep their values, so un-ticking restores them.

The settings screen (`UiGameSettings.Create()`) has the tabs **Accessibility**, **Captions and Speech** and **Input Assist** built from the default settings model, so any game gets them. `AccessSettingsStore` answers these keys from `Accessibility`. The Input Assist tab also gets a *Toggle <Action>* row for every registered Digital action the game has not exempted. The key table is `modules/framework/include/aver/framework/AccessKeys.hpp`; `uiDefaultSettingsModel()` in Aver.UI mirrors it (neither module can see the other), and `GameAccessibilityTest` fails if they drift.

## UI scale, text scale, high contrast

`graphics.uiScale` scales the whole layout. `access.ui.textScale` multiplies font sizes only: row heights, padding and widths stay on the UI scale, so a large text size wraps onto more lines instead of moving widgets. High contrast swaps the tree to the built-in `contrast` theme (black panels, white borders, yellow accent), ignores every widget's `bgColor`, `textColor`, `accentColor` (and rich-text colour tags), draws the focus ring twice as thick and the scrollbar in full white. Images and tints are untouched. The game's own theme (`aver_ui_set_theme`) is kept and comes back when the option turns off.

The settings window fills 90 percent of the viewport up to 980 x 720 design pixels and stays centred, so a 2.0 UI scale on 1280 x 720 still shows the Back button; row labels wrap. A wrapped `Text` that takes its width from a fill weight (`fillW > 0`, no `width`) now wraps at the width its slot gave it, through a second layout pass the first frame it appears or the viewport changes.

`accessName` (a widget property, group Accessibility) is the label a narrator speaks for the widget. Empty means derived: the widget's own text for a Button, a Toggle with text or a Text, otherwise the nearest preceding `Text` sibling (the settings rows are a `Text` and a control in one row), otherwise the placeholder.

## Captions

Captions are data plus the audio ABI, not a second audio system.

**`Content/Captions/*.occaptions`**, merged in name order (a later duplicate cue id replaces the earlier one and logs once):

```
OCCAPTIONS 1
SPEAKER guard name "@Cap:spk.guard=Guard" colour #7DD3FC
CUE intro_01 sound "Audio/VO/intro_01.ocaudio" kind dialogue speaker guard priority 8
  LINE 0.0 2.4 "@Cap:intro_01_a=Welcome to the station."
  LINE 2.6 0   "@Cap:intro_01_b=Keep your voice down."        # dur 0 = until the voice ends
CUE boom sound "Audio/SFX/explosion_*.ocaudio" kind sound priority 5 radius 4000 cooldown 1.5 hold 1.8 tag "@Cap:fx.explosion=[explosion]"
```

* `SPEAKER <id> [name <text>] [colour #RRGGBB[AA]]`. Names and texts are text or localization references.
* `CUE <id>` keywords: `sound <pattern>` (repeatable; content-relative, `/` separators, case-insensitive, `*` and `?` inside a segment, matched against the **tail** of the played path; an exact path beats a pattern), `kind dialogue|sound|music` (default sound), `speaker <id>`, `priority <int>`, `radius <cm>` (0 = always; positional sounds farther than this from the listener are skipped), `cooldown <sec>` (default 1.0, per cue), `hold <sec>` (minimum on-screen time; default 1.5, or 0.8 for dialogue), `tag <text>` (shorthand for one `LINE 0 0 <text>`).
* `LINE <start> <dur> <text> [speaker <id>]`: seconds from the cue start; `dur 0` = until the voice ends (plus 0.35 s, never less than `hold`). A dialogue line whose voice is cut short keeps its line for at least 0.8 s, and lines that would begin after the cut never show.
* A malformed record is skipped with a diagnostic naming the line; a cue with no `LINE` and no `tag` is skipped with a diagnostic. Only a missing or newer header fails. Unknown records and comments survive a save from the editor.

**What raises a cue.** `aver_audio_set_play_observer` (audio ABI) fires after every successful start from `aver_audio_play`, `_play_at`, `_stream_play`, `_stream_play_at` and `_music_play`, so scripts, graph nodes and sequencer audio tracks produce captions with no per-caller work. Generated PCM has no path and captions through `Captions.Show("cue")`. A sequencer **Event** key named `Caption.Show` with the cue id as its string argument also works (the bus event takes the cue id and optionally x, y, z or a Vec3 to make it positional); `Caption.Stop` ends one.

**Display.** The engine keeps the visible lines (priority, then age; a sound caption never evicts dialogue; capped at `access.captions.lines`; a speaker name only when the speaker changes; the last 0.25 s fade, forced opaque under reduce motion). Aver.UI draws them as a passive Overlay root named `Aver.Captions` (`UiCaptionView`) that never takes the pointer or focus. Text stays a raw localization reference in the widget, so a culture switch re-reads it. Captions run on **unscaled** frame time: they follow audio, which keeps playing while the game is paused.

## Colour vision

A filter in the post chain (and in the HUD shader, which draws after the tonemap and would otherwise be unfiltered): `aver_colour_vision.hlsli`, one include shared by `PSComposite` and `UiPS`, on D3D12 and Vulkan. Simulation uses the Machado, Oliveira and Fernandes (2009) severity-1.0 matrices in linear RGB (every row sums to 1, so grey stays grey); correction adds the Fidaner, Lin and Ashburner (2005) error shift. `access.colour.strength` lerps from the unfiltered colour. The player's choice is applied **after** the level's post volumes blend, so a level cannot undo it. Defaults leave every frame bit-identical.

The editor has **Colour Vision Preview** in the viewport menu (Off, Protanopia, Deuteranopia, Tritanopia; Correction or Simulation; strength). It is an editor preference, never written to a project, and it filters the scene, not the editor panels. The player's own filter wins in a Play session.

## Reduce motion and camera feel

The engine had no camera shake or head bob, so there is a small one (`CameraFx`, deterministic, no allocation): `CameraFx.AddShake(trauma)` (0 to 1, saturating, decaying at 1.5 per second; at full trauma 1.5 degrees yaw, 1.2 degrees pitch and 1.5 cm of offset) and `CameraFx.SetBob(amount, speedCmPerSec)` (call each frame while moving; 0 stops it; 2.2 cm vertical and 1.1 cm sideways, one cycle per 320 cm). Nothing calls them automatically. Hosts apply the result right before the camera is built, with the effective scales, so with both at 0 the camera is untouched. The editor's free-fly camera is not affected.

Reduce motion also zeroes motion blur, chromatic aberration, grain and lens flare in the post settings (blur and lens separately when only those toggles are off). Photosensitivity (flash limiting) is not in this version; bloom, flare and exposure adaptation stay as authored.

## Input assist

Applied inside the Enhanced Input layer, so it covers **named actions** (`.ocinput`, `InputAction`, graph `InputAction` nodes) and not raw `Input.IsKeyDown` reads.

* **Toggle instead of hold**, per Digital action (`access.input.toggle.<Action>`): a press latches the action on, the next press releases it. `held`, `pressed` and `released` read the latched value. Axis actions ignore it. `Accessibility.Input.SetToggleAllowed("Pause", false)` hides an action from the settings page and ignores a saved toggle. The latch resets at Play start, on `aver_fw_action_reset_state` and when bindings are cleared.
* **Timing**: `holdScale` multiplies `Hold` and `HoldRelease` times; `windowScale` multiplies the `Tap` maximum and the `Combo` window. `Pulse`, `Chord` and `Down` are untouched. With both at 1 the trigger engine is exactly what it was.

## Speech and narration

The game talks through the OS voice (`aver::platform::SpeechSynth`: SAPI on a worker thread on Windows, a silent stub elsewhere). It is **not a screen-reader bridge**: NVDA and Narrator do not see the widgets, because the UI is custom drawn and would need a UI Automation provider per widget. With narration on:

* focusing a widget speaks `name, role, state or value` (for example `Fullscreen, checkbox, checked`, `UI scale, slider, 1.00`, `Quality, choice, High, 3 of 4`); a changed slider, toggle or choice speaks the new value; a focus change interrupts the previous speech. A masked field's content is never read. Roles and states are localization references (`@Access:role.button=button`) so they translate.
* new caption lines are read when `access.speech.captions` is on.
* `Speech.Announce(text, interrupt)` speaks from a script, and `Announce` from a graph. At most three announcements wait; the oldest is dropped; the same text within a second is dropped.
* with no voice installed `Speech.Available` is false, announcements return false and nothing throws.

## C# API

```csharp
Accessibility.Get("access.ui.textScale"); Accessibility.Set("access.motion.reduce", 1f);
Accessibility.TextScale = 1.5f;  Accessibility.ColourVision = ColourVisionMode.Deuteranopia;  Accessibility.ReduceMotion = true;
Accessibility.Changed += (key, value) => { ... };            // once per accepted change, from the frame tick
Accessibility.Input.SetToggle("Sprint", true);  Accessibility.Input.SetToggleAllowed("Pause", false);
Captions.Mode = CaptionMode.Captions;  Captions.Show("intro_01");  Captions.ShowAt("boom", pos);
Captions.ShowText("Narrator", "@Cap:x=Hello", seconds: 3f);  Captions.Stop();
Speech.Enabled = true;  Speech.Announce("Checkpoint reached");  Speech.IsSpeaking;
CameraFx.AddShake(0.6f);  CameraFx.SetBob(1f, groundSpeed);
```

`Accessibility.Tick(dt)` runs in the per-frame pump (HostBridge group 0): it drains accepted changes into `Settings`, raises `Changed`, flushes the file after half a second without a change and advances the speech queue.

## Aver Node

Category **Accessibility**, eleven nodes: `SetAccessibilityOption` and `GetAccessibilityOption` (attribute `key`), `ShowCaption` (attribute `cue`, pins `x y z positional`), `ShowCaptionText` (attributes `speaker?` and `text`, pins `seconds kind`), `StopCaption` (`cue?`, empty = all), `Announce` (attribute `text`, pin `interrupt`), `StopSpeaking`, `IsSpeaking`, `AddCameraShake` (pin `trauma`), `SetCameraBob` (pins `amount speed`) and `SetActionToggleAllowed` (attribute `action`). An `AN_OnEvent` entry named `Access.Changed` reacts to an option change. Option keys, cues and texts are node attributes, never pins.

## Editor

* **Captions tab** (double-click a `.occaptions`, or New Captions in the Content Browser): speakers (id, name reference, colour), a filterable cue list, cue details (id, kind, speaker, priority, radius, cooldown, hold, tag, sound patterns with a live "matches N sounds in this project" tester), a line table (start, duration, text with its resolved preview, per-line speaker), **From Sound** (a cue named after an audio asset, dialogue if it sits under a VO or Voice folder) and **Preview** (shows the cue through the engine). Save keeps the file's own comments.
* **Viewport > Colour Vision Preview**, described above.
* **Gather** (Localization) collects the references in `.occaptions` files like any other source.
* A Play session reads the project's `Saved/Settings.ini` accessibility keys (the player's own choices are the thing being tested), unlike the localization preview.

## Files

Native: `AccessKeys.hpp`, `AccessAssist.hpp`, `CaptionEngine.hpp`, `SpeechQueue.hpp`, `CameraFx.hpp`, `framework_access_abi.h`, `AccessAbi.cpp` (Aver.Framework); `Speech.hpp`, `Win32Speech.cpp`, `PosixSpeech.cpp` (Aver.Platform); `ColourVision.hpp`, `aver_colour_vision.hlsli` (Aver.RHI); `UiAccess.hpp/.cpp`, `UiCaptionView.cpp` (Aver.UI); `OcCaptions.hpp/.cpp` (Aver.Formats); `GameAccessibility.hpp/.cpp` (Runtime); `CaptionEditor`, `AccessibilityPreview` (editor). Managed: `Accessibility.cs`, `AccessNative.cs`, `GraphAccessibilityNodeTable.cs`. Format: [formats/FORMAT_SPECS.md](formats/FORMAT_SPECS.md).

## Not done

macOS and Linux speech (the stub answers `available() == false`), real screen-reader support (UI Automation, VoiceOver), photosensitivity limiting, per-action timing overrides, and UI or text scale above 2.0. Under the FXC fallback (no `dxcompiler.dll`) the two shaders that `#include` the colour-vision file do not compile; DXC is required, as it already is for ray tracing.
