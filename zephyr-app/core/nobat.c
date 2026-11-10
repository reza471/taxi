#include "nobat.h"
#include <string.h>
#include "json_lite.h"

struct ctx {
	int state; /* -1 = not seen / unknown */
	int64_t pos;
	int64_t id;
};

static void cb(const char *path, int idx, const jl_val_t *v, void *user)
{
	struct ctx *c = user;
	int64_t n;

	if (idx >= 0) {
		return;
	}
	if (strcmp(path, "state") == 0 && v->type == JL_STR) {
		if (strcmp(v->str, "wait") == 0) {
			c->state = NOBAT_WAIT;
		} else if (strcmp(v->str, "turn") == 0) {
			c->state = NOBAT_TURN;
		} else if (strcmp(v->str, "none") == 0) {
			c->state = NOBAT_NONE;
		}
	} else if (strcmp(path, "position") == 0 && jl_as_int(v, &n)) {
		c->pos = n;
	} else if (strcmp(path, "nobat_id") == 0 && jl_as_int(v, &n)) {
		c->id = n;
	}
}

bool nobat_parse(const char *text, size_t len, nobat_info_t *out)
{
	struct ctx c = {.state = -1, .pos = 0, .id = 0};

	if (!text || jl_parse(text, len, cb, &c) != 0 || c.state < 0) {
		return false;
	}
	out->state = (nobat_state_t)c.state;
	out->position = (int32_t)(c.pos < 0 ? 0 : (c.pos > 999 ? 999 : c.pos));
	out->id = c.id < 0 ? 0 : c.id;
	return true;
}
