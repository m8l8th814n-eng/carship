#ifndef CARSHIP_MODULE_H
#define CARSHIP_MODULE_H

#include "context.h"
#include "format.h"
#include "toml.h"

#define MODULE_MAX_VARS 32

/* Delimits a fill module's padding character inside a rendered prompt; the
 * prompt renderer replaces the whole marker once line widths are known. */
#define FILL_MARKER '\x1e'

typedef struct {
	const char *names[MODULE_MAX_VARS];
	char *values[MODULE_MAX_VARS];
	/* Values already carrying prompt markup, which must not be escaped. */
	bool raw[MODULE_MAX_VARS];
	size_t n;
} varset;

void var_add(varset *v, const char *name, char *owned);
void var_addz(varset *v, const char *name, const char *value);
void var_addf(varset *v, const char *name, const char *fmt, ...);
void var_add_raw(varset *v, const char *name, char *owned);
void varset_free(varset *v);

typedef struct module_def module_def;

typedef struct {
	context *ctx;
	const module_def *def;
	const toml_value *cfg;
	const toml_value *palette;
	varset vars;
} module_state;

struct module_def {
	const char *name;
	const char *format;
	const char *style;
	bool disabled;
	const void *data; /* module-specific table, e.g. a language spec */
	/* Fills st->vars and returns false when the module should stay hidden. */
	bool (*probe)(module_state *st);
};

/* Renders a nested format string (a symbol, say) against the vars gathered so
 * far. The caller owns the result. */
char *module_render_sub(module_state *st, const char *format);

const module_def *module_lookup(const char *name);
bool module_render(const char *name, context *ctx, const toml_value *root,
                   const toml_value *palette, str *out);
/* Same, but reports through *why why nothing was produced. */
bool module_render_ex(const char *name, context *ctx, const toml_value *root,
                      const toml_value *palette, str *out, const char **why);

/* Module names in the order the default prompt format uses them. */
extern const char *const carship_module_order[];
extern const size_t carship_module_order_count;

#endif
