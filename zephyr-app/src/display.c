/*
 * 12-digit 7-segment display on a HT16K33 (I2C 0x70).
 * Wiring is custom: segments a..dp are on COM0..COM7, digits are on the ROW pins.
 * Same digit remap and buffer layout as the Arduino version.
 */
#include "hw.h"
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(display, LOG_LEVEL_INF);

#define HT16K33_ADDR 0x70
#define FRAME_MS 100

static const struct device *const i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));

/* ASCII 32..127 -> segments (bit0..6 = a..g, bit7 = dp) */
static const uint8_t font[96] = {

    0x00, /* (space) */
    0x86, /* ! */
    0x22, /* " */
    0x7E, /* # */
    0x6D, /* $ */
    0xD2, /* % */
    0x46, /* & */
    0x20, /* ' */
    0x29, /* ( */
    0x0B, /* ) */
    0x21, /* * */
    0x70, /* + */
    0x10, /* , */
    0x40, /* - */
    0x80, /* . */
    0x52, /* / */
    0x3F, /* 0 */
    0x06, /* 1 */
    0x5B, /* 2 */
    0x4F, /* 3 */
    0x66, /* 4 */
    0x6D, /* 5 */
    0x7D, /* 6 */
    0x07, /* 7 */
    0x7F, /* 8 */
    0x6F, /* 9 */
    0x09, /* : */
    0x0D, /* ; */
    0x61, /* < */
    0x48, /* = */
    0x43, /* > */
    0xD3, /* ? */
    0x5F, /* @ */
    0x77, /* A */
    0x7C, /* B */
    0x39, /* C */
    0x5E, /* D */
    0x79, /* E */
    0x71, /* F */
    0x3D, /* G */
    0x76, /* H */
    0x30, /* I */
    0x1E, /* J */
    0x75, /* K */
    0x38, /* L */
    0x15, /* M */
    0x37, /* N */
    0x3F, /* O */
    0x73, /* P */
    0x6B, /* Q */
    0x33, /* R */
    0x6D, /* S */
    0x78, /* T */
    0x3E, /* U */
    0x3E, /* V */
    0x2A, /* W */
    0x76, /* X */
    0x6E, /* Y */
    0x5B, /* Z */
    0x39, /* [ */
    0x64, /* \ */
    0x0F, /* ] */
    0x23, /* ^ */
    0x08, /* _ */
    0x02, /* ` */
    0x5F, /* a */
    0x7C, /* b */
    0x58, /* c */
    0x5E, /* d */
    0x7B, /* e */
    0x71, /* f */
    0x6F, /* g */
    0x74, /* h */
    0x10, /* i */
    0x0C, /* j */
    0x75, /* k */
    0x30, /* l */
    0x14, /* m */
    0x54, /* n */
    0x5C, /* o */
    0x73, /* p */
    0x67, /* q */
    0x50, /* r */
    0x6D, /* s */
    0x78, /* t */
    0x1C, /* u */
    0x1C, /* v */
    0x14, /* w */
    0x76, /* x */
    0x6E, /* y */
    0x5B, /* z */
    0x46, /* { */
    0x30, /* | */
    0x70, /* } */
    0x01, /* ~ */
    0x00, /* (del) */
};

static K_MUTEX_DEFINE(lock);
static char text[12 + 1] = "            ";
static uint8_t dp_mask[12]; /* dp requested by a '.' in the text */
static bool colon_on;
static bool wifi_ok;

/* Parse "123.4" style text into `width` digit characters.
 * '.' sets the dp of the previous digit. Result is padded with spaces. */
static void parse_field(const char *s, int width, char *chars, uint8_t *dps)
{
	int n = 0;

	memset(chars, ' ', width);
	memset(dps, 0, width);

	for (const char *p = s; *p && n < width; p++) {
		if (*p == '.') {
			if (n > 0) {
				dps[n - 1] = 0x80;
			}
			continue;
		}
		chars[n++] = *p;
	}
}

void display_set(const char *big8, const char *small4, bool show_colon)
{
	char c1[8], c2[4];
	uint8_t d1[8], d2[4];

	parse_field(big8, 8, c1, d1);
	parse_field(small4, 4, c2, d2);

	k_mutex_lock(&lock, K_FOREVER);
	memcpy(text, c1, 8);
	memcpy(text + 8, c2, 4);
	memcpy(dp_mask, d1, 8);
	memcpy(dp_mask + 8, d2, 4);
	colon_on = show_colon;
	k_mutex_unlock(&lock);
}

void display_set_wifi(bool connected)
{
	wifi_ok = connected;
}

/* logical index (0..7 big field, 8..11 small field) -> physical position */
static const uint8_t phys_to_logical[12] = {7, 8, 9, 10, 11, 3, 2, 0, 1, 4, 5, 6};

static void build_frame(uint8_t buf[16], const char *txt, const uint8_t *dps, bool colon)
{
	uint8_t seg[12];

	for (int i = 0; i < 12; i++) {
		unsigned char ch = (unsigned char)txt[i];

		if (ch < 32 || ch > 127) {
			ch = ' ';
		}
		seg[i] = font[ch - 32] | dps[i];
	}
	/* colon = dp of the 2nd char of the small field (logical 9) */
	if (colon) {
		seg[9] |= 0x80;
	} else {
		seg[9] &= (uint8_t)~0x80;
	}

	uint8_t phys[12];

	for (int p = 0; p < 12; p++) {
		phys[p] = seg[phys_to_logical[p]];
	}

	memset(buf, 0, 16);
	for (int s = 0; s < 8; s++) {
		uint8_t lower = 0, upper = 0;

		for (int i = 0; i < 5; i++) { /* ROW3..ROW7 */
			lower <<= 1;
			if (phys[i] & (1 << s)) {
				lower |= 1;
			}
		}
		for (int i = 5; i < 12; i++) { /* ROW8..ROW14 */
			upper <<= 1;
			if (phys[i] & (1 << s)) {
				upper |= 1;
			}
		}
		buf[2 * s] = (uint8_t)(lower << 3);
		buf[2 * s + 1] = upper;
	}
}

static void display_thread(void *a, void *b, void *c)
{
	uint8_t frame[17]; /* address byte + 16 data bytes */
	int64_t next = k_uptime_get();

	while (1) {
		next += FRAME_MS;

		char txt[12];
		uint8_t dps[12];
		bool colon;

		k_mutex_lock(&lock, K_FOREVER);
		memcpy(txt, text, 12);
		memcpy(dps, dp_mask, 12);
		colon = colon_on && ((k_uptime_get() / 500) & 1);
		k_mutex_unlock(&lock);

		build_frame(&frame[1], txt, dps, colon);

		/* status LEDs on ROW15 (bit 7 of the high bytes of COM0 and COM1) */
		bool blink = (k_uptime_get() / 500) & 1;

		if (wifi_ok) {
			frame[1 + 1] |= 0x80;
			frame[1 + 3] &= (uint8_t)~0x80;
		} else {
			frame[1 + 1] &= (uint8_t)~0x80;
			if (blink) {
				frame[1 + 3] |= 0x80;
			} else {
				frame[1 + 3] &= (uint8_t)~0x80;
			}
		}

		frame[0] = 0x00; /* RAM start address */
		int err = i2c_write(i2c_dev, frame, sizeof(frame), HT16K33_ADDR);

		if (err) {
			LOG_WRN_ONCE("display i2c write failed: %d", err);
		}

		k_sleep(K_TIMEOUT_ABS_MS(next));
	}
}

K_THREAD_DEFINE(display_tid, 2048, display_thread, NULL, NULL, NULL, 8, 0, 300);

void display_init(void)
{
	static const uint8_t cmds[] = {
		0x21, /* oscillator on */
		0x81, /* display on, no blink */
		0xE0, /* brightness level 0 (0xE0..0xEF) */
	};

	if (!device_is_ready(i2c_dev)) {
		LOG_ERR("i2c0 not ready");
		return;
	}
	for (size_t i = 0; i < ARRAY_SIZE(cmds); i++) {
		i2c_write(i2c_dev, &cmds[i], 1, HT16K33_ADDR);
	}
}
