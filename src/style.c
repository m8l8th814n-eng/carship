#include "style.h"

#include <stdlib.h>
#include <string.h>

const esc_wrap esc_wrap_none = {"", ""};
/* bash expands PS1's backslash escapes before command substitution runs, so
 * "\[" from our output would never be interpreted. readline's raw markers are
 * what survive the round trip. */
const esc_wrap esc_wrap_bash = {"\001", "\002"};
const esc_wrap esc_wrap_zsh = {"%{", "%}"};

static const struct {
	const char *name;
	uint8_t idx;
} named_colors[] = {
	{"black", 0},          {"red", 1},           {"green", 2},         {"yellow", 3},
	{"blue", 4},           {"purple", 5},        {"magenta", 5},       {"cyan", 6},
	{"white", 7},          {"bright-black", 8},  {"bright-red", 9},    {"bright-green", 10},
	{"bright-yellow", 11}, {"bright-blue", 12},  {"bright-purple", 13},
	{"bright-magenta", 13}, {"bright-cyan", 14}, {"bright-white", 15},
};

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static bool parse_hex_color(const char *s, color *out)
{
	size_t len = strlen(s);
	if (len != 7 && len != 4)
		return false;

	int v[6];
	size_t digits = len - 1;
	for (size_t i = 0; i < digits; i++) {
		v[i] = hex_digit(s[i + 1]);
		if (v[i] < 0)
			return false;
	}

	out->kind = COLOR_RGB;
	if (digits == 6) {
		out->r = (uint8_t)(v[0] * 16 + v[1]);
		out->g = (uint8_t)(v[2] * 16 + v[3]);
		out->b = (uint8_t)(v[4] * 16 + v[5]);
	} else {
		out->r = (uint8_t)(v[0] * 17);
		out->g = (uint8_t)(v[1] * 17);
		out->b = (uint8_t)(v[2] * 17);
	}
	return true;
}

static bool resolve_color(const char *name, const toml_value *palette, color *out, int depth)
{
	if (!name || !*name || depth > 8)
		return false;

	if (name[0] == '#')
		return parse_hex_color(name, out);

	if (name[0] >= '0' && name[0] <= '9') {
		char *end;
		long n = strtol(name, &end, 10);
		if (*end == '\0' && n >= 0 && n <= 255) {
			out->kind = COLOR_ANSI;
			out->idx = (uint8_t)n;
			return true;
		}
		return false;
	}

	if (strcmp(name, "prev_fg") == 0) {
		out->kind = COLOR_PREV_FG;
		return true;
	}
	if (strcmp(name, "prev_bg") == 0) {
		out->kind = COLOR_PREV_BG;
		return true;
	}

	/* The palette wins over the built-in names: themes such as catppuccin
	 * deliberately redefine "green", "red" and friends. Entries may also
	 * alias one another. */
	const char *alias = toml_str_at(palette, name, NULL);
	if (alias)
		return resolve_color(alias, palette, out, depth + 1);

	for (size_t i = 0; i < sizeof named_colors / sizeof *named_colors; i++) {
		if (strcmp(name, named_colors[i].name) == 0) {
			out->kind = COLOR_ANSI;
			out->idx = named_colors[i].idx;
			return true;
		}
	}

	return false;
}

void style_apply(style *s, const char *spec, const toml_value *palette)
{
	if (!spec)
		return;

	const char *p = spec;
	while (*p) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;

		const char *start = p;
		while (*p && *p != ' ' && *p != '\t')
			p++;

		size_t len = (size_t)(p - start);
		char tok[128];
		if (len >= sizeof tok)
			continue;
		memcpy(tok, start, len);
		tok[len] = '\0';

		if (strcmp(tok, "none") == 0) {
			memset(s, 0, sizeof *s);
		} else if (strcmp(tok, "bold") == 0) {
			s->bold = true;
		} else if (strcmp(tok, "italic") == 0) {
			s->italic = true;
		} else if (strcmp(tok, "underline") == 0) {
			s->underline = true;
		} else if (strcmp(tok, "dimmed") == 0) {
			s->dimmed = true;
		} else if (strcmp(tok, "inverted") == 0) {
			s->inverted = true;
		} else if (strcmp(tok, "blink") == 0) {
			s->blink = true;
		} else if (strcmp(tok, "strikethrough") == 0) {
			s->strikethrough = true;
		} else if (strcmp(tok, "hidden") == 0) {
			s->hidden = true;
		} else if (has_prefix(tok, "fg:")) {
			resolve_color(tok + 3, palette, &s->fg, 0);
		} else if (has_prefix(tok, "bg:")) {
			resolve_color(tok + 3, palette, &s->bg, 0);
		} else {
			resolve_color(tok, palette, &s->fg, 0);
		}
	}
}

bool style_is_plain(const style *s)
{
	return s->fg.kind == COLOR_NONE && s->bg.kind == COLOR_NONE && !s->bold && !s->italic &&
	       !s->underline && !s->dimmed && !s->inverted && !s->blink && !s->strikethrough &&
	       !s->hidden;
}

static void add_code(str *out, bool *first, const char *code)
{
	if (!*first)
		str_addc(out, ';');
	str_add(out, code);
	*first = false;
}

static void add_color(str *out, bool *first, const color *c, bool background)
{
	if (c->kind == COLOR_NONE || c->kind == COLOR_PREV_FG || c->kind == COLOR_PREV_BG)
		return;

	if (!*first)
		str_addc(out, ';');
	*first = false;

	if (c->kind == COLOR_RGB) {
		str_addf(out, "%d;2;%u;%u;%u", background ? 48 : 38, c->r, c->g, c->b);
	} else if (c->idx < 8) {
		str_addf(out, "%d", (background ? 40 : 30) + c->idx);
	} else if (c->idx < 16) {
		str_addf(out, "%d", (background ? 100 : 90) + (c->idx - 8));
	} else {
		str_addf(out, "%d;5;%u", background ? 48 : 38, c->idx);
	}
}

void style_open(str *out, const style *s, const esc_wrap *w)
{
	if (style_is_plain(s))
		return;

	str seq = {0};
	bool first = true;

	str_add(&seq, "\x1b[");
	if (s->bold)
		add_code(&seq, &first, "1");
	if (s->dimmed)
		add_code(&seq, &first, "2");
	if (s->italic)
		add_code(&seq, &first, "3");
	if (s->underline)
		add_code(&seq, &first, "4");
	if (s->blink)
		add_code(&seq, &first, "5");
	if (s->inverted)
		add_code(&seq, &first, "7");
	if (s->hidden)
		add_code(&seq, &first, "8");
	if (s->strikethrough)
		add_code(&seq, &first, "9");
	add_color(&seq, &first, &s->fg, false);
	add_color(&seq, &first, &s->bg, true);
	str_addc(&seq, 'm');

	str_add(out, w->open);
	str_add(out, seq.data);
	str_add(out, w->close);
	str_free(&seq);
}

void style_close(str *out, const esc_wrap *w)
{
	str_add(out, w->open);
	str_add(out, "\x1b[0m");
	str_add(out, w->close);
}
