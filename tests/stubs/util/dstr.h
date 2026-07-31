#pragma once

#include <stdlib.h>
#include <string.h>

struct dstr {
	char *array;
	size_t len;
	size_t capacity;
};

static inline void dstr_cat(struct dstr *str, const char *text)
{
	if (!str || !text)
		return;
	size_t add = strlen(text);
	if (add > SIZE_MAX - str->len - 1)
		abort();
	size_t needed = str->len + add + 1;
	if (needed > str->capacity) {
		size_t capacity = str->capacity ? str->capacity : 64;
		while (capacity < needed) {
			if (capacity > SIZE_MAX / 2)
				abort();
			capacity *= 2;
		}
		char *grown = realloc(str->array, capacity);
		if (!grown)
			abort();
		str->array = grown;
		str->capacity = capacity;
	}
	memcpy(str->array + str->len, text, add + 1);
	str->len += add;
}
static inline void dstr_free(struct dstr *str)
{
	if (!str)
		return;
	free(str->array);
	memset(str, 0, sizeof(*str));
}
