#include "toml.h"

#include "util.h"

#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_BLOCK 16384

typedef struct arena_block {
	struct arena_block *next;
	size_t used, cap;
	char data[];
} arena_block;

struct toml_doc {
	arena_block *blocks;
	toml_value *root;
};

typedef struct {
	const char *p;
	int line;
	char *err;
	size_t errlen;
	toml_doc *doc;
	str *tmp; /* string under construction, released if parsing aborts */
	jmp_buf jmp;
} parser;

static void *arena_alloc(toml_doc *doc, size_t n)
{
	size_t align = _Alignof(max_align_t);
	n = (n + align - 1) & ~(align - 1);

	arena_block *b = doc->blocks;
	if (!b || b->cap - b->used < n) {
		size_t cap = n > ARENA_BLOCK ? n : ARENA_BLOCK;
		b = xmalloc(sizeof *b + cap);
		b->next = doc->blocks;
		b->used = 0;
		b->cap = cap;
		doc->blocks = b;
	}

	void *p = b->data + b->used;
	b->used += n;
	return p;
}

static char *arena_strndup(toml_doc *doc, const char *s, size_t n)
{
	char *p = arena_alloc(doc, n + 1);
	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

/* Moves a built-up string into the arena and releases the scratch buffer. */
static char *arena_take(parser *ps, str *s)
{
	char *p = arena_strndup(ps->doc, s->data ? s->data : "", s->len);
	str_free(s);
	ps->tmp = NULL;
	return p;
}

void toml_doc_free(toml_doc *doc)
{
	if (!doc)
		return;
	arena_block *b = doc->blocks;
	while (b) {
		arena_block *next = b->next;
		free(b);
		b = next;
	}
	free(doc);
}

const toml_value *toml_root(const toml_doc *doc)
{
	return doc ? doc->root : NULL;
}

static _Noreturn void fail(parser *ps, const char *msg)
{
	if (ps->err && ps->errlen)
		snprintf(ps->err, ps->errlen, "line %d: %s", ps->line, msg);
	if (ps->tmp)
		str_free(ps->tmp);
	longjmp(ps->jmp, 1);
}

static toml_value *new_value(parser *ps, toml_type t)
{
	toml_value *v = arena_alloc(ps->doc, sizeof *v);
	memset(v, 0, sizeof *v);
	v->type = t;
	return v;
}

const toml_value *toml_get(const toml_value *tab, const char *key)
{
	if (!tab || tab->type != TOML_TABLE)
		return NULL;
	for (size_t i = 0; i < tab->tab.n; i++)
		if (strcmp(tab->tab.keys[i], key) == 0)
			return tab->tab.vals[i];
	return NULL;
}

static void table_set(parser *ps, toml_value *tab, char *key, toml_value *val)
{
	for (size_t i = 0; i < tab->tab.n; i++) {
		if (strcmp(tab->tab.keys[i], key) == 0) {
			tab->tab.vals[i] = val;
			return;
		}
	}

	if (tab->tab.n == tab->tab.cap) {
		size_t cap = tab->tab.cap ? tab->tab.cap * 2 : 8;
		char **keys = arena_alloc(ps->doc, cap * sizeof *keys);
		toml_value **vals = arena_alloc(ps->doc, cap * sizeof *vals);
		if (tab->tab.n) {
			memcpy(keys, tab->tab.keys, tab->tab.n * sizeof *keys);
			memcpy(vals, tab->tab.vals, tab->tab.n * sizeof *vals);
		}
		tab->tab.keys = keys;
		tab->tab.vals = vals;
		tab->tab.cap = cap;
	}

	tab->tab.keys[tab->tab.n] = key;
	tab->tab.vals[tab->tab.n] = val;
	tab->tab.n++;
}

static void array_push(parser *ps, toml_value *arr, toml_value *val)
{
	if (arr->arr.n == arr->arr.cap) {
		size_t cap = arr->arr.cap ? arr->arr.cap * 2 : 8;
		toml_value **items = arena_alloc(ps->doc, cap * sizeof *items);
		if (arr->arr.n)
			memcpy(items, arr->arr.items, arr->arr.n * sizeof *items);
		arr->arr.items = items;
		arr->arr.cap = cap;
	}
	arr->arr.items[arr->arr.n++] = val;
}

const toml_value *toml_path(const toml_value *root, const char *dotted)
{
	const toml_value *cur = root;
	const char *p = dotted;

	while (cur && *p) {
		const char *dot = strchr(p, '.');
		size_t n = dot ? (size_t)(dot - p) : strlen(p);

		if (cur->type != TOML_TABLE)
			return NULL;

		const toml_value *found = NULL;
		for (size_t i = 0; i < cur->tab.n; i++) {
			if (strncmp(cur->tab.keys[i], p, n) == 0 && cur->tab.keys[i][n] == '\0') {
				found = cur->tab.vals[i];
				break;
			}
		}

		cur = found;
		p = dot ? dot + 1 : p + n;
	}
	return cur;
}

const char *toml_str(const toml_value *v, const char *dflt)
{
	return (v && v->type == TOML_STRING) ? v->s : dflt;
}

long long toml_int(const toml_value *v, long long dflt)
{
	if (!v)
		return dflt;
	if (v->type == TOML_INT)
		return v->i;
	if (v->type == TOML_FLOAT)
		return (long long)v->f;
	return dflt;
}

double toml_float(const toml_value *v, double dflt)
{
	if (!v)
		return dflt;
	if (v->type == TOML_FLOAT)
		return v->f;
	if (v->type == TOML_INT)
		return (double)v->i;
	return dflt;
}

bool toml_bool(const toml_value *v, bool dflt)
{
	return (v && v->type == TOML_BOOL) ? v->b : dflt;
}

const char *toml_str_at(const toml_value *tab, const char *key, const char *dflt)
{
	return toml_str(toml_get(tab, key), dflt);
}

long long toml_int_at(const toml_value *tab, const char *key, long long dflt)
{
	return toml_int(toml_get(tab, key), dflt);
}

bool toml_bool_at(const toml_value *tab, const char *key, bool dflt)
{
	return toml_bool(toml_get(tab, key), dflt);
}

static void skip_ws(parser *ps)
{
	while (*ps->p == ' ' || *ps->p == '\t')
		ps->p++;
}

static void skip_comment(parser *ps)
{
	if (*ps->p == '#')
		while (*ps->p && *ps->p != '\n')
			ps->p++;
}

/* Whitespace, comments and newlines are all insignificant between array and
 * table entries. */
static void skip_blanks(parser *ps)
{
	for (;;) {
		skip_ws(ps);
		skip_comment(ps);
		if (*ps->p == '\r')
			ps->p++;
		if (*ps->p == '\n') {
			ps->line++;
			ps->p++;
			continue;
		}
		return;
	}
}

static void encode_utf8(str *out, unsigned long cp)
{
	if (cp < 0x80) {
		str_addc(out, (char)cp);
	} else if (cp < 0x800) {
		str_addc(out, (char)(0xc0 | (cp >> 6)));
		str_addc(out, (char)(0x80 | (cp & 0x3f)));
	} else if (cp < 0x10000) {
		str_addc(out, (char)(0xe0 | (cp >> 12)));
		str_addc(out, (char)(0x80 | ((cp >> 6) & 0x3f)));
		str_addc(out, (char)(0x80 | (cp & 0x3f)));
	} else {
		str_addc(out, (char)(0xf0 | (cp >> 18)));
		str_addc(out, (char)(0x80 | ((cp >> 12) & 0x3f)));
		str_addc(out, (char)(0x80 | ((cp >> 6) & 0x3f)));
		str_addc(out, (char)(0x80 | (cp & 0x3f)));
	}
}

static void parse_escape(parser *ps, str *out, bool multiline)
{
	ps->p++;
	char c = *ps->p;

	switch (c) {
	case 'b': str_addc(out, '\b'); ps->p++; return;
	case 't': str_addc(out, '\t'); ps->p++; return;
	case 'n': str_addc(out, '\n'); ps->p++; return;
	case 'f': str_addc(out, '\f'); ps->p++; return;
	case 'r': str_addc(out, '\r'); ps->p++; return;
	case '"': str_addc(out, '"'); ps->p++; return;
	case '\\': str_addc(out, '\\'); ps->p++; return;
	case 'e': str_addc(out, 0x1b); ps->p++; return;
	case 'u':
	case 'U': {
		int digits = (c == 'u') ? 4 : 8;
		ps->p++;
		unsigned long cp = 0;
		for (int i = 0; i < digits; i++) {
			char h = *ps->p;
			int d;
			if (h >= '0' && h <= '9')
				d = h - '0';
			else if (h >= 'a' && h <= 'f')
				d = h - 'a' + 10;
			else if (h >= 'A' && h <= 'F')
				d = h - 'A' + 10;
			else
				fail(ps, "bad unicode escape");
			cp = cp * 16 + (unsigned long)d;
			ps->p++;
		}
		encode_utf8(out, cp);
		return;
	}
	default:
		break;
	}

	/* A backslash before a newline swallows the newline and the indentation
	 * that follows it. Starship's multi-line formats rely on this. */
	if (multiline && (c == '\n' || c == '\r' || c == ' ' || c == '\t')) {
		while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\r')
			ps->p++;
		if (*ps->p != '\n')
			fail(ps, "bad escape");
		while (*ps->p == '\n' || *ps->p == ' ' || *ps->p == '\t' || *ps->p == '\r') {
			if (*ps->p == '\n')
				ps->line++;
			ps->p++;
		}
		return;
	}
	fail(ps, "unknown escape sequence");
}

static char *parse_basic_string(parser *ps)
{
	str out = {0};
	bool multiline = false;

	ps->tmp = &out;
	ps->p++;

	if (ps->p[0] == '"' && ps->p[1] == '"') {
		multiline = true;
		ps->p += 2;
		if (*ps->p == '\r')
			ps->p++;
		if (*ps->p == '\n') {
			ps->line++;
			ps->p++;
		}
	}

	for (;;) {
		if (!*ps->p)
			fail(ps, "unterminated string");

		if (*ps->p == '"') {
			if (!multiline) {
				ps->p++;
				break;
			}
			if (ps->p[1] == '"' && ps->p[2] == '"') {
				ps->p += 3;
				/* Up to two extra quotes belong to the content. */
				for (int i = 0; i < 2 && *ps->p == '"'; i++) {
					str_addc(&out, '"');
					ps->p++;
				}
				break;
			}
			str_addc(&out, '"');
			ps->p++;
			continue;
		}

		if (*ps->p == '\\') {
			parse_escape(ps, &out, multiline);
			continue;
		}

		if (*ps->p == '\n') {
			if (!multiline)
				fail(ps, "newline in single-line string");
			ps->line++;
		}
		str_addc(&out, *ps->p++);
	}

	return arena_take(ps, &out);
}

static char *parse_literal_string(parser *ps)
{
	str out = {0};
	bool multiline = false;

	ps->tmp = &out;
	ps->p++;

	if (ps->p[0] == '\'' && ps->p[1] == '\'') {
		multiline = true;
		ps->p += 2;
		if (*ps->p == '\r')
			ps->p++;
		if (*ps->p == '\n') {
			ps->line++;
			ps->p++;
		}
	}

	for (;;) {
		if (!*ps->p)
			fail(ps, "unterminated literal string");

		if (*ps->p == '\'') {
			if (!multiline) {
				ps->p++;
				break;
			}
			if (ps->p[1] == '\'' && ps->p[2] == '\'') {
				ps->p += 3;
				for (int i = 0; i < 2 && *ps->p == '\''; i++) {
					str_addc(&out, '\'');
					ps->p++;
				}
				break;
			}
			str_addc(&out, '\'');
			ps->p++;
			continue;
		}

		if (*ps->p == '\n') {
			if (!multiline)
				fail(ps, "newline in single-line string");
			ps->line++;
		}
		str_addc(&out, *ps->p++);
	}

	return arena_take(ps, &out);
}

static bool is_bare_key_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
	       c == '_' || c == '-';
}

static char *parse_key_segment(parser *ps)
{
	skip_ws(ps);
	if (*ps->p == '"')
		return parse_basic_string(ps);
	if (*ps->p == '\'')
		return parse_literal_string(ps);

	const char *start = ps->p;
	while (is_bare_key_char(*ps->p))
		ps->p++;
	if (ps->p == start)
		fail(ps, "expected key");
	return arena_strndup(ps->doc, start, (size_t)(ps->p - start));
}

/* Walks a dotted key, creating intermediate tables, and hands the final
 * segment back for the caller to assign. */
static toml_value *walk_key(parser *ps, toml_value *tab, char **last)
{
	char *seg = parse_key_segment(ps);

	for (;;) {
		skip_ws(ps);
		if (*ps->p != '.')
			break;
		ps->p++;

		toml_value *next = (toml_value *)toml_get(tab, seg);
		if (!next) {
			next = new_value(ps, TOML_TABLE);
			table_set(ps, tab, seg, next);
		} else if (next->type == TOML_ARRAY && next->arr.n > 0) {
			next = next->arr.items[next->arr.n - 1];
		} else if (next->type != TOML_TABLE) {
			fail(ps, "key path traverses a non-table");
		}
		tab = next;
		seg = parse_key_segment(ps);
	}

	*last = seg;
	return tab;
}

static toml_value *parse_value(parser *ps);

static toml_value *parse_array(parser *ps)
{
	toml_value *arr = new_value(ps, TOML_ARRAY);
	ps->p++;

	for (;;) {
		skip_blanks(ps);
		if (*ps->p == ']') {
			ps->p++;
			break;
		}
		if (!*ps->p)
			fail(ps, "unterminated array");

		array_push(ps, arr, parse_value(ps));
		skip_blanks(ps);

		if (*ps->p == ',') {
			ps->p++;
			continue;
		}
		if (*ps->p == ']') {
			ps->p++;
			break;
		}
		fail(ps, "expected ',' or ']' in array");
	}
	return arr;
}

static toml_value *parse_inline_table(parser *ps)
{
	toml_value *tab = new_value(ps, TOML_TABLE);
	ps->p++;

	for (;;) {
		skip_ws(ps);
		if (*ps->p == '}') {
			ps->p++;
			break;
		}
		if (!*ps->p)
			fail(ps, "unterminated inline table");

		char *key = NULL;
		toml_value *target = walk_key(ps, tab, &key);
		skip_ws(ps);
		if (*ps->p != '=')
			fail(ps, "expected '=' in inline table");
		ps->p++;
		table_set(ps, target, key, parse_value(ps));

		skip_ws(ps);
		if (*ps->p == ',') {
			ps->p++;
			continue;
		}
		if (*ps->p == '}') {
			ps->p++;
			break;
		}
		fail(ps, "expected ',' or '}' in inline table");
	}
	return tab;
}

static toml_value *parse_number_or_date(parser *ps)
{
	const char *start = ps->p;
	bool is_float = false;

	if (*ps->p == '+' || *ps->p == '-')
		ps->p++;

	while (*ps->p) {
		char c = *ps->p;
		if ((c >= '0' && c <= '9') || c == '_') {
			ps->p++;
		} else if (c == '.' || c == 'e' || c == 'E') {
			is_float = true;
			ps->p++;
			if ((c == 'e' || c == 'E') && (*ps->p == '+' || *ps->p == '-'))
				ps->p++;
		} else if (c == ':' || c == 'T' || c == 'Z' || c == '-' || c == '+') {
			/* Date-times are kept verbatim as strings. */
			while (*ps->p && *ps->p != '\n' && *ps->p != ',' && *ps->p != ']' &&
			       *ps->p != '}' && *ps->p != '#')
				ps->p++;
			const char *end = ps->p;
			while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
				end--;
			toml_value *v = new_value(ps, TOML_STRING);
			v->s = arena_strndup(ps->doc, start, (size_t)(end - start));
			return v;
		} else {
			break;
		}
	}

	size_t raw_len = (size_t)(ps->p - start);
	char *clean = arena_strndup(ps->doc, start, raw_len);
	char *w = clean;
	for (const char *r = clean; *r; r++)
		if (*r != '_')
			*w++ = *r;
	*w = '\0';

	toml_value *v;
	if (is_float) {
		v = new_value(ps, TOML_FLOAT);
		v->f = strtod(clean, NULL);
	} else {
		v = new_value(ps, TOML_INT);
		v->i = strtoll(clean, NULL, 10);
	}
	return v;
}

static toml_value *parse_value(parser *ps)
{
	skip_ws(ps);

	switch (*ps->p) {
	case '"': {
		char *s = parse_basic_string(ps);
		toml_value *v = new_value(ps, TOML_STRING);
		v->s = s;
		return v;
	}
	case '\'': {
		char *s = parse_literal_string(ps);
		toml_value *v = new_value(ps, TOML_STRING);
		v->s = s;
		return v;
	}
	case '[':
		return parse_array(ps);
	case '{':
		return parse_inline_table(ps);
	default:
		break;
	}

	if (strncmp(ps->p, "true", 4) == 0) {
		ps->p += 4;
		toml_value *v = new_value(ps, TOML_BOOL);
		v->b = true;
		return v;
	}
	if (strncmp(ps->p, "false", 5) == 0) {
		ps->p += 5;
		toml_value *v = new_value(ps, TOML_BOOL);
		v->b = false;
		return v;
	}
	if (strncmp(ps->p, "inf", 3) == 0 || strncmp(ps->p, "nan", 3) == 0) {
		toml_value *v = new_value(ps, TOML_FLOAT);
		v->f = (*ps->p == 'i') ? INFINITY : NAN;
		ps->p += 3;
		return v;
	}

	if ((*ps->p >= '0' && *ps->p <= '9') || *ps->p == '+' || *ps->p == '-')
		return parse_number_or_date(ps);

	fail(ps, "unexpected value");
}

/* Resolves a [table.header] or [[array.header]] path, creating tables as
 * needed, and returns the table that subsequent keys belong to. */
static toml_value *resolve_header(parser *ps, toml_value *root, bool array_of_tables)
{
	toml_value *tab = root;
	char *seg = parse_key_segment(ps);

	for (;;) {
		skip_ws(ps);
		bool last = (*ps->p != '.');
		toml_value *next = (toml_value *)toml_get(tab, seg);

		if (last && array_of_tables) {
			if (!next) {
				next = new_value(ps, TOML_ARRAY);
				table_set(ps, tab, seg, next);
			}
			if (next->type != TOML_ARRAY)
				fail(ps, "cannot redefine key as array of tables");
			toml_value *elem = new_value(ps, TOML_TABLE);
			array_push(ps, next, elem);
			return elem;
		}

		if (!next) {
			next = new_value(ps, TOML_TABLE);
			table_set(ps, tab, seg, next);
		} else if (next->type == TOML_ARRAY && next->arr.n > 0) {
			next = next->arr.items[next->arr.n - 1];
		} else if (next->type != TOML_TABLE) {
			fail(ps, "cannot redefine key as table");
		}

		tab = next;
		if (last)
			return tab;

		ps->p++; /* the dot */
		seg = parse_key_segment(ps);
	}
}

toml_doc *toml_parse(const char *text, char *err, size_t errlen)
{
	toml_doc *doc = xmalloc(sizeof *doc);
	doc->blocks = NULL;
	doc->root = NULL;

	parser ps = {.p = text, .line = 1, .err = err, .errlen = errlen, .doc = doc};

	if (setjmp(ps.jmp)) {
		toml_doc_free(doc);
		return NULL;
	}

	toml_value *root = new_value(&ps, TOML_TABLE);
	doc->root = root;

	/* A UTF-8 BOM is tolerated at the start of the document. */
	if ((unsigned char)ps.p[0] == 0xef && (unsigned char)ps.p[1] == 0xbb &&
	    (unsigned char)ps.p[2] == 0xbf)
		ps.p += 3;

	toml_value *cur = root;

	for (;;) {
		skip_blanks(&ps);
		if (!*ps.p)
			break;

		if (*ps.p == '[') {
			ps.p++;
			bool aot = false;
			if (*ps.p == '[') {
				aot = true;
				ps.p++;
			}
			cur = resolve_header(&ps, root, aot);
			skip_ws(&ps);
			if (*ps.p != ']')
				fail(&ps, "expected ']'");
			ps.p++;
			if (aot) {
				if (*ps.p != ']')
					fail(&ps, "expected ']]'");
				ps.p++;
			}
			skip_ws(&ps);
			skip_comment(&ps);
			continue;
		}

		char *key = NULL;
		toml_value *target = walk_key(&ps, cur, &key);
		skip_ws(&ps);
		if (*ps.p != '=')
			fail(&ps, "expected '='");
		ps.p++;
		table_set(&ps, target, key, parse_value(&ps));

		skip_ws(&ps);
		skip_comment(&ps);
		if (*ps.p && *ps.p != '\n' && *ps.p != '\r')
			fail(&ps, "trailing characters after value");
	}

	return doc;
}
