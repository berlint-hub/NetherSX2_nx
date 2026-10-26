#ifndef NETHERSX2_FPS_MODS_H
#define NETHERSX2_FPS_MODS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NxFpsModInstallResult {
  NX_FPS_MOD_INSTALLED = 0,
  NX_FPS_MOD_ALREADY_INSTALLED,
  NX_FPS_MOD_UNSUPPORTED_GAME,
  NX_FPS_MOD_NETWORK_ERROR,
  NX_FPS_MOD_INVALID_SOURCE,
  NX_FPS_MOD_FILE_ERROR,
} NxFpsModInstallResult;

// The catalog is deliberately opt-in and game/CRC-specific. It must never
// infer a patch for an unlisted game from its title or region alone.
int nx_fps_mod_available(uint32_t crc);
int nx_fps_mod_installed(uint32_t crc, const char *pnach_path);
const char *nx_fps_mod_label(uint32_t crc);
NxFpsModInstallResult nx_fps_mod_install(uint32_t crc, const char *pnach_path);

#ifdef __cplusplus
}
#endif

#endif
