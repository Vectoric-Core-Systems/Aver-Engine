# Localization

Translated text per culture, chosen at run time, edited in the engine. Design: [plans/wave2/DATA_TABLES_LOCALIZATION.md](plans/wave2/DATA_TABLES_LOCALIZATION.md).

## Cultures

A culture is a tag such as `en`, `pt-BR`, `zh-Hans`. `pt_br` is accepted and normalised to `pt-BR`. Lookup falls back through the active culture, its parents (`zh-Hans-CN` to `zh-Hans` to `zh`), the project default (`LOCALE.DEFAULT`, default `en`), the inline default of the reference, then the literal `[Table:Key]`.

The active culture at start-up, first hit wins: `--culture <tag>` (game and editor) / the editor's saved preview culture, the player's `locale.culture` setting, the OS UI language reduced to the nearest shipped culture, `LOCALE.DEFAULT`. `Loc.SetCulture(tag)` stores the choice as `locale.culture`; the editor preview never does. A culture is not part of a save.

## Text references

Any UI text field, graph text attribute, C# string or data-table cell may hold a reference instead of text:

```
@Table:Key              @Menu:Play=Play
```

`Table` is `[A-Za-z0-9_]+`, `Key` is `[A-Za-z0-9_.-]+`, the default is everything after the first `=`. `@@x` is the literal `@x`; a string that starts with `@` but does not match is literal. Typed text and event payloads are never resolved (a Choice event carries the raw item, `index` is the identity).

## Files

```
Content/Localization/<culture>/<Table>.ocstrings      one file per table per culture
Content/Localization/<culture>/<Table>.csv            the same as CSV (Key,Text[,Note]); .ocstrings wins if both exist
```

```
OCSTRINGS 1
CULTURE de
# Main menu
ENTRY Menu.Play "Spielen"
ENTRY Menu.Quit "Beenden" NOTE "Shown on the quit button; keep short"
```

A malformed `ENTRY` line is skipped with a warning naming the line; a typo never blanks a table. Saving from the editor keeps comments, blank lines and unknown records. An empty text in a non-default culture means "not translated" and is not written, so the runtime falls through to the default culture.

### Gettext messages (.po)

A culture folder may also hold gettext `.po` files (Godot's PO translations). Their messages are looked up by
**source text**, not by table and key: `Loc.Tr("Hello, world!")` (Godot's `tr`) returns the translation from the
culture chain, or the source text itself when no culture has one; `Loc.Tr(text, context)` uses `msgctxt`;
`Loc.TrN(singular, plural, n)` (Godot's `tr_n`) picks `msgstr[0]` for one and `msgstr[1]` otherwise (no
`Plural-Forms` formula yet). All `.po` files of a culture merge; an empty `msgstr` counts as untranslated. A `.po`
file is read when its culture loads; editing one needs a reload of the localization (no hot reload yet).

## Editor

- **String Table tab** (double-click a `.ocstrings`, or New String Table in the Content Browser): rows are the union of keys, columns are Key, Note and one per culture (default first). Missing cells are tinted; typing creates the entry. Add key, Delete key, Rename key (every culture), Add culture, Show missing only.
- **Gather** adds every key the project uses (`.ocui`, `.ocgraph`, `.occaptions` speaker names, cue tags and line texts, C# `Loc.Text("T","K","Default")` / `new LocText(...)`, data-table cells) to the default culture, text = the inline default. **Report** lists missing, unused, conflicting-default and untranslated keys, with Copy as text. When a Play session ends, a toast says how many keys it found missing.
- **Project Settings > Description**: Default Culture (`LOCALE.DEFAULT`) and Preview Culture (editor only).
- **UI Layout tab**: a culture box previews that culture; Localize rewrites a literal text/placeholder/items field to `@UI:<Layout>.<Widget>.<property>=<literal>`; a field holding a reference shows its resolved text.
- External edits of string files apply within the watcher debounce; a file that no longer parses keeps the old text and logs once.

## C#

```csharp
Loc.Text("UI", "Play", "Play");                    // chain, then default, then "[UI:Play]"
Loc.Format("UI", "Score", "Score: {0:N0}", score);  // composite format in the culture's number style
Loc.Plural("Items", "count", n);                   // Items.count.one / .few / .many / .other, then .other, then the key
Loc.SetCulture("de");                              // false for a malformed tag
Loc.CultureChanged += tag => { ... };              // raised on the game thread, once per frame at most
public LocText Title = new("UI", "Title", "My Game");   // follows a culture switch without being touched
```

`Loc.MissingKeys` lists text asked for that no culture had. In a graph, `AN_OnEvent event=loc.culture_changed` reacts to a switch; `AN_SetCulture`, `AN_IsCulture`, `AN_CultureCount`, `AN_CurrentCultureIndex`, `AN_SetCultureByIndex` and `AN_SetUiItemsToCultures` drive it, and `AN_SetUiText text="@UI:hud.ammo=Ammo"` shows localized text. When more than one culture ships, the settings screen gets a Language choice.

Plural categories: `ja zh ko vi th id ms` other only; `fr pt` one for 0 and 1; `ru uk be` one/few/many; `pl` one/few/many; `cs sk` one/few/other; everything else one/other. No gender, no ICU MessageFormat.

## Fonts

The game UI draws with a font set (`Content/UI/Fonts.ocfonts`, or the built-in default: Roboto, then the Windows fonts, then the baked bitmap font), so a glyph the first font lacks comes from the next one in the chain. Latin with accents, Greek and Cyrillic come from Roboto (`Roboto-Regular.ttf` and `Roboto-Medium.ttf` ship next to the executable); CJK, Arabic and Hebrew come from the Windows fonts on Windows, or from a project font you add to the set. Arabic is shaped (letters join, lam-alef ligatures), Arabic and Hebrew run right to left, mixed text is reordered, and Chinese and Japanese wrap between characters with the usual line-start and line-end rules. A language can name its own family (`OVERRIDE ja Body BodyJa`), and entries tagged `lang=` are tried first for that language, which is how Han characters pick the Japanese or the Chinese shapes. The font set follows the active culture. Not covered: Indic, Thai, Khmer and Myanmar shaping, vertical text, colour emoji and IME input. Details, the `.ocfonts` format and the limits are in [GAME_UI_TEXT.md](GAME_UI_TEXT.md).

The baked bitmap font (`assets/fonts/Roboto-Regular.ocfont`, made by `scripts/bake-font.py`) is the fallback when no TrueType font can be opened; it covers Latin with accents, Greek and Cyrillic only. The editor's own ImGui font has no Greek or Cyrillic, so the editor chrome shows boxes where the game draws correctly; the UI layout editor's preview uses the game's text engine and is not affected.

## Packaging

Nothing to stage: `.ocstrings` and `.csv` are ordinary content files, `Game.ocproject` carries `LOCALE.DEFAULT`, `Aver.Framework.dll` is already shipped. Only the editor hot reloads.
