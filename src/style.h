#ifndef CARSHIP_STYLE_H
#define CARSHIP_STYLE_H

#include "toml.h"
#include "util.h"

#include <stdint.h>

typedef enum {
	COLOR_NONE,
	COLOR_ANSI,    /* 0-255, index into the terminal palette */
	COLOR_RGB,     /* 24-bit truecolor */
	COLOR_PREV_FG, /* resolved against the enclosing style */
	COLOR_PREV_BG,
} color_kind;

typedef struct {
	color_kind kind;
	uint8_t idx;
	uint8_t r, g, b;
} color;

typedef struct {
	color fg, bg;
	bool bold, italic, underline, dimmed, inverted, blink, strikethrough, hidden;
} style;

/* Non-printing sequences must be bracketed so the shell can compute the
 * prompt's display width. */
typedef struct {
	const char *open;
	const char *close;
} esc_wrap;

extern const esc_wrap esc_wrap_none;
extern const esc_wrap esc_wrap_bash;
extern const esc_wrap esc_wrap_zsh;

/* Applies a starship style spec ("bold fg:red bg:#1e1e2e") on top of *s. */
void style_apply(style *s, const char *spec, const toml_value *palette);

bool style_is_plain(const style *s);
/* Writes the SGR sequence for *s, bracketed per *w. */
void style_open(str *out, const style *s, const esc_wrap *w);
void style_close(str *out, const esc_wrap *w);

#endif
