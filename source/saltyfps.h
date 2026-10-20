/* saltyfps.h -- publish our frame pacing to the SaltySD shared block that
 * FPSLocker and Status Monitor read.
 *
 * Those overlays do not look inside a process. They connect to the "SaltySD"
 * named port, map a 4 KiB block the sysmodule hands out, and scan it for the
 * magic 0x465053; whoever wrote that block owns the FPS readout. Normally that
 * is the SaltyNX plugin injected into a retail title, and it finds its hooks
 * through nn::ro::LookupSymbol -- which never resolves for a homebrew NRO, so
 * without this module the overlays would sit at "waiting for frame" forever.
 *
 * Layout source of truth: masagrator/SaltyNX tag 2.1.3 (a3fee3f), file
 * saltysd_core/source/NX-FPS-Common.hpp. That is the version shipped inside the
 * NX Venom bundle, and the first 174 bytes are identical in the older 174-byte
 * generation that berlint-hub/FPSLocker still declares, so one writer serves
 * both.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __SALTYFPS_H__
#define __SALTYFPS_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SALTYFPS_MAGIC      0x465053u /* "\x00FPS" little-endian */
#define SALTYFPS_BLOCK_SIZE 310u
#define SALTYFPS_API_VULKAN 3u
#define SALTYFPS_SHMEM_SIZE 0x1000u

/* Attach once, after the services we need are up. Returns false and stays
 * permanently inert when the SaltySD port is missing, so a console without
 * SaltyNX behaves exactly as it did before this module existed. Never throws,
 * never blocks for more than a couple of milliseconds. */
bool saltyfps_init(void);

/* Called on every present, on whichever path served it. Cheap: one tick read
 * plus a handful of stores into an already-mapped page. */
void saltyfps_present(void);

/* Report the swapchain image count so the overlay can show real buffering.
 * Passing 0 leaves the fields untouched. */
void saltyfps_set_buffers(int count);

/* FPS target set by an overlay (FPSLocker writes FPSlocked, or
 * FPSlockedDocked when the console is docked), or 0 when nothing is set.
 * Values are clamped to 1..120. */
int saltyfps_target_fps(void);

/* The panel's actual refresh rate, as reported by the SaltySD sysmodule in
 * currentRefreshRate -- the only thing on the console that can change it, and the
 * field Display Sync writes through. Returns 0 when the value is absent or out of
 * a plausible range, so callers must treat 0 as "unknown", not as "no limit". */
int saltyfps_refresh_rate(void);   /* 40..120, or 0 = unknown */

/* True while we are the ones publishing. Goes false if another writer takes
 * the block over, which is the case we must lose on purpose. */
bool saltyfps_active(void);

/* One line for the diagnostic echo, e.g.
 * "offset=320 adopted=0 api=3 fps=50 avg=50.2 lock=0 rr=60 frames=1234". */
void saltyfps_status(char *out, size_t out_size);

#endif /* __SALTYFPS_H__ */
