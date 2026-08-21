#ifndef AVER_SETTINGS_ABI_H
#define AVER_SETTINGS_ABI_H

/* Aver.Settings — durable key/value a SHIPPED GAME can read and write.
 *
 * WHY THIS EXISTS SEPARATELY FROM A SAVE. A save is a world; settings outlive every world and
 * belong to the player, not to a playthrough. Deleting a save must not reset the volume, and
 * loading one must not change the resolution. Two lifetimes, two files.
 *
 * WHY IT IS NOT EditorPrefs. sandbox/src/EditorPrefs.cpp already does exactly this shape and is
 * `namespace aver::editor` inside the editor executable -- `grep -rn "EditorPrefs"
 * modules/runtime.game` returns nothing. A shipped game could not reach it, which is why the engine
 * had nowhere at all to put a chosen setting.
 *
 * A SHARED LIBRARY WITH A PLAIN C SEAM, like Aver.Physics, because the C# layer P/Invokes straight
 * into it and one binary means one store. No relay through the framework: settings depend on
 * nothing above Core and Platform, so there is no link edge to avoid. */

#include <stdint.h>

#if defined(_WIN32)
#  if defined(AVER_SETTINGS_BUILD)
#    define AVER_SETTINGS_API __declspec(dllexport)
#  else
#    define AVER_SETTINGS_API __declspec(dllimport)
#  endif
#else
#  define AVER_SETTINGS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Opens (or creates) the store at `utf8Path`. Reading a file that does not exist is not an error --
 * a first run has no settings and that is the ordinary case. Returns 1 unless the path is empty.
 * Calling it again with a different path closes the first WITHOUT flushing it; call
 * aver_settings_flush yourself if you meant to keep it. */
AVER_SETTINGS_API int32_t aver_settings_open(const char* utf8Path);

/* The default location: <user data dir>/Aver/settings.ini. Returned as a UTF-8 pointer the caller
 * must not free, valid until the next call. A game that wants its own file passes its own path to
 * aver_settings_open instead; the engine does not decide where a project's settings live. */
AVER_SETTINGS_API const char* aver_settings_default_path(void);

/* Writes the file if anything changed since the last flush. Returns 1 when the file is on disk and
 * current -- INCLUDING when nothing needed writing. 0 only when a write was needed and failed.
 *
 * ATOMIC: temp file, then rename. Settings are small and rewritten often, and half a settings file
 * is a game that will not start. */
AVER_SETTINGS_API int32_t aver_settings_flush(void);

/* Readers. Each returns `fallback` for a missing key, and for a value that does not parse as the
 * requested type -- a hand-edited file with `volume=loud` reads as the fallback rather than 0. */
AVER_SETTINGS_API float   aver_settings_get_f32 (const char* key, float   fallback);
AVER_SETTINGS_API int32_t aver_settings_get_i32 (const char* key, int32_t fallback);
AVER_SETTINGS_API int32_t aver_settings_get_bool(const char* key, int32_t fallback);
/* Returns a UTF-8 pointer the caller must not free, valid until the next set or open. */
AVER_SETTINGS_API const char* aver_settings_get_str(const char* key, const char* fallback);

/* Writers. Each returns 1 on success, 0 for an empty key or a value that cannot be stored.
 * Writing marks the store dirty; nothing reaches disk until aver_settings_flush. */
AVER_SETTINGS_API int32_t aver_settings_set_f32 (const char* key, float   value);
AVER_SETTINGS_API int32_t aver_settings_set_i32 (const char* key, int32_t value);
AVER_SETTINGS_API int32_t aver_settings_set_bool(const char* key, int32_t value);
/* A value containing a newline is REFUSED rather than written: the format is one key=value per
 * line with no escaping, and a smuggled newline would silently become a second key. */
AVER_SETTINGS_API int32_t aver_settings_set_str (const char* key, const char* value);

/* 1 when the key is present, whatever its type. */
AVER_SETTINGS_API int32_t aver_settings_has(const char* key);
/* Removes a key. 1 if it is gone afterwards, INCLUDING when it never existed. */
AVER_SETTINGS_API int32_t aver_settings_remove(const char* key);
/* How many keys the store holds. Mostly for tests and for a settings screen that wants to say
 * "nothing has been changed from its default yet". */
AVER_SETTINGS_API int32_t aver_settings_count(void);

#ifdef __cplusplus
}
#endif

#endif /* AVER_SETTINGS_ABI_H */
