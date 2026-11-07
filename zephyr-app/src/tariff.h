#ifndef TARIFF_H
#define TARIFF_H
#include <stddef.h>
#include <stdbool.h>

/* Read the server tariff JSON, check every value, then apply it and save it to flash
 * (flash is written only if something changed).
 * Returns false and changes NOTHING if the JSON is broken or a value is out of range. */
bool tariff_apply_json(const char *json, size_t len);
#endif
