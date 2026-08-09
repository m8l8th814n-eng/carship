#include "module.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *replace_all(const char *hay, const char *needle, const char *with)
{
	size_t nlen = strlen(needle);
	if (!nlen)
		return xstrdup(hay);

	str out = {0};
	for (const char *p = hay; *p;) {
		if (strncmp(p, needle, nlen) == 0) {
			str_add(&out, with);
			p += nlen;
		} else {
			str_addc(&out, *p++);
		}
	}
	return str_take(&out);
}

static const char *basename_of(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash && slash[1] ? slash + 1 : path;
}

/* Keeps the trailing `keep` components, prefixing `symbol` when anything was
 * dropped. A leading "~" or "/" counts as a component of its own. */
static char *truncate_path(const char *path, long long keep, const char *symbol)
{
	if (keep <= 0)
		return xstrdup(path);

	size_t count = 0;
	for (const char *p = path; *p; p++)
		if (*p == '/')
			count++;

	bool absolute = (path[0] == '/');
	size_t components = absolute ? count : count + 1;
	if (components <= (size_t)keep)
		return xstrdup(path);

	size_t drop = components - (size_t)keep;
	const char *p = path;
	if (absolute)
		p++;

	for (size_t i = 0; i < drop; i++) {
		const char *slash = strchr(p, '/');
		if (!slash)
			break;
		p = slash + 1;
	}

	str out = {0};
	str_add(&out, symbol);
	str_add(&out, p);
	return str_take(&out);
}

static bool probe_directory(module_state *st)
{
	context *ctx = st->ctx;
	const char *home_symbol = toml_str_at(st->cfg, "home_symbol", "~");
	long long truncation_length = toml_int_at(st->cfg, "truncation_length", 3);
	const char *truncation_symbol = toml_str_at(st->cfg, "truncation_symbol", "");
	bool truncate_to_repo = toml_bool_at(st->cfg, "truncate_to_repo", true);

	const char *cwd = ctx->cwd;
	const char *git_root = truncate_to_repo ? ctx_git_root(ctx) : NULL;
	str display = {0};

	bool home_is_repo = git_root && ctx->home[0] && strcmp(git_root, ctx->home) == 0;

	if (git_root && !home_is_repo) {
		/* Everything above the repository root was cut, so say so. */
		if (strchr(git_root + 1, '/'))
			str_add(&display, truncation_symbol);
		str_add(&display, basename_of(git_root));
		str_add(&display, cwd + strlen(git_root));
	} else if (ctx->home[0] && has_prefix(cwd, ctx->home) &&
	           (cwd[strlen(ctx->home)] == '/' || cwd[strlen(ctx->home)] == '\0')) {
		str_add(&display, home_symbol);
		str_add(&display, cwd + strlen(ctx->home));
	} else {
		str_add(&display, cwd);
	}

	char *path = str_take(&display);

	const toml_value *subs = toml_get(st->cfg, "substitutions");
	if (subs && subs->type == TOML_TABLE) {
		for (size_t i = 0; i < subs->tab.n; i++) {
			const char *with = toml_str(subs->tab.vals[i], NULL);
			if (!with)
				continue;
			char *next = replace_all(path, subs->tab.keys[i], with);
			free(path);
			path = next;
		}
	}

	/* The repo-relative form is already short; only plain paths get cut. */
	if (!git_root || home_is_repo) {
		char *cut = truncate_path(path, truncation_length, truncation_symbol);
		free(path);
		path = cut;
	}

	/* A custom separator replaces every "/" except a leading root slash. */
	const char *separator = toml_str_at(st->cfg, "path_separator", NULL);
	if (separator) {
		str joined = {0};
		const char *p = path;
		if (*p == '/')
			str_addc(&joined, *p++);
		for (; *p; p++) {
			if (*p == '/')
				str_add(&joined, separator);
			else
				str_addc(&joined, *p);
		}
		free(path);
		path = str_take(&joined);
	}

	var_add(&st->vars, "path", path);

	bool writable = access(cwd, W_OK) == 0;
	var_addz(&st->vars, "read_only",
	         writable ? "" : toml_str_at(st->cfg, "read_only", " 🔒"));
	var_addz(&st->vars, "read_only_style",
	         toml_str_at(st->cfg, "read_only_style", "red"));
	return true;
}

const module_def directory_modules[] = {
	{.name = "directory", .format = "[$path]($style)[$read_only]($read_only_style) ",
	 .style = "bold cyan", .probe = probe_directory},
};

const size_t directory_modules_count = sizeof directory_modules / sizeof *directory_modules;
