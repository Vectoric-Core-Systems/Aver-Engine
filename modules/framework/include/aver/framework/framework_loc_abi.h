#ifndef AVER_FRAMEWORK_LOC_ABI_H
#define AVER_FRAMEWORK_LOC_ABI_H

/* Localization C ABI (Aver.Framework): string tables per culture, a fallback chain, text references.
 * Separate header with its own version so framework_abi.h is untouched (framework_timers_abi.h is the
 * precedent). Conventions as there: int32_t/float/const char* only, UTF-8, single game thread, a returned
 * const char* is valid until the next call that returns a string. Semantics: docs/LOCALIZATION.md.
 *
 * A text reference is "@Table:Key" or "@Table:Key=Default" (the whole raw string); "@@x" is the literal
 * "@x"; any other string is literal text. */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI */

#ifdef __cplusplus
extern "C" {
#endif

/* 2: aver_fw_loc_tr / aver_fw_loc_tr_n (gettext messages from .po files, Godot's tr). */
#define AVER_FW_LOC_ABI_VERSION 2
AVER_FW_ABI int32_t aver_fw_loc_abi_version(void);

/* Scans utf8Dir (e.g. <content>/Localization) for culture folders; utf8DefaultCulture is LOCALE.DEFAULT
 * ("" = "en"). Replaces any previous state, loads the default culture, makes it active. Returns the
 * number of culture folders found, -1 if utf8Dir is not a directory (the store stays usable: everything
 * resolves to inline defaults). */
AVER_FW_ABI int32_t aver_fw_loc_init(const char* utf8Dir, const char* utf8DefaultCulture);
AVER_FW_ABI void    aver_fw_loc_clear(void);                       /* back to the empty store */
AVER_FW_ABI int32_t aver_fw_loc_culture_count(void);               /* folders found, normalised, sorted */
AVER_FW_ABI const char* aver_fw_loc_culture_at(int32_t index);     /* "" out of range */
AVER_FW_ABI const char* aver_fw_loc_default_culture(void);
AVER_FW_ABI const char* aver_fw_loc_culture(void);                 /* active, normalised */
/* -1 malformed tag (nothing changes); 0 set, but neither it nor any parent has tables (resolves from the
 * fallback); 1 set. Loads the new chain's tables lazily, bumps the epoch, and POSTS the framework event
 * "loc.culture_changed" (aver_fw_event_post, one string arg = the culture) so AN_OnEvent graphs can react.
 * Setting the culture that is already active changes nothing and posts nothing. */
AVER_FW_ABI int32_t aver_fw_loc_set_culture(const char* utf8Tag);
AVER_FW_ABI int32_t aver_fw_loc_epoch(void);                       /* bumps on init/set_culture/reload/clear */

/* The relay target (aver_ui_set_text_resolver takes it directly). utf8Ref is the WHOLE raw string. Returns
 * -1 when it is not a reference (caller keeps the literal). Otherwise returns the byte length of the
 * resolved text and copies up to cap-1 bytes + NUL into out (out NULL or cap <= 0: length only; a return
 * >= cap means truncated, retry with a bigger buffer). Never fails for a valid reference: it falls through
 * the chain, then the inline default, then "[Table:Key]". Records misses (below). */
AVER_FW_ABI int32_t aver_fw_loc_resolve(const char* utf8Ref, char* out, int32_t cap);
/* Table/key lookup through the chain WITHOUT default or bracket fallback. Same length/copy rule; -1 if absent. */
AVER_FW_ABI int32_t aver_fw_loc_get(const char* utf8Table, const char* utf8Key, char* out, int32_t cap);
AVER_FW_ABI int32_t aver_fw_loc_has(const char* utf8Table, const char* utf8Key);
/* A file under <dir>/<culture>/ changed or was deleted on disk. If its culture is loaded, reparse that table
 * (a failed parse keeps the old table and logs; a deleted file drops it), bump the epoch. 1 if it was a
 * localization file, else 0. */
AVER_FW_ABI int32_t aver_fw_loc_reload_path(const char* utf8AbsPath);
/* "Table:Key" requested since init/clear that no culture in the chain had, unique, in first-hit order. */
/* A message by its source text (Godot's tr), from the .po files of the culture chain: the translation, or the
 * source text itself when no culture has one. `context` is msgctxt, null or "" for none. Same length/copy rule. (2) */
AVER_FW_ABI int32_t aver_fw_loc_tr(const char* utf8Message, const char* utf8Context, char* out, int32_t cap);
/* The plural form for count `n` (Godot's tr_n): msgstr[0] for n == 1, msgstr[1] otherwise; the source singular or
 * plural when untranslated. (2) */
AVER_FW_ABI int32_t aver_fw_loc_tr_n(const char* utf8Message, const char* utf8Plural, int32_t n, const char* utf8Context,
                                     char* out, int32_t cap);
AVER_FW_ABI int32_t aver_fw_loc_missing_count(void);
AVER_FW_ABI const char* aver_fw_loc_missing_at(int32_t index);
AVER_FW_ABI void    aver_fw_loc_missing_clear(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AVER_FRAMEWORK_LOC_ABI_H */
