#ifndef WDOG_H
#define WDOG_H
#include <stdint.h>

/* Task watchdog: every important thread registers and must feed it in its loop.
 * If a thread hangs, the chip resets itself (hardware fallback covers a dead scheduler). */
void wdog_init(void);
int wdog_add(uint32_t timeout_ms); /* returns a channel id, or -1 */
void wdog_feed(int id);
void log_reset_cause(void);
#endif
