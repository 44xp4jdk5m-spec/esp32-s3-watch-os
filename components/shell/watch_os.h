#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Watch boot sequence + main loop.  Runs forever; never returns. */
void watch_os_run(void);

#ifdef __cplusplus
}
#endif
