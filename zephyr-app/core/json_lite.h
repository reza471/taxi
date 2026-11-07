/*
 * Small tolerant JSON reader (pure C, no heap, no OS calls).
 *
 * It walks the whole document once and calls a callback for every scalar value.
 * The caller collects what it needs and decides after the parse if the data is acceptable.
 *
 * What it accepts (servers written in PHP / JS / Python all look different):
 *   - any key order, whitespace, nested objects and arrays (unknown ones are just walked)
 *   - numbers as integers, decimals, exponents or as strings ("46")
 *   - true / false / null
 *   - string escapes \" \\ \/ \b \f \n \r \t \uXXXX (surrogate pairs too), decoded to UTF-8
 * What it rejects: broken syntax, top level that is not an object, nesting deeper than JL_MAX_DEPTH,
 * documents longer than JL_MAX_LEN, text after the closing brace.
 */
#ifndef JSON_LITE_H
#define JSON_LITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JL_MAX_DEPTH 8
#define JL_MAX_LEN 16384
#define JL_PATH_MAX 96
#define JL_STR_MAX 192 /* longer strings are cut and flagged */

typedef enum { JL_NUM, JL_STR, JL_BOOL, JL_NULL } jl_type_t;

typedef struct {
	jl_type_t type;
	int64_t num;       /* JL_NUM: rounded to the nearest integer, JL_BOOL: 0/1 */
	bool num_overflow; /* JL_NUM: the value is outside +-9e15 (num is clamped) */
	const char *str;   /* JL_STR: decoded, NUL terminated, valid only inside the callback */
	bool str_cut;      /* JL_STR: the text was longer than JL_STR_MAX */
} jl_val_t;

/* path: "PM", "taxiFare.PM", "service" (arrays do not add to the path).
 * idx : index inside the innermost array, -1 when the value is not in an array. */
typedef void (*jl_cb)(const char *path, int idx, const jl_val_t *v, void *user);

/* Returns 0 on success, a negative number on a syntax / limit error. */
int jl_parse(const char *text, size_t len, jl_cb cb, void *user);

/* Read an integer out of a value: numbers, bools and numeric strings ("46", " 12.5 ") work.
 * Returns false for null / text that is not a number / values outside +-9e15. */
bool jl_as_int(const jl_val_t *v, int64_t *out);

#endif
