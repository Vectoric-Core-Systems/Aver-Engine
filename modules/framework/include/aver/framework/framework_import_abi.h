#ifndef AVER_FRAMEWORK_IMPORT_ABI_H
#define AVER_FRAMEWORK_IMPORT_ABI_H

/* Model import C ABI (Aver.Framework): import a model file (FBX, glTF/GLB) into project Content, relayed to
 * the editor.
 *
 * Importing is an editor-time act, so this is a thin relay and not a gameplay system. Aver.Framework does
 * not link the importers; the EDITOR installs ONE provider (sandbox/src/ModelImportProvider.cpp) and every
 * call below forwards to it. A packaged game never installs one, so there every call returns 0 and writes
 * "model import is not available in this host" -- the answer, not a crash. Same conventions as
 * framework_abi.h (1 on success, 0 on refusal). Own version, separate from AVER_FW_ABI_VERSION_MINOR.
 * The provider runs synchronously on the calling thread. Design: docs/plans/wave9/FBX_IMPORT.md 2.10. */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI, AVER_FW_CALL */

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_FW_IMPORT_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_import_abi_version(void);

/* Provider: imports `utf8Source` into the Content folder `utf8DestFolder` (Content-relative; the provider
 * refuses `..`, absolute paths and engine content) with the `utf8Options` text ("scale=100;rootMotion=auto",
 * empty = defaults). Writes a NUL-terminated message into outMessage (messageCap bytes, may be null) and
 * five counts into outCounts5: meshes, skeletons, clips, materials, textures written. Returns 1 on success. */
typedef int32_t (AVER_FW_CALL* aver_fw_import_fn)(const char* utf8Source, const char* utf8DestFolder,
                                                   const char* utf8Options, int32_t overwrite,
                                                   char* outMessage, int32_t messageCap, int32_t* outCounts5,
                                                   void* user);
/* Installs the provider. Passing null clears it. Always returns 1. */
AVER_FW_ABI int32_t aver_fw_set_import_provider(aver_fw_import_fn fn, void* user);

/* 1 when a provider is installed. */
AVER_FW_ABI int32_t aver_fw_import_available(void);

/* Relays to the provider. 1 on success; 0 when it failed or no provider is installed (the message says
 * which, and the counts are zero). outMessage and outCounts5 may be null. */
AVER_FW_ABI int32_t aver_fw_import_model(const char* utf8Source, const char* utf8DestFolder, const char* utf8Options,
                                         int32_t overwrite, char* outMessage, int32_t messageCap,
                                         int32_t* outCounts5);

#ifdef __cplusplus
}
#endif

#endif /* AVER_FRAMEWORK_IMPORT_ABI_H */
