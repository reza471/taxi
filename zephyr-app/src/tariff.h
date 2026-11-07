#ifndef TARIFF_H
#define TARIFF_H
#include <stddef.h>
#include <stdbool.h>

/* Parse the server tariff JSON, validate it, apply it and save it to flash.
 * `json` is modified in place. Returns true if the tariff was accepted. */
bool tariff_apply_json(char *json, size_t len);
#endif
