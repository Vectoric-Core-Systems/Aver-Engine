// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Managed face of the localization store (framework_loc_abi.h). See docs/LOCALIZATION.md.

using System.Globalization;

namespace Aver.Framework;

/// <summary>Localized text for the active culture. Text is looked up as Table + Key through a fallback chain
/// (active culture, its parents, the project default, then the inline default, then "[Table:Key]").
/// A text <em>reference</em> is the same thing as a string: <c>"@Table:Key"</c> or <c>"@Table:Key=Default"</c>,
/// accepted by every UI text field, graph text attribute and data-table text cell; <c>"@@x"</c> is the literal
/// <c>"@x"</c>. Inert without the native library: references then read as their inline default.</summary>
public static class Loc
{
    private static string s_pumpCulture = string.Empty;
    private static bool s_pumped;
    private static bool s_noNative;
    private static string s_infoFor = string.Empty;
    private static CultureInfo s_info = CultureInfo.InvariantCulture;

    /// <summary>The active culture tag, normalised ("pt-BR"). "en" without the native library.</summary>
    public static string Culture => Native(() => FwLoc.Str(FwLoc.aver_fw_loc_culture()), "en");

    /// <summary>The project's default culture (LOCALE.DEFAULT), the last stop of the fallback chain.</summary>
    public static string DefaultCulture => Native(() => FwLoc.Str(FwLoc.aver_fw_loc_default_culture()), "en");

    /// <summary>The cultures the project ships (the folders under Content/Localization), sorted.</summary>
    public static IReadOnlyList<string> Cultures => Native<IReadOnlyList<string>>(() =>
    {
        int n = FwLoc.aver_fw_loc_culture_count();
        var list = new List<string>(n);
        for (int i = 0; i < n; i++) list.Add(FwLoc.Str(FwLoc.aver_fw_loc_culture_at(i)));
        return list;
    }, Array.Empty<string>());

    /// <summary>For number and date formatting in the active culture; invariant when the OS does not know the tag.</summary>
    public static CultureInfo CultureInfo
    {
        get
        {
            string tag = Culture;
            if (tag != s_infoFor)
            {
                s_infoFor = tag;
                try { s_info = CultureInfo.GetCultureInfo(tag); }
                catch (CultureNotFoundException) { s_info = CultureInfo.InvariantCulture; }
            }
            return s_info;
        }
    }

    /// <summary>Switches the active culture. False when <paramref name="tag"/> is malformed (nothing changes).
    /// A culture with no tables is accepted and resolves from the fallback chain. With
    /// <paramref name="persist"/> the choice is stored as the player's <c>locale.culture</c> setting.
    /// <see cref="CultureChanged"/> follows on the next <see cref="Pump"/>.</summary>
    public static bool SetCulture(string tag, bool persist = true)
    {
        if (string.IsNullOrWhiteSpace(tag)) return false;
        bool ok = Native(() => FwLoc.aver_fw_loc_set_culture(tag) >= 0, false);
        if (ok && persist)
        {
            try
            {
                Settings.SetString("locale.culture", Culture);
                Settings.Flush();
            }
            catch (Exception e) when (e is DllNotFoundException or BadImageFormatException) { }
        }
        return ok;
    }

    /// <summary>The text for <paramref name="table"/>/<paramref name="key"/>: the chain, then
    /// <paramref name="defaultText"/>, then <c>"[Table:Key]"</c>.</summary>
    public static string Text(string table, string key, string? defaultText = null)
    {
        if (!LocRef.Valid(table, key)) return defaultText ?? "[" + table + ":" + key + "]";
        return Resolve("@" + table + ":" + key + (defaultText != null ? "=" + defaultText : string.Empty));
    }

    /// <summary>A message by its source text, from the culture chain's .po files; the source text itself
    /// when no culture translates it. <paramref name="context"/> is the msgctxt.</summary>
    public static string Tr(string message, string context = "") =>
        Native(() => FwLoc.Fetch((b, c) => FwLoc.aver_fw_loc_tr(message, context, b, c)), message) ?? message;

    /// <summary>The singular or plural form of a message for <paramref name="n"/>: msgstr[0] for 1,
    /// msgstr[1] otherwise; the source forms when untranslated.</summary>
    public static string TrN(string message, string plural, int n, string context = "") =>
        Native(() => FwLoc.Fetch((b, c) => FwLoc.aver_fw_loc_tr_n(message, plural, n, context, b, c)), n == 1 ? message : plural)
        ?? (n == 1 ? message : plural);

    /// <summary>The text for the key in the chain only: false when no culture in it has the key (no default, no brackets).</summary>
    public static bool TryGet(string table, string key, out string text)
    {
        string? s = Native(() => FwLoc.Fetch((b, c) => FwLoc.aver_fw_loc_get(table, key, b, c)), null);
        text = s ?? string.Empty;
        return s != null;
    }

    /// <summary><c>"@T:K=D"</c> resolved, <c>"@@x"</c> to <c>"@x"</c>, anything else unchanged.</summary>
    public static string Resolve(string textOrReference)
    {
        if (string.IsNullOrEmpty(textOrReference) || textOrReference[0] != '@') return textOrReference;
        if (textOrReference.Length > 1 && textOrReference[1] == '@') return textOrReference.Substring(1);
        if (!LocRef.TryParse(textOrReference, out LocRef r)) return textOrReference;
        string? s = Native(() => FwLoc.Fetch((b, c) => FwLoc.aver_fw_loc_resolve(textOrReference, b, c)), null);
        return s ?? r.Fallback();
    }

    /// <summary><see cref="Text"/> formatted with <paramref name="args"/> in the active culture's number and date style.</summary>
    public static string Format(string table, string key, string? defaultText, params object?[] args)
        => LocFormat.Apply(Text(table, key, defaultText), CultureInfo, args);

    /// <summary>Count-dependent text: looks up <c>key.one</c>, <c>key.few</c>, <c>key.many</c> or <c>key.other</c> by the
    /// culture's plural rule, falling back to <c>key.other</c>, then <c>key</c>. <c>{0}</c> is <paramref name="count"/>
    /// unless <paramref name="args"/> are given.</summary>
    public static string Plural(string table, string key, long count, params object?[] args)
    {
        string category = LocFormat.PluralCategory(Culture, count);
        string? template = null;
        if (TryGet(table, key + "." + category, out string t)) template = t;
        else if (TryGet(table, key + ".other", out t)) template = t;
        else if (TryGet(table, key, out t)) template = t;
        template ??= Text(table, key + "." + category);   // records the miss; shows the bracket form
        return LocFormat.Apply(template, CultureInfo, args.Length == 0 ? new object?[] { count } : args);
    }

    /// <summary>"Table:Key" of every text asked for since the last <see cref="ClearMissing"/> that no culture had.</summary>
    public static IReadOnlyList<string> MissingKeys => Native<IReadOnlyList<string>>(() =>
    {
        int n = FwLoc.aver_fw_loc_missing_count();
        var list = new List<string>(n);
        for (int i = 0; i < n; i++) list.Add(FwLoc.Str(FwLoc.aver_fw_loc_missing_at(i)));
        return list;
    }, Array.Empty<string>());

    /// <summary>Forgets the missing-key list.</summary>
    public static void ClearMissing() => Native(() => { FwLoc.aver_fw_loc_missing_clear(); return 0; }, 0);

    /// <summary>Raised from <see cref="Pump"/> (the game thread, once per frame) with the new culture after it changed.</summary>
    public static event Action<string>? CultureChanged;

    /// <summary>Compares the active culture with the last frame's and raises <see cref="CultureChanged"/>. Called by the host's tick.</summary>
    public static void Pump()
    {
        string now = Culture;
        if (!s_pumped) { s_pumped = true; s_pumpCulture = now; return; }
        if (now == s_pumpCulture) return;
        s_pumpCulture = now;
        CultureChanged?.Invoke(now);
    }

    /// <summary>The native text epoch: changes whenever any text may have changed (culture switch, reload).</summary>
    internal static int Epoch => Native(() => FwLoc.aver_fw_loc_epoch(), 0);

    // Runs a native call; without the library (a bare process, a tool) returns the fallback and stays quiet afterwards.
    private static T Native<T>(Func<T> call, T fallback)
    {
        if (s_noNative) return fallback;
        try { return call(); }
        catch (Exception e) when (e is DllNotFoundException or BadImageFormatException) { s_noNative = true; return fallback; }
        catch (EntryPointNotFoundException) { s_noNative = true; return fallback; }
    }
}

/// <summary>A localizable string field, the LOCTEXT analogue: a table/key with an inline default, resolved against the
/// active culture every time it is read, so it follows a culture switch without being touched.</summary>
public readonly struct LocText
{
    private readonly string? _table, _key, _default, _literal;

    /// <summary>A reference to <paramref name="table"/>/<paramref name="key"/>.</summary>
    public LocText(string table, string key, string? defaultText = null)
    {
        _table = table; _key = key; _default = defaultText; _literal = null;
    }

    /// <summary>From <c>"@Table:Key=Default"</c>, or a literal (which <see cref="ToString"/> returns as is, <c>"@@"</c> unescaped).</summary>
    public LocText(string reference)
    {
        if (LocRef.TryParse(reference, out LocRef r))
        {
            _table = r.Table; _key = r.Key; _default = r.HasDefault ? r.Default : null; _literal = null;
        }
        else
        {
            _table = _key = _default = null; _literal = reference ?? string.Empty;
        }
    }

    /// <summary>The table, empty for a literal.</summary>
    public string Table => _table ?? string.Empty;
    /// <summary>The key, empty for a literal.</summary>
    public string Key => _key ?? string.Empty;
    /// <summary>The inline default, null when none was given.</summary>
    public string? Default => _default;
    /// <summary>True for <c>default(LocText)</c> and the empty literal.</summary>
    public bool IsEmpty => _table == null && string.IsNullOrEmpty(_literal);

    /// <summary>The text for the active culture.</summary>
    public override string ToString() => _table != null ? Loc.Text(_table, _key!, _default) : Loc.Resolve(_literal ?? string.Empty);

    /// <summary><see cref="ToString"/> formatted with <paramref name="args"/> in the active culture.</summary>
    public string Format(params object?[] args) => LocFormat.Apply(ToString(), Loc.CultureInfo, args);

    /// <summary>The text for the active culture.</summary>
    public static implicit operator string(LocText t) => t.ToString();
}

/// <summary>Plural categories and composite formatting. Pure: no natives.</summary>
public static class LocFormat
{
    /// <summary>The plural category ("one", "few", "many" or "other") of <paramref name="n"/> in <paramref name="culture"/>.
    /// By primary language: ja zh ko vi th id ms have only "other"; fr pt treat 0 and 1 as "one"; ru uk be and pl have
    /// one/few/many; cs sk have one/few/other; the rest (en de es it nl sv ...) have one/other.</summary>
    public static string PluralCategory(string culture, long n)
    {
        int dash = culture.IndexOfAny(new[] { '-', '_' });
        string lang = (dash < 0 ? culture : culture.Substring(0, dash)).ToLowerInvariant();
        long a = n < 0 ? -n : n;
        if (n == long.MinValue) a = long.MaxValue;
        long m10 = a % 10, m100 = a % 100;
        switch (lang)
        {
            case "ja": case "zh": case "ko": case "vi": case "th": case "id": case "ms":
                return "other";
            case "fr": case "pt":
                return a == 0 || a == 1 ? "one" : "other";
            case "ru": case "uk": case "be":
                if (m10 == 1 && m100 != 11) return "one";
                if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return "few";
                return "many";
            case "pl":
                if (a == 1) return "one";
                if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return "few";
                return "many";
            case "cs": case "sk":
                if (a == 1) return "one";
                return a >= 2 && a <= 4 ? "few" : "other";
            default:
                return a == 1 ? "one" : "other";
        }
    }

    /// <summary>string.Format with the culture; a template that is not a valid composite format comes back unformatted.</summary>
    public static string Apply(string template, CultureInfo culture, object?[] args)
    {
        if (args == null || args.Length == 0) return template;
        try { return string.Format(culture, template, args); }
        catch (FormatException) { return template; }
    }
}

// The reference grammar (same as aver::fmt::parseLocRef): "@" Table ":" Key [ "=" Default ].
// Table [A-Za-z0-9_]+, Key [A-Za-z0-9_.-]+, the default is everything after the first '='.
internal readonly struct LocRef
{
    internal string Table { get; }
    internal string Key { get; }
    internal string Default { get; }
    internal bool HasDefault { get; }

    private LocRef(string table, string key, string def, bool has) { Table = table; Key = key; Default = def; HasDefault = has; }

    internal string Fallback() => HasDefault ? Default : "[" + Table + ":" + Key + "]";

    internal static bool Valid(string table, string key)
    {
        if (string.IsNullOrEmpty(table) || string.IsNullOrEmpty(key)) return false;
        foreach (char c in table) if (!IsWord(c)) return false;
        foreach (char c in key) if (!IsWord(c) && c != '.' && c != '-') return false;
        return true;
    }

    internal static bool TryParse(string? s, out LocRef r)
    {
        r = default;
        if (s == null || s.Length < 4 || s[0] != '@' || s[1] == '@') return false;
        int i = 1;
        while (i < s.Length && IsWord(s[i])) i++;
        if (i == 1 || i >= s.Length || s[i] != ':') return false;
        int tableEnd = i++;
        int keyStart = i;
        while (i < s.Length && (IsWord(s[i]) || s[i] == '.' || s[i] == '-')) i++;
        if (i == keyStart || (i < s.Length && s[i] != '=')) return false;
        bool has = i < s.Length;
        r = new LocRef(s.Substring(1, tableEnd - 1), s.Substring(keyStart, i - keyStart), has ? s.Substring(i + 1) : string.Empty, has);
        return true;
    }

    private static bool IsWord(char c) => (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
