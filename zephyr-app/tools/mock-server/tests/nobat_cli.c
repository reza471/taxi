/* Reads a NobatReady / NobatStatus answer from stdin and prints what the meter's own reader
 * (core/nobat.c) understands: "<state> <position> <id>" (state 0 none, 1 wait, 2 turn). Exit 1 = refused. */
#include "../../../core/nobat.h"
#include <stdio.h>

int main(void)
{
	static char buf[4096];
	size_t n = fread(buf, 1, sizeof(buf) - 1, stdin);
	nobat_info_t o;

	buf[n] = '\0';
	if (!nobat_parse(buf, n, &o)) {
		return 1;
	}
	printf("%d %d %lld\n", (int)o.state, (int)o.position, (long long)o.id);
	return 0;
}
