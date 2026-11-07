/*
 * Pure logic of the OTA update (no flash, no network, no OS): easy to test on a PC.
 *   - version text "0.2.1" / "v0.2" and comparing versions
 *   - reading the MCUboot image header of the downloaded file
 *   - checking the download URL (https only, safe characters, length)
 *   - the decision "should this offer be downloaded?"
 */
#ifndef OTA_LOGIC_H
#define OTA_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OTA_VER_TEXT 16
#define OTA_HOST_MAX 64
#define OTA_PATH_MAX 160

typedef struct {
	uint8_t major;
	uint8_t minor;
	uint16_t patch;
} ota_ver_t;

/* "1.2.3", "1.2", "v1.2.3". Rejects anything else (empty, letters, 3+ dots, > 255 ...). */
bool ota_parse_version(const char *s, ota_ver_t *v);
/* <0, 0, >0 */
int ota_ver_cmp(const ota_ver_t *a, const ota_ver_t *b);
/* "major.minor.patch" */
void ota_ver_str(const ota_ver_t *v, char *out, size_t size);

/* MCUboot image header (little endian) at the start of the file. */
bool ota_header_parse(const uint8_t *hdr, size_t len, ota_ver_t *ver, uint32_t *img_size);
#define OTA_HEADER_MIN 32

/* "https://host[:port]/path?query". Only https. Host: letters, digits, '-' and '.'. */
bool ota_url_parse(const char *url, char *host, size_t host_size, uint16_t *port, char *path,
		   size_t path_size);

/* Download only a NEWER version that did not already fail before. `failed` may be "" or NULL. */
bool ota_should_download(const ota_ver_t *current, const ota_ver_t *offered, const char *failed);

#endif
