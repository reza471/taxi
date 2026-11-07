#include "../core/urlq.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	char b[200];
	urlq_t q;

	/* normal request, same layout as the old firmware: "?a=1&b=x" */
	urlq_init(&q, b, sizeof(b), "/api/taxi/SetTaxiEnd?");
	urlq_str(&q, "serial", "08A6F7A6A30C");
	urlq_int(&q, "bt_number", 5);
	urlq_str(&q, "start_time", "13:05");
	urlq_int(&q, "total_price", 123456789);
	assert(urlq_ok(&q));
	assert(!strcmp(b, "/api/taxi/SetTaxiEnd?serial=08A6F7A6A30C&bt_number=5&start_time=13:05&total_price=123456789"));

	/* the old service URL has the quirk "?&serial=..." and the server may rely on it */
	urlq_init(&q, b, sizeof(b), "/api/taxi/StartService?&");
	urlq_str(&q, "serial", "AB");
	urlq_int(&q, "serviceNum", 211);
	urlq_int(&q, "start", 1);
	assert(!strcmp(b, "/api/taxi/StartService?&serial=AB&serviceNum=211&start=1"));

	/* values can not break the request */
	urlq_init(&q, b, sizeof(b), "/x?");
	urlq_str(&q, "a", "x&evil=1 y=2%3\xc3\xa9\n#");
	assert(urlq_ok(&q));
	assert(!strcmp(b, "/x?a=x%26evil%3D1%20y%3D2%253%C3%A9%0A%23"));
	urlq_init(&q, b, sizeof(b), "/x?");
	urlq_str(&q, "a", NULL);
	assert(!strcmp(b, "/x?a="));

	/* integers */
	urlq_init(&q, b, sizeof(b), "/x?");
	urlq_int(&q, "n", 0);
	urlq_int(&q, "m", -45);
	urlq_int(&q, "big", INT64_MAX);
	urlq_int(&q, "min", INT64_MIN);
	assert(!strcmp(b, "/x?n=0&m=-45&big=9223372036854775807&min=-9223372036854775808"));

	/* overflow is detected and the buffer stays terminated */
	for (size_t size = 1; size < 40; size++) {
		char small[40];

		memset(small, 'Z', sizeof(small));
		urlq_init(&q, small, size, "/api/taxi/SetTaxiEnd?");
		urlq_str(&q, "serial", "08A6F7A6A30C");
		urlq_int(&q, "total", 123456);
		assert(!urlq_ok(&q));
		assert(strlen(small) < size);
	}
	char exact[64];

	urlq_init(&q, exact, 6, "/x?");
	urlq_int(&q, "a", 1); /* "/x?a=1" needs 7 bytes with the terminator */
	assert(!urlq_ok(&q));
	urlq_init(&q, exact, 7, "/x?");
	urlq_int(&q, "a", 1);
	assert(urlq_ok(&q) && !strcmp(exact, "/x?a=1"));

	puts("all urlq tests passed");
	return 0;
}
