#include "urlq.h"
#include <string.h>

static void put(urlq_t *q, char c)
{
	if (q->len + 1 < q->size) {
		q->buf[q->len++] = c;
		q->buf[q->len] = '\0';
	} else {
		q->overflow = true;
	}
}

static void put_str(urlq_t *q, const char *s)
{
	while (*s) {
		put(q, *s++);
	}
}

void urlq_init(urlq_t *q, char *buf, size_t size, const char *head)
{
	q->buf = buf;
	q->size = size;
	q->len = 0;
	q->first = true;
	q->overflow = false;
	if (size) {
		buf[0] = '\0';
	}
	put_str(q, head);
}

static void sep(urlq_t *q)
{
	/* a head that already ends with '&' or '?' needs no extra '&' for the first parameter */
	if (q->first) {
		q->first = false;
		return;
	}
	put(q, '&');
}

static void put_key(urlq_t *q, const char *key)
{
	sep(q);
	put_str(q, key); /* keys are program constants */
	put(q, '=');
}

void urlq_str(urlq_t *q, const char *key, const char *value)
{
	static const char hex[] = "0123456789ABCDEF";

	put_key(q, key);
	for (const unsigned char *p = (const unsigned char *)value; p && *p; p++) {
		unsigned char c = *p;

		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		    c == '-' || c == '_' || c == '.' || c == '~' || c == ':') {
			put(q, (char)c); /* ':' is allowed in a query value and is what "13:05" needs */
		} else {
			put(q, '%');
			put(q, hex[c >> 4]);
			put(q, hex[c & 15]);
		}
	}
}

void urlq_int(urlq_t *q, const char *key, int64_t value)
{
	char tmp[24];
	int n = 0;
	bool neg = value < 0;
	uint64_t u = neg ? (uint64_t)0 - (uint64_t)value : (uint64_t)value;

	put_key(q, key);
	do {
		tmp[n++] = (char)('0' + (u % 10));
		u /= 10;
	} while (u && n < (int)sizeof(tmp));
	if (neg) {
		put(q, '-');
	}
	while (n) {
		put(q, tmp[--n]);
	}
}

bool urlq_ok(const urlq_t *q)
{
	return !q->overflow;
}
