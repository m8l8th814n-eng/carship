#ifndef CARSHIP_TOML_H
#define CARSHIP_TOML_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
	TOML_STRING,
	TOML_INT,
	TOML_FLOAT,
	TOML_BOOL,
	TOML_ARRAY,
	TOML_TABLE,
} toml_type;

typedef struct toml_value toml_value;
typedef struct toml_doc toml_doc;

struct toml_value {
	toml_type type;
	union {
		char *s;
		long long i;
		double f;
		bool b;
		struct {
			toml_value **items;
			size_t n, cap;
		} arr;
		struct {
			char **keys;
			toml_value **vals;
			size_t n, cap;
		} tab;
	};
};

/* Every value in the returned document is owned by the document's arena;
 * toml_doc_free() releases all of it at once. */
toml_doc *toml_parse(const char *text, char *err, size_t errlen);
const toml_value *toml_root(const toml_doc *doc);
void toml_doc_free(toml_doc *doc);

const toml_value *toml_get(const toml_value *tab, const char *key);
const toml_value *toml_path(const toml_value *root, const char *dotted);

const char *toml_str(const toml_value *v, const char *dflt);
long long toml_int(const toml_value *v, long long dflt);
double toml_float(const toml_value *v, double dflt);
bool toml_bool(const toml_value *v, bool dflt);

const char *toml_str_at(const toml_value *tab, const char *key, const char *dflt);
long long toml_int_at(const toml_value *tab, const char *key, long long dflt);
bool toml_bool_at(const toml_value *tab, const char *key, bool dflt);

#endif
