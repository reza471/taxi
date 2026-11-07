#include "json_lite.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	const char *p;
	const char *end;
	jl_cb cb;
	void *user;
	char path[JL_PATH_MAX];
	size_t path_len;
	int arr_idx; /* innermost array index, -1 outside arrays */
	char sbuf[JL_STR_MAX + 1];
	bool scut;
} ctx_t;

#define ERR_SYNTAX (-1)
#define ERR_DEPTH (-2)
#define ERR_TOP (-3)
#define ERR_LEN (-4)

static void skip_ws(ctx_t *c)
{
	while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) {
		c->p++;
	}
}

static void put_char(ctx_t *c, unsigned char ch, size_t *n)
{
	if (*n < JL_STR_MAX) {
		c->sbuf[(*n)++] = (char)ch;
	} else {
		c->scut = true;
	}
}

static void put_utf8(ctx_t *c, uint32_t cp, size_t *n)
{
	if (cp < 0x80) {
		put_char(c, (unsigned char)cp, n);
	} else if (cp < 0x800) {
		put_char(c, 0xC0 | (cp >> 6), n);
		put_char(c, 0x80 | (cp & 0x3F), n);
	} else if (cp < 0x10000) {
		put_char(c, 0xE0 | (cp >> 12), n);
		put_char(c, 0x80 | ((cp >> 6) & 0x3F), n);
		put_char(c, 0x80 | (cp & 0x3F), n);
	} else {
		put_char(c, 0xF0 | (cp >> 18), n);
		put_char(c, 0x80 | ((cp >> 12) & 0x3F), n);
		put_char(c, 0x80 | ((cp >> 6) & 0x3F), n);
		put_char(c, 0x80 | (cp & 0x3F), n);
	}
}

static int hex4(ctx_t *c, uint32_t *out)
{
	uint32_t v = 0;

	if (c->end - c->p < 4) {
		return ERR_SYNTAX;
	}
	for (int i = 0; i < 4; i++) {
		char h = c->p[i];

		v <<= 4;
		if (h >= '0' && h <= '9') {
			v |= (uint32_t)(h - '0');
		} else if (h >= 'a' && h <= 'f') {
			v |= (uint32_t)(h - 'a' + 10);
		} else if (h >= 'A' && h <= 'F') {
			v |= (uint32_t)(h - 'A' + 10);
		} else {
			return ERR_SYNTAX;
		}
	}
	c->p += 4;
	*out = v;
	return 0;
}

/* Parses a string at c->p (which points to the opening quote) into c->sbuf. */
static int parse_string(ctx_t *c)
{
	size_t n = 0;

	c->scut = false;
	c->p++; /* opening quote */
	while (c->p < c->end) {
		unsigned char ch = (unsigned char)*c->p++;

		if (ch == '"') {
			c->sbuf[n] = '\0';
			return 0;
		}
		if (ch < 0x20) {
			return ERR_SYNTAX; /* raw control character */
		}
		if (ch != '\\') {
			put_char(c, ch, &n);
			continue;
		}
		if (c->p >= c->end) {
			return ERR_SYNTAX;
		}
		ch = (unsigned char)*c->p++;
		switch (ch) {
		case '"': case '\\': case '/':
			put_char(c, ch, &n);
			break;
		case 'b': put_char(c, '\b', &n); break;
		case 'f': put_char(c, '\f', &n); break;
		case 'n': put_char(c, '\n', &n); break;
		case 'r': put_char(c, '\r', &n); break;
		case 't': put_char(c, '\t', &n); break;
		case 'u': {
			uint32_t cp;

			if (hex4(c, &cp) < 0) {
				return ERR_SYNTAX;
			}
			if (cp >= 0xD800 && cp <= 0xDBFF) {
				uint32_t lo;

				if (c->end - c->p >= 6 && c->p[0] == '\\' && c->p[1] == 'u') {
					c->p += 2;
					if (hex4(c, &lo) < 0) {
						return ERR_SYNTAX;
					}
					if (lo >= 0xDC00 && lo <= 0xDFFF) {
						cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
					} else {
						cp = '?';
					}
				} else {
					cp = '?';
				}
			} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
				cp = '?';
			}
			put_utf8(c, cp, &n);
			break;
		}
		default:
			return ERR_SYNTAX;
		}
	}
	return ERR_SYNTAX; /* no closing quote */
}

static int parse_number(ctx_t *c, jl_val_t *v)
{
	char tmp[40];
	size_t n = 0;

	while (c->p < c->end && n < sizeof(tmp) - 1 &&
	       ((*c->p >= '0' && *c->p <= '9') || *c->p == '-' || *c->p == '+' || *c->p == '.' ||
		*c->p == 'e' || *c->p == 'E')) {
		tmp[n++] = *c->p++;
	}
	tmp[n] = '\0';
	/* a number that is longer than the buffer */
	if (c->p < c->end && ((*c->p >= '0' && *c->p <= '9') || *c->p == '.')) {
		return ERR_SYNTAX;
	}
	if (n == 0) {
		return ERR_SYNTAX;
	}
	/* strict JSON number: -? (0 | 1-9 digits*) (. digits+)? ([eE] [+-]? digits+)? */
	const char *q = tmp;

	if (*q == '-') {
		q++;
	}
	if (*q == '0') {
		q++;
	} else if (*q >= '1' && *q <= '9') {
		while (*q >= '0' && *q <= '9') {
			q++;
		}
	} else {
		return ERR_SYNTAX;
	}
	if (*q == '.') {
		q++;
		if (*q < '0' || *q > '9') {
			return ERR_SYNTAX;
		}
		while (*q >= '0' && *q <= '9') {
			q++;
		}
	}
	if (*q == 'e' || *q == 'E') {
		q++;
		if (*q == '+' || *q == '-') {
			q++;
		}
		if (*q < '0' || *q > '9') {
			return ERR_SYNTAX;
		}
		while (*q >= '0' && *q <= '9') {
			q++;
		}
	}
	if (*q != '\0') {
		return ERR_SYNTAX;
	}
	char *endp;
	double d = strtod(tmp, &endp);

	if (*endp != '\0' || isnan(d)) {
		return ERR_SYNTAX;
	}
	v->type = JL_NUM;
	v->num_overflow = false;
	if (isinf(d) || fabs(d) > 9e15) {
		v->num_overflow = true;
		d = d < 0 ? -9e15 : 9e15;
	}
	v->num = (int64_t)(d >= 0 ? d + 0.5 : d - 0.5);
	return 0;
}

static int parse_value(ctx_t *c, int depth);

static void emit(ctx_t *c, const jl_val_t *v)
{
	c->cb(c->path, c->arr_idx, v, c->user);
}

static int parse_object(ctx_t *c, int depth)
{
	c->p++; /* { */
	skip_ws(c);
	if (c->p < c->end && *c->p == '}') {
		c->p++;
		return 0;
	}
	for (;;) {
		skip_ws(c);
		if (c->p >= c->end || *c->p != '"') {
			return ERR_SYNTAX;
		}
		int r = parse_string(c);

		if (r < 0) {
			return r;
		}
		size_t keep = c->path_len; /* path length before this key, restored after the value */
		const char *key = c->sbuf;
		size_t klen = strlen(key);
		int saved_idx = c->arr_idx;

		/* a key that does not fit in the path is replaced by "~" (never matches anything) */
		if (keep + (keep ? 1 : 0) + klen >= sizeof(c->path)) {
			key = "~";
			klen = 1;
		}
		if (keep + (keep ? 1 : 0) + klen < sizeof(c->path)) {
			if (keep) {
				c->path[c->path_len++] = '.';
			}
			memcpy(c->path + c->path_len, key, klen + 1);
			c->path_len += klen;
		}
		skip_ws(c);
		if (c->p >= c->end || *c->p != ':') {
			return ERR_SYNTAX;
		}
		c->p++;
		c->arr_idx = -1;
		r = parse_value(c, depth + 1);
		if (r < 0) {
			return r;
		}
		c->arr_idx = saved_idx;
		c->path_len = keep;
		c->path[keep] = '\0';

		skip_ws(c);
		if (c->p >= c->end) {
			return ERR_SYNTAX;
		}
		if (*c->p == ',') {
			c->p++;
			continue;
		}
		if (*c->p == '}') {
			c->p++;
			return 0;
		}
		return ERR_SYNTAX;
	}
}

static int parse_array(ctx_t *c, int depth)
{
	int saved = c->arr_idx;
	int i = 0;

	c->p++; /* [ */
	skip_ws(c);
	if (c->p < c->end && *c->p == ']') {
		c->p++;
		return 0;
	}
	for (;;) {
		c->arr_idx = i++;
		int r = parse_value(c, depth + 1);

		if (r < 0) {
			return r;
		}
		skip_ws(c);
		if (c->p >= c->end) {
			return ERR_SYNTAX;
		}
		if (*c->p == ',') {
			c->p++;
			continue;
		}
		if (*c->p == ']') {
			c->p++;
			c->arr_idx = saved;
			return 0;
		}
		return ERR_SYNTAX;
	}
}

static bool match_word(ctx_t *c, const char *w)
{
	size_t n = strlen(w);

	if ((size_t)(c->end - c->p) >= n && memcmp(c->p, w, n) == 0) {
		c->p += n;
		return true;
	}
	return false;
}

static int parse_value(ctx_t *c, int depth)
{
	jl_val_t v = {0};

	if (depth > JL_MAX_DEPTH) {
		return ERR_DEPTH;
	}
	skip_ws(c);
	if (c->p >= c->end) {
		return ERR_SYNTAX;
	}
	switch (*c->p) {
	case '{':
		return parse_object(c, depth);
	case '[':
		return parse_array(c, depth);
	case '"': {
		int r = parse_string(c);

		if (r < 0) {
			return r;
		}
		v.type = JL_STR;
		v.str = c->sbuf;
		v.str_cut = c->scut;
		emit(c, &v);
		return 0;
	}
	case 't':
		if (!match_word(c, "true")) {
			return ERR_SYNTAX;
		}
		v.type = JL_BOOL;
		v.num = 1;
		emit(c, &v);
		return 0;
	case 'f':
		if (!match_word(c, "false")) {
			return ERR_SYNTAX;
		}
		v.type = JL_BOOL;
		v.num = 0;
		emit(c, &v);
		return 0;
	case 'n':
		if (!match_word(c, "null")) {
			return ERR_SYNTAX;
		}
		v.type = JL_NULL;
		emit(c, &v);
		return 0;
	default: {
		int r = parse_number(c, &v);

		if (r < 0) {
			return r;
		}
		emit(c, &v);
		return 0;
	}
	}
}

int jl_parse(const char *text, size_t len, jl_cb cb, void *user)
{
	ctx_t c = {.p = text, .end = text + len, .cb = cb, .user = user, .arr_idx = -1};

	if (!text || !cb) {
		return ERR_SYNTAX;
	}
	if (len > JL_MAX_LEN) {
		return ERR_LEN;
	}
	skip_ws(&c);
	if (c.p >= c.end || *c.p != '{') {
		return ERR_TOP;
	}
	int r = parse_object(&c, 0);

	if (r < 0) {
		return r;
	}
	skip_ws(&c);
	return c.p == c.end ? 0 : ERR_SYNTAX;
}

bool jl_as_int(const jl_val_t *v, int64_t *out)
{
	switch (v->type) {
	case JL_NUM:
		if (v->num_overflow) {
			return false;
		}
		*out = v->num;
		return true;
	case JL_BOOL:
		*out = v->num;
		return true;
	case JL_STR: {
		const char *s = v->str;
		char *endp;

		while (*s == ' ' || *s == '\t') {
			s++;
		}
		if (*s == '\0' || v->str_cut) {
			return false;
		}
		double d = strtod(s, &endp);

		while (*endp == ' ' || *endp == '\t') {
			endp++;
		}
		if (*endp != '\0' || isnan(d) || isinf(d) || fabs(d) > 9e15) {
			return false;
		}
		*out = (int64_t)(d >= 0 ? d + 0.5 : d - 0.5);
		return true;
	}
	default:
		return false;
	}
}
