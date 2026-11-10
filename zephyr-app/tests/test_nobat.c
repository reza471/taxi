/* gcc -O1 -fsanitize=address,undefined -o /tmp/tn core/nobat.c core/json_lite.c tests/test_nobat.c -Icore -lm && /tmp/tn */
#include <stdio.h>
#include <string.h>
#include "../core/nobat.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static bool p(const char *s, nobat_info_t *o)
{
	return nobat_parse(s, strlen(s), o);
}

int main(void)
{
	nobat_info_t o;

	CHECK(p("{\"state\":\"wait\",\"position\":3,\"nobat_id\":17}", &o));
	CHECK(o.state == NOBAT_WAIT && o.position == 3 && o.id == 17);
	CHECK(p("{\"nobat_id\":\"99\",\"position\":\"1\",\"state\":\"turn\"}", &o));
	CHECK(o.state == NOBAT_TURN && o.position == 1 && o.id == 99);
	CHECK(p("{\"state\":\"none\"}", &o));
	CHECK(o.state == NOBAT_NONE && o.position == 0 && o.id == 0);
	CHECK(p("{\"data\":{\"x\":1},\"state\":\"wait\",\"position\":5000}", &o));
	CHECK(o.position == 999);
	CHECK(p("{\"state\":\"wait\",\"position\":-4,\"nobat_id\":-1}", &o));
	CHECK(o.position == 0 && o.id == 0);
	o.position = 42;
	CHECK(!p("{\"position\":3}", &o));          /* no state */
	CHECK(!p("{\"state\":\"maybe\"}", &o));     /* unknown state */
	CHECK(!p("{\"state\":\"wait\"", &o));       /* broken */
	CHECK(!p("", &o));
	CHECK(!p("[1,2]", &o));
	CHECK(o.position == 42);                    /* not touched on failure */
	CHECK(!nobat_parse(NULL, 0, &o));
	if (fails) {
		printf("%d FAILED\n", fails);
		return 1;
	}
	printf("nobat: all tests passed\n");
	return 0;
}
