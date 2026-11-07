#ifndef OTA_H
#define OTA_H
#include <stdbool.h>

typedef enum {
	OTA_DISABLED,    /* built without MCUboot: no update possible */
	OTA_IDLE,        /* nothing to do */
	OTA_DOWNLOADING, /* percent is valid */
	OTA_READY,       /* downloaded, will reboot when the meter is idle */
	OTA_TESTING,     /* a new image runs and is not confirmed yet */
	OTA_FAILED,      /* last attempt failed */
} ota_state_t;

/* The server tariff may carry "fw_version" and "fw_url". Called after a tariff was accepted;
 * NULL, NULL means "no offer". Net thread only. */
void ota_offer(const char *version, const char *url);

/* Net thread, once at start: finds out if this boot is a test run or a rollback. */
void ota_boot_check(void);

/* Net thread, every loop while WiFi is up: download, reboot, confirm. */
void ota_poll(void);

ota_state_t ota_status(int *percent);
#endif
