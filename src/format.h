#ifndef CARSHIP_FORMAT_H
#define CARSHIP_FORMAT_H

#include "style.h"
#include "toml.h"
#include "util.h"

typedef struct fnode fnode;

typedef struct {
	const toml_value *palette;
	const esc_wrap *wrap;
	/* Double every '%' in literal text so zsh's prompt expansion leaves it
	 * alone. Variable values are escaped by the resolver. */
	bool escape_percent;
	void *user;
	/* Appends the value of `name` to out. Returns false when the variable is
	 * unknown or resolves to nothing, which lets ( ) groups drop out. */
	bool (*resolve)(void *user, const char *name, str *out);
} render_ctx;

/* Parses starship's format grammar: literal text, $variable, [text](style)
 * groups and (optional) groups. Returns NULL on a syntax error. */
fnode *fmt_parse(const char *format);
void fmt_free(fnode *n);
void fmt_render(const fnode *n, const render_ctx *ctx, str *out);

/* Expands $variables in a style spec, then applies it on top of *s. */
void fmt_apply_style_spec(style *s, const char *spec, const render_ctx *ctx);

#endif
