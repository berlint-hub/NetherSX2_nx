/* imports.h -- .so import resolution
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __IMPORTS_H__
#define __IMPORTS_H__

#include <stdio.h>
#include <stdlib.h>
#include "so_util.h"

extern DynLibFunction dynlib_functions[];
extern size_t dynlib_numfunctions;

void update_imports(void);

// The core's saveStateSlot is asynchronous. Observe its terminal status message
// so the wrapper restores settings only after serialization completes.
void imports_savestate_watch_begin(void);
int imports_savestate_watch_result(void); // 0 pending, 1 saved, -1 failed
void imports_savestate_watch_end(void);

#endif
