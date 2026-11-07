/* Reads a JSON text from stdin and tells if the meter's reader (core/json_lite.c) accepts it.
 * Exit 0 = accepted and a numeric PM >= 1 was found (top level or inside "taxiFare"), 1 = refused. */
#include "../../../core/json_lite.h"
#include <stdio.h>
#include <string.h>

static int pm;

static void cb(const char *path, int idx, const jl_val_t *v, void *u)
{
	int64_t n;

	(void)u;
	if (idx < 0 && (!strcmp(path, "PM") || !strcmp(path, "taxiFare.PM")) && jl_as_int(v, &n)) {
		pm = (int)n;
	}
}

int main(void)
{
	static char buf[JL_MAX_LEN + 16];
	size_t n = fread(buf, 1, sizeof(buf) - 1, stdin);

	if (jl_parse(buf, n, cb, NULL) < 0) {
		return 1;
	}
	return pm >= 1 ? 0 : 1;
}
