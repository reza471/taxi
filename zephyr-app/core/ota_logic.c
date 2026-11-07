#include "ota_logic.h"
#include <stdio.h>
#include <string.h>

#define MCUBOOT_MAGIC 0x96f3b83dU

bool ota_parse_version(const char *s, ota_ver_t *v)
{
	unsigned parts[3] = {0, 0, 0};
	int n = 0;

	if (!s) {
		return false;
	}
	if (*s == 'v' || *s == 'V') {
		s++;
	}
	if (*s < '0' || *s > '9') {
		return false;
	}
	for (;;) {
		unsigned val = 0;
		int digits = 0;

		while (*s >= '0' && *s <= '9') {
			val = val * 10 + (unsigned)(*s - '0');
			if (++digits > 5 || val > 65535) {
				return false;
			}
			s++;
		}
		if (digits == 0) {
			return false; /* "1..2", "1." */
		}
		parts[n++] = val;
		if (*s == '.') {
			if (n == 3) {
				return false; /* 4 parts */
			}
			s++;
			continue;
		}
		break;
	}
	if (*s != '\0' || n < 2 || parts[0] > 255 || parts[1] > 255) {
		return false;
	}
	v->major = (uint8_t)parts[0];
	v->minor = (uint8_t)parts[1];
	v->patch = (uint16_t)parts[2];
	return true;
}

int ota_ver_cmp(const ota_ver_t *a, const ota_ver_t *b)
{
	if (a->major != b->major) {
		return a->major < b->major ? -1 : 1;
	}
	if (a->minor != b->minor) {
		return a->minor < b->minor ? -1 : 1;
	}
	if (a->patch != b->patch) {
		return a->patch < b->patch ? -1 : 1;
	}
	return 0;
}

void ota_ver_str(const ota_ver_t *v, char *out, size_t size)
{
	snprintf(out, size, "%u.%u.%u", v->major, v->minor, v->patch);
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

bool ota_header_parse(const uint8_t *hdr, size_t len, ota_ver_t *ver, uint32_t *img_size)
{
	if (!hdr || len < OTA_HEADER_MIN) {
		return false;
	}
	if (le32(hdr) != MCUBOOT_MAGIC) {
		return false;
	}
	uint16_t hdr_size = le16(hdr + 8);
	uint32_t size = le32(hdr + 12);

	if (hdr_size < 0x20 || hdr_size > 0x400 || size == 0 || size > 0x10000000U) {
		return false;
	}
	if (ver) {
		ver->major = hdr[20];
		ver->minor = hdr[21];
		ver->patch = le16(hdr + 22);
	}
	if (img_size) {
		*img_size = size;
	}
	return true;
}

bool ota_url_parse(const char *url, char *host, size_t host_size, uint16_t *port, char *path,
		   size_t path_size)
{
	static const char prefix[] = "https://";
	size_t n = 0;

	if (!url || strncmp(url, prefix, sizeof(prefix) - 1) != 0) {
		return false; /* no plain http, no other schemes */
	}
	const char *p = url + sizeof(prefix) - 1;

	/* host */
	while (*p && *p != ':' && *p != '/') {
		char c = *p;

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		      c == '-' || c == '.')) {
			return false;
		}
		if (n + 1 >= host_size || n + 1 >= OTA_HOST_MAX) {
			return false;
		}
		host[n++] = c;
		p++;
	}
	host[n] = '\0';
	if (n == 0 || host[0] == '.' || host[0] == '-') {
		return false;
	}

	/* port */
	unsigned pt = 443;

	if (*p == ':') {
		p++;
		pt = 0;
		int digits = 0;

		while (*p >= '0' && *p <= '9') {
			pt = pt * 10 + (unsigned)(*p - '0');
			if (++digits > 5 || pt > 65535) {
				return false;
			}
			p++;
		}
		if (digits == 0 || pt == 0) {
			return false;
		}
	}
	*port = (uint16_t)pt;

	/* path: must start with '/', printable ASCII only, no spaces */
	if (*p != '/') {
		return false;
	}
	size_t m = 0;

	while (*p) {
		unsigned char c = (unsigned char)*p;

		if (c <= 0x20 || c >= 0x7F || c == '"' || c == '<' || c == '>' || c == '\\') {
			return false;
		}
		if (m + 1 >= path_size || m + 1 >= OTA_PATH_MAX) {
			return false;
		}
		path[m++] = (char)c;
		p++;
	}
	path[m] = '\0';
	return true;
}

bool ota_should_download(const ota_ver_t *current, const ota_ver_t *offered, const char *failed)
{
	ota_ver_t f;

	if (ota_ver_cmp(offered, current) <= 0) {
		return false; /* same or older: never downgrade */
	}
	if (failed && failed[0] && ota_parse_version(failed, &f) && ota_ver_cmp(offered, &f) == 0) {
		return false; /* this exact version already failed on this device */
	}
	return true;
}
