/*
 * Safe builder for "path?key=value&key=value" request strings (pure C).
 * Every value is percent-encoded, so a space, '&', '=', '%' or any byte can never break the
 * request or inject another parameter. Overflow is detected, never silently cut.
 */
#ifndef URLQ_H
#define URLQ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
	char *buf;
	size_t size;
	size_t len;
	bool first;     /* next parameter is the first one (no '&' before it) */
	bool overflow;
} urlq_t;

/* `head` is copied as it is, e.g. "/api/taxi/SetTaxiEnd?" (it must end with '?' or '&'). */
void urlq_init(urlq_t *q, char *buf, size_t size, const char *head);
void urlq_str(urlq_t *q, const char *key, const char *value); /* value is percent-encoded */
void urlq_int(urlq_t *q, const char *key, int64_t value);
/* true if everything fitted. The buffer is always NUL terminated. */
bool urlq_ok(const urlq_t *q);

#endif
