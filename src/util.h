#ifndef CARSHIP_UTIL_H
#define CARSHIP_UTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
	char *data;
	size_t len;
	size_t cap;
} str;

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

void str_reserve(str *s, size_t extra);
void str_addc(str *s, char c);
void str_addn(str *s, const char *p, size_t n);
void str_add(str *s, const char *p);
void str_addf(str *s, const char *fmt, ...);
void str_clear(str *s);
void str_free(str *s);
char *str_take(str *s);

bool str_eq(const char *a, const char *b);
bool has_prefix(const char *s, const char *prefix);
bool has_suffix(const char *s, const char *suffix);
char *trim(char *s);

char *read_file(const char *path, size_t *len_out);
bool file_exists(const char *path);
bool dir_has_entry(const char *dir, const char *name);

/* Runs argv with cwd as working directory, capturing stdout. Returns NULL on
 * failure or non-zero exit. Never invokes a shell. */
char *capture(const char *cwd, char *const argv[], int timeout_ms);

/* Display width of a UTF-8 string, ignoring ANSI escape sequences. */
size_t display_width(const char *s);

#endif
