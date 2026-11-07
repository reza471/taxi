#ifndef HW_H
#define HW_H
#include <stdint.h>
#include <stdbool.h>

/* ---- pulse.c ---- */
void pulse_init(void);
/* Returns pulses since last call and clears the counter (atomic). */
uint32_t pulse_take(void);

/* ---- keys.c ---- */
#define KEY_COUNT 5 /* K1..K5 are index 0..4 */

typedef struct {
	bool pressed[KEY_COUNT];       /* press edge since last key_poll() */
	bool released[KEY_COUNT];      /* release edge since last key_poll() */
	bool still[KEY_COUNT];         /* key is down now */
	int32_t hold_ms;               /* how long the current key is held */
	int32_t rest_ms;               /* how long no key is down */
} key_snapshot_t;

void keys_init(void);
/* Copy the edge flags that happened since the last call, then clear them. */
void key_poll(key_snapshot_t *out);
/* ms with no key pressed (for the network "idle" rule). */
int32_t key_rest_ms(void);

/* ---- display.c ---- */
void display_init(void);
/* Text for the big field (8 chars) and the small field (4 chars).
 * A '.' attaches a decimal point to the previous digit and uses no digit. */
void display_set(const char *big8, const char *small4, bool show_colon);
void display_set_wifi(bool connected);

/* ---- beep.c ---- */
void beep_init(void);
void beep(uint32_t freq_hz, uint32_t ms); /* non-blocking */

/* ---- clock.c ---- */
void clock_set_epoch(int64_t epoch_s, int32_t tz_offset_s);
bool clock_is_set(void);
void clock_get_hm(int *h, int *m);

#endif
