#include "module.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void var_add(varset *v, const char *name, char *owned)
{
	if (v->n >= MODULE_MAX_VARS) {
		free(owned);
		return;
	}
	v->names[v->n] = name;
	v->values[v->n] = owned;
	v->raw[v->n] = false;
	v->n++;
}

void var_add_raw(varset *v, const char *name, char *owned)
{
	var_add(v, name, owned);
	if (v->n)
		v->raw[v->n - 1] = true;
}

void var_addz(varset *v, const char *name, const char *value)
{
	var_add(v, name, value ? xstrdup(value) : NULL);
}

void var_addf(varset *v, const char *name, const char *fmt, ...)
{
	str s = {0};
	va_list ap;
	va_start(ap, fmt);
	va_list copy;
	va_copy(copy, ap);
	int n = vsnprintf(NULL, 0, fmt, copy);
	va_end(copy);
	if (n > 0) {
		str_reserve(&s, (size_t)n);
		vsnprintf(s.data, (size_t)n + 1, fmt, ap);
		s.len = (size_t)n;
	}
	va_end(ap);
	var_add(v, name, str_take(&s));
}

void varset_free(varset *v)
{
	for (size_t i = 0; i < v->n; i++)
		free(v->values[i]);
	v->n = 0;
}

extern const module_def basic_modules[];
extern const size_t basic_modules_count;
extern const module_def directory_modules[];
extern const size_t directory_modules_count;
extern const module_def git_modules[];
extern const size_t git_modules_count;
extern const module_def lang_modules[];
extern const size_t lang_modules_count;
extern const module_def env_modules[];
extern const size_t env_modules_count;
extern const module_def animation_modules[];
extern const size_t animation_modules_count;
extern const module_def system_modules[];
extern const size_t system_modules_count;

const module_def *module_lookup(const char *name)
{
	const struct {
		const module_def *defs;
		const size_t *count;
	} registries[] = {
		{basic_modules, &basic_modules_count},
		{directory_modules, &directory_modules_count},
		{git_modules, &git_modules_count},
		{lang_modules, &lang_modules_count},
		{env_modules, &env_modules_count},
		{animation_modules, &animation_modules_count},
		{system_modules, &system_modules_count},
	};

	for (size_t r = 0; r < sizeof registries / sizeof *registries; r++)
		for (size_t i = 0; i < *registries[r].count; i++)
			if (strcmp(registries[r].defs[i].name, name) == 0)
				return &registries[r].defs[i];
	return NULL;
}

typedef struct {
	const varset *vars;
	/* zsh runs prompt expansion over whatever we print, so a '%' coming from
	 * a path or branch name has to be doubled or it is eaten (or worse,
	 * "%d" expands to the working directory). */
	bool escape_percent;
} var_ctx;

static bool var_resolve(void *user, const char *name, str *out)
{
	const var_ctx *vc = user;
	const varset *v = vc->vars;

	for (size_t i = 0; i < v->n; i++) {
		if (strcmp(v->names[i], name) != 0)
			continue;
		if (!v->values[i])
			return false;

		const char *value = v->values[i];
		if (vc->escape_percent && !v->raw[i]) {
			for (const char *p = value; *p; p++) {
				str_addc(out, *p);
				if (*p == '%')
					str_addc(out, '%');
			}
		} else {
			str_add(out, value);
		}
		return value[0] != '\0';
	}
	return false;
}

char *module_render_sub(module_state *st, const char *format)
{
	fnode *n = fmt_parse(format);
	if (!n)
		return xstrdup(format ? format : "");

	var_ctx vc = {.vars = &st->vars, .escape_percent = st->ctx->wrap == &esc_wrap_zsh};
	render_ctx rc = {
		.palette = st->palette,
		.wrap = st->ctx->wrap,
		.escape_percent = vc.escape_percent,
		.user = &vc,
		.resolve = var_resolve,
	};

	str out = {0};
	fmt_render(n, &rc, &out);
	fmt_free(n);
	return str_take(&out);
}

bool module_render(const char *name, context *ctx, const toml_value *root,
                   const toml_value *palette, str *out)
{
	return module_render_ex(name, ctx, root, palette, out, NULL);
}

bool module_render_ex(const char *name, context *ctx, const toml_value *root,
                      const toml_value *palette, str *out, const char **why)
{
	if (why)
		*why = NULL;

	const module_def *def = module_lookup(name);
	if (!def) {
		if (why)
			*why = "no such module";
		return false;
	}

	const toml_value *cfg = toml_get(root, name);
	if (toml_bool_at(cfg, "disabled", def->disabled)) {
		if (why)
			*why = def->disabled && !toml_get(cfg, "disabled")
			               ? "off by default; set disabled = false to use it"
			               : "disabled in the configuration";
		return false;
	}

	module_state st = {.ctx = ctx, .def = def, .cfg = cfg, .palette = palette};
	if (!def->probe(&st)) {
		varset_free(&st.vars);
		if (why)
			*why = "nothing to report here";
		return false;
	}

	const char *style = toml_str_at(cfg, "style", def->style);
	var_add_raw(&st.vars, "style", xstrdup(style ? style : ""));

	fnode *fmt = fmt_parse(toml_str_at(cfg, "format", def->format));
	if (!fmt) {
		varset_free(&st.vars);
		return false;
	}

	var_ctx vc = {.vars = &st.vars, .escape_percent = ctx->wrap == &esc_wrap_zsh};
	render_ctx rc = {
		.palette = palette,
		.wrap = ctx->wrap,
		.escape_percent = vc.escape_percent,
		.user = &vc,
		.resolve = var_resolve,
	};

	str rendered = {0};
	fmt_render(fmt, &rc, &rendered);
	bool produced = rendered.len > 0;
	if (produced)
		str_addn(out, rendered.data, rendered.len);

	str_free(&rendered);
	fmt_free(fmt);
	varset_free(&st.vars);
	return produced;
}

const char *const carship_module_order[] = {
	"os",        "username",   "hostname",  "localip",    "ip",         "shlvl",
	"container", "nix_shell",  "directory", "git_branch", "git_state",  "git_status",
	"git_metrics", "bun",      "c",         "cpp",        "elixir",     "elm",
	"golang",    "gradle",     "haskell",   "java",       "julia",      "kotlin",
	"maven",     "nim",        "nodejs",    "php",        "python",     "rust",
	"scala",     "conda",      "pixi",      "docker_context", "env_var", "sudo",
	"jobs",      "cmd_duration", "status",  "time",       "uptime",     "shell",
	"tty",       "tailscale",  "bluetooth",
	"animation",
	"fill",
	"line_break",
	"character",
};

const size_t carship_module_order_count =
	sizeof carship_module_order / sizeof *carship_module_order;
