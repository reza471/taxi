/* Host tests for core/json_lite.c, including a fuzz run under ASAN/UBSAN. */
#include "../core/json_lite.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct rec {
	char path[8][JL_PATH_MAX];
	int idx[8];
	int64_t num[8];
	char str[8][JL_STR_MAX + 1];
	jl_type_t type[8];
	bool cut[8];
	bool ovf[8];
	int n;
};

static void cb(const char *path, int idx, const jl_val_t *v, void *user)
{
	struct rec *r = user;

	if (r->n >= 8) {
		return;
	}
	snprintf(r->path[r->n], sizeof(r->path[0]), "%s", path);
	r->idx[r->n] = idx;
	r->type[r->n] = v->type;
	r->num[r->n] = v->num;
	r->ovf[r->n] = v->num_overflow;
	r->cut[r->n] = v->str_cut;
	if (v->type == JL_STR) {
		snprintf(r->str[r->n], sizeof(r->str[0]), "%s", v->str);
	} else {
		r->str[r->n][0] = '\0';
	}
	r->n++;
}

static int run(const char *text, struct rec *r)
{
	memset(r, 0, sizeof(*r));
	return jl_parse(text, strlen(text), cb, r);
}

static void test_basic(void)
{
	struct rec r;

	assert(run("{\"a\":1,\"b\":\"x\",\"c\":true,\"d\":null}", &r) == 0);
	assert(r.n == 4);
	assert(!strcmp(r.path[0], "a") && r.type[0] == JL_NUM && r.num[0] == 1);
	assert(!strcmp(r.path[1], "b") && r.type[1] == JL_STR && !strcmp(r.str[1], "x"));
	assert(r.type[2] == JL_BOOL && r.num[2] == 1);
	assert(r.type[3] == JL_NULL);
	assert(run(" \n\t{ }\r\n", &r) == 0 && r.n == 0);
	assert(run("{\"a\" :\n 5 , \"b\":\t6 }", &r) == 0 && r.n == 2);
}

static void test_numbers(void)
{
	struct rec r;
	int64_t v;
	jl_val_t jv;

	assert(run("{\"a\":-5,\"b\":1318.5,\"c\":1e3,\"d\":-0.4,\"e\":2.5}", &r) == 0);
	assert(r.num[0] == -5);
	assert(r.num[1] == 1319); /* rounds to nearest */
	assert(r.num[2] == 1000);
	assert(r.num[3] == 0);
	assert(r.num[4] == 3);
	assert(run("{\"a\":99999999999999999999}", &r) == 0);
	assert(r.ovf[0]);
	assert(run("{\"a\":1e400}", &r) == 0 && r.ovf[0]);

	/* jl_as_int: numbers, bools, numeric strings */
	jv = (jl_val_t){.type = JL_STR, .str = " 46 "};
	assert(jl_as_int(&jv, &v) && v == 46);
	jv = (jl_val_t){.type = JL_STR, .str = "12.5"};
	assert(jl_as_int(&jv, &v) && v == 13);
	jv = (jl_val_t){.type = JL_STR, .str = "abc"};
	assert(!jl_as_int(&jv, &v));
	jv = (jl_val_t){.type = JL_STR, .str = ""};
	assert(!jl_as_int(&jv, &v));
	jv = (jl_val_t){.type = JL_STR, .str = "12abc"};
	assert(!jl_as_int(&jv, &v));
	jv = (jl_val_t){.type = JL_STR, .str = "nan"};
	assert(!jl_as_int(&jv, &v));
	jv = (jl_val_t){.type = JL_STR, .str = "inf"};
	assert(!jl_as_int(&jv, &v));
	jv = (jl_val_t){.type = JL_STR, .str = "1e99"};
	assert(!jl_as_int(&jv, &v));
	jv = (jl_val_t){.type = JL_NULL};
	assert(!jl_as_int(&jv, &v));
	jv = (jl_val_t){.type = JL_BOOL, .num = 1};
	assert(jl_as_int(&jv, &v) && v == 1);
	jv = (jl_val_t){.type = JL_NUM, .num = 9000000000000000, .num_overflow = true};
	assert(!jl_as_int(&jv, &v));
}

static void test_strings(void)
{
	struct rec r;

	assert(run("{\"a\":\"p\\\"ss\\\\w\\/x\\n\"}", &r) == 0);
	assert(!strcmp(r.str[0], "p\"ss\\w/x\n"));
	assert(run("{\"a\":\"caf\\u00e9\"}", &r) == 0);
	assert(!strcmp(r.str[0], "caf\xc3\xa9"));
	assert(run("{\"a\":\"\\ud83d\\ude00\"}", &r) == 0); /* emoji, surrogate pair */
	assert(!strcmp(r.str[0], "\xf0\x9f\x98\x80"));
	assert(run("{\"a\":\"\\ud83d\"}", &r) == 0); /* lone surrogate: no crash */
	assert(!strcmp(r.str[0], "?"));
	assert(run("{\"a\":\"\\q\"}", &r) < 0);       /* unknown escape */
	assert(run("{\"a\":\"\\u12\"}", &r) < 0);     /* short \u */
	assert(run("{\"a\":\"abc}", &r) < 0);         /* no closing quote */
	assert(run("{\"a\":\"a\nb\"}", &r) < 0);      /* raw newline in a string */

	char longs[300];
	char doc[400];

	memset(longs, 'a', 200);
	longs[200] = '\0';
	snprintf(doc, sizeof(doc), "{\"a\":\"%s\"}", longs);
	assert(run(doc, &r) == 0);
	assert(r.cut[0] && strlen(r.str[0]) == JL_STR_MAX);
}

static void test_nesting(void)
{
	struct rec r;

	assert(run("{\"meta\":{\"a\":[1,2,{\"b\":3}]},\"list\":[[1],[2]],\"x\":7}", &r) == 0);
	assert(!strcmp(r.path[0], "meta.a") && r.idx[0] == 0 && r.num[0] == 1);
	assert(!strcmp(r.path[1], "meta.a") && r.idx[1] == 1 && r.num[1] == 2);
	assert(!strcmp(r.path[2], "meta.a.b") && r.idx[2] == -1 && r.num[2] == 3);
	assert(!strcmp(r.path[5], "x") && r.num[5] == 7 && r.idx[5] == -1);
	assert(run("{\"taxiFare\":{\"PM\":46}}", &r) == 0);
	assert(!strcmp(r.path[0], "taxiFare.PM"));
	assert(run("{\"service\":[211,222,233]}", &r) == 0);
	assert(r.n == 3 && r.idx[2] == 2 && !strcmp(r.path[2], "service"));
	assert(run("{\"a\":[],\"b\":{},\"c\":1}", &r) == 0 && r.n == 1);

	/* depth limit */
	char deep[200] = "";

	for (int i = 0; i < 20; i++) {
		strcat(deep, "{\"a\":");
	}
	strcat(deep, "1");
	for (int i = 0; i < 20; i++) {
		strcat(deep, "}");
	}
	assert(run(deep, &r) < 0);
	/* long key does not corrupt the path of later keys */
	char lk[200];
	char doc2[300];

	memset(lk, 'k', 150);
	lk[150] = '\0';
	snprintf(doc2, sizeof(doc2), "{\"o\":{\"%s\":1,\"ok\":2},\"z\":3}", lk);
	assert(run(doc2, &r) == 0);
	assert(!strcmp(r.path[1], "o.ok") && r.num[1] == 2);
	assert(!strcmp(r.path[2], "z"));
}

static void test_rejects(void)
{
	struct rec r;
	const char *bad[] = {
		"", " ", "null", "[1,2]", "5", "\"x\"", "{", "}", "{\"a\"}", "{\"a\":}", "{\"a\":1,}",
		"{,\"a\":1}", "{\"a\":1 \"b\":2}", "{\"a\":1}x", "{\"a\":1}{\"b\":2}", "{a:1}",
		"{'a':1}", "{\"a\":tru}", "{\"a\":nul}", "{\"a\":01x}", "{\"a\":--1}", "{\"a\":.5}",
		"{\"a\":+1}", "{\"a\":[1,}", "{\"a\":[1 2]}", "<html>502</html>", "{\"a\":1",
		"{\"a\":\"", "{\"a\":1e}", "{\"a\":1.}",
	};

	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		if (run(bad[i], &r) >= 0) {
			fprintf(stderr, "accepted bad json: %s\n", bad[i]);
			assert(0);
		}
	}
	/* too long */
	static char big[JL_MAX_LEN + 100];

	memset(big, ' ', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	memcpy(big, "{}", 2);
	assert(jl_parse(big, strlen(big), cb, &r) < 0);
	assert(jl_parse(NULL, 0, cb, &r) < 0);
	assert(jl_parse("{}", 2, NULL, &r) < 0);
}

/* random damage of a realistic document: must never crash, hang or read outside the buffer */
static void test_fuzz(void)
{
	static const char sample[] =
		"{\"PM\":46,\"pri_cost\":26200,\"Start_night\":22,\"Stop_night\":6,\"Cost_200\":1318,"
		"\"wifi\":\"My\\u00e9Net\",\"wifiPass\":\"s\\\"e\",\"service\":[211,222,233],"
		"\"nested\":{\"a\":[1,2.5e3,{\"b\":null}],\"c\":true}}";
	struct rec r;
	unsigned seed = 12345;
	size_t n = sizeof(sample) - 1;
	int ok = 0, bad = 0;

	for (int iter = 0; iter < 300000; iter++) {
		/* the copy is exactly as long as the text so ASAN sees any read past the end */
		size_t len = n;
		char *buf;

		seed = seed * 1103515245u + 12345u;
		if ((seed >> 16) % 4 == 0) {
			len = (seed >> 8) % (n + 1); /* truncated */
		}
		buf = malloc(len ? len : 1);
		memcpy(buf, sample, len);
		int flips = 1 + (int)((seed >> 4) % 4);

		for (int f = 0; f < flips && len; f++) {
			seed = seed * 1103515245u + 12345u;
			size_t pos = (seed >> 8) % len;

			seed = seed * 1103515245u + 12345u;
			buf[pos] = (char)(seed >> 16);
		}
		memset(&r, 0, sizeof(r));
		if (jl_parse(buf, len, cb, &r) == 0) {
			ok++;
		} else {
			bad++;
		}
		free(buf);
	}
	printf("fuzz: %d accepted, %d rejected, no crash\n", ok, bad);
	assert(ok > 0 && bad > 0);
}

int main(void)
{
	test_basic();
	test_numbers();
	test_strings();
	test_nesting();
	test_rejects();
	test_fuzz();
	puts("all json tests passed");
	return 0;
}
