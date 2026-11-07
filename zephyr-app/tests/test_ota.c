#include "../core/ota_logic.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_version(void)
{
	ota_ver_t v, w;
	char b[24];

	assert(ota_parse_version("1.2.3", &v) && v.major == 1 && v.minor == 2 && v.patch == 3);
	assert(ota_parse_version("v0.10", &v) && v.major == 0 && v.minor == 10 && v.patch == 0);
	assert(ota_parse_version("255.255.65535", &v));
	const char *bad[] = {"", "v", "1", "1.", ".1", "1..2", "1.2.3.4", "1.2.x", "a.b", "256.0", "0.256",
			     "1.2.65536", "1.2 ", " 1.2", "1.2-rc1", "-1.2", "1.2.3junk", "999999.1"};

	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		if (ota_parse_version(bad[i], &v)) {
			fprintf(stderr, "accepted bad version '%s'\n", bad[i]);
			assert(0);
		}
	}
	assert(!ota_parse_version(NULL, &v));

	ota_parse_version("0.1.0", &v);
	ota_parse_version("0.1.1", &w);
	assert(ota_ver_cmp(&v, &w) < 0 && ota_ver_cmp(&w, &v) > 0 && ota_ver_cmp(&v, &v) == 0);
	ota_parse_version("0.2.0", &w);
	assert(ota_ver_cmp(&v, &w) < 0);
	ota_parse_version("1.0.0", &w);
	assert(ota_ver_cmp(&v, &w) < 0);
	ota_parse_version("0.10.0", &v);
	ota_parse_version("0.9.0", &w);
	assert(ota_ver_cmp(&v, &w) > 0); /* numeric, not text order */
	ota_ver_str(&v, b, sizeof(b));
	assert(!strcmp(b, "0.10.0"));
}

static void put32(uint8_t *p, uint32_t x)
{
	p[0] = x; p[1] = x >> 8; p[2] = x >> 16; p[3] = x >> 24;
}

static void test_header(void)
{
	uint8_t h[32];
	ota_ver_t v;
	uint32_t size;

	memset(h, 0, sizeof(h));
	put32(h, 0x96f3b83d);
	h[8] = 0x20;              /* hdr_size */
	put32(h + 12, 720604);    /* img_size */
	h[20] = 0; h[21] = 2; h[22] = 5; h[23] = 0; /* 0.2.5 */
	assert(ota_header_parse(h, sizeof(h), &v, &size));
	assert(v.major == 0 && v.minor == 2 && v.patch == 5 && size == 720604);
	assert(ota_header_parse(h, sizeof(h), NULL, NULL));
	assert(!ota_header_parse(h, 31, &v, &size));      /* too short */
	assert(!ota_header_parse(NULL, 32, &v, &size));
	h[0] ^= 1;
	assert(!ota_header_parse(h, sizeof(h), &v, &size)); /* not an MCUboot image */
	h[0] ^= 1;
	put32(h + 12, 0);
	assert(!ota_header_parse(h, sizeof(h), &v, &size)); /* empty image */
	put32(h + 12, 0xFFFFFFFFu);
	assert(!ota_header_parse(h, sizeof(h), &v, &size)); /* silly size */
	put32(h + 12, 1000);
	h[8] = 0x10;
	assert(!ota_header_parse(h, sizeof(h), &v, &size)); /* header too small */
	/* the real header printed by imgtool for our build: magic, hdr 0x20, size 0xafebc, 0.0.0 */
	uint8_t real[32] = {0x3d, 0xb8, 0xf3, 0x96, 0, 0, 0, 0, 0x20, 0, 0, 0, 0xbc, 0xfe, 0x0a, 0};
	assert(ota_header_parse(real, sizeof(real), &v, &size) && size == 0xafebc);
}

static void test_url(void)
{
	char host[64], path[160];
	uint16_t port;

	assert(ota_url_parse("https://fw.example.ir/fw/taxi-0.2.0.signed.bin", host, sizeof(host), &port,
			     path, sizeof(path)));
	assert(!strcmp(host, "fw.example.ir") && port == 443 && !strcmp(path, "/fw/taxi-0.2.0.signed.bin"));
	assert(ota_url_parse("https://a-b.c.d:8443/x?y=1&z=2", host, sizeof(host), &port, path, sizeof(path)));
	assert(!strcmp(host, "a-b.c.d") && port == 8443 && !strcmp(path, "/x?y=1&z=2"));
	const char *bad[] = {
		"http://x.com/a", "ftp://x.com/a", "https://", "https:///a", "https://x.com", "https://x.com:/a",
		"https://x.com:0/a", "https://x.com:70000/a", "https://.x.com/a", "https://-x.com/a",
		"https://x_y.com/a", "https://user@x.com/a", "https://x.com/a b", "https://x.com/a\"b",
		"https://x.com/a\r\nHost: evil", "https://[::1]/a", "", "//x.com/a", "https://x.com/\\a",
	};

	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		if (ota_url_parse(bad[i], host, sizeof(host), &port, path, sizeof(path))) {
			fprintf(stderr, "accepted bad url '%s'\n", bad[i]);
			assert(0);
		}
	}
	/* too long host / path */
	char longu[400] = "https://";

	memset(longu + 8, 'a', 100);
	strcpy(longu + 108, "/x");
	assert(!ota_url_parse(longu, host, sizeof(host), &port, path, sizeof(path)));
	strcpy(longu, "https://x.com/");
	memset(longu + 14, 'a', 300);
	longu[314] = '\0';
	assert(!ota_url_parse(longu, host, sizeof(host), &port, path, sizeof(path)));
	/* output buffer smaller than the data */
	char tiny[4];

	assert(!ota_url_parse("https://example.com/a", tiny, sizeof(tiny), &port, path, sizeof(path)));
	assert(!ota_url_parse("https://x.com/abcdef", host, sizeof(host), &port, tiny, sizeof(tiny)));
	assert(!ota_url_parse(NULL, host, sizeof(host), &port, path, sizeof(path)));
}

static void test_decision(void)
{
	ota_ver_t cur, off;

	ota_parse_version("0.1.0", &cur);
	ota_parse_version("0.2.0", &off);
	assert(ota_should_download(&cur, &off, NULL));
	assert(ota_should_download(&cur, &off, ""));
	assert(ota_should_download(&cur, &off, "0.1.5"));    /* another version failed */
	assert(!ota_should_download(&cur, &off, "0.2.0"));   /* this one failed already */
	assert(!ota_should_download(&cur, &off, "v0.2"));    /* same version, other spelling */
	assert(ota_should_download(&cur, &off, "garbage"));
	assert(!ota_should_download(&off, &cur, NULL));      /* never downgrade */
	assert(!ota_should_download(&cur, &cur, NULL));      /* same version */
}

int main(void)
{
	test_version();
	test_header();
	test_url();
	test_decision();
	puts("all ota logic tests passed");
	return 0;
}
