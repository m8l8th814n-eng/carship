#include "cmd.h"
#include "config.h"
#include "context.h"
#include "format.h"
#include "module.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
	context *ctx;
	const toml_value *root;
	const toml_value *palette;
} prompt_vars;

static bool resolve_module(void *user, const char *name, str *out)
{
	prompt_vars *pv = user;
	return module_render(name, pv->ctx, pv->root, pv->palette, out);
}

/* Measures a rendered segment as the terminal will see it: ANSI sequences are
 * already skipped by display_width, but the shell's own non-printing brackets
 * have to come out first. */
static size_t measure(const char *s, size_t len, const esc_wrap *wrap)
{
	size_t open_len = strlen(wrap->open), close_len = strlen(wrap->close);
	str clean = {0};

	for (size_t i = 0; i < len;) {
		if (open_len && len - i >= open_len && memcmp(s + i, wrap->open, open_len) == 0) {
			i += open_len;
		} else if (close_len && len - i >= close_len &&
		           memcmp(s + i, wrap->close, close_len) == 0) {
			i += close_len;
		} else {
			str_addc(&clean, s[i++]);
		}
	}

	size_t w = clean.data ? display_width(clean.data) : 0;
	str_free(&clean);
	return w;
}

/* Replaces every fill marker with enough repetitions of its symbol to push the
 * rest of the line to the right edge, splitting the slack evenly when a line
 * holds more than one. */
static char *expand_fills(const char *text, size_t len, int width, const esc_wrap *wrap)
{
	if (!memchr(text, FILL_MARKER, len))
		return xstrndup(text, len);

	str out = {0};
	size_t line_start = 0;

	while (line_start <= len) {
		const char *nl = memchr(text + line_start, '\n', len - line_start);
		size_t line_len = nl ? (size_t)(nl - (text + line_start)) : len - line_start;
		const char *line = text + line_start;

		/* Collect the markers and the line as it reads without them. */
		str visible = {0};
		const char *symbols[16];
		size_t symbol_lens[16], nmarkers = 0;

		for (size_t i = 0; i < line_len;) {
			if (line[i] != FILL_MARKER) {
				str_addc(&visible, line[i++]);
				continue;
			}
			size_t j = i + 1;
			while (j < line_len && line[j] != FILL_MARKER)
				j++;
			if (nmarkers < 16) {
				symbols[nmarkers] = line + i + 1;
				symbol_lens[nmarkers] = j - i - 1;
				nmarkers++;
			}
			i = (j < line_len) ? j + 1 : j;
		}

		if (!nmarkers) {
			str_addn(&out, line, line_len);
		} else {
			size_t used = visible.data ? measure(visible.data, visible.len, wrap) : 0;
			size_t slack = (width > 0 && (size_t)width > used) ? (size_t)width - used : 0;
			size_t share = slack / nmarkers;
			size_t extra = slack % nmarkers;

			size_t seen = 0;
			for (size_t i = 0; i < line_len;) {
				if (line[i] != FILL_MARKER) {
					str_addc(&out, line[i++]);
					continue;
				}
				size_t j = i + 1;
				while (j < line_len && line[j] != FILL_MARKER)
					j++;

				size_t columns = share + (seen < extra ? 1 : 0);
				const char *sym = symbols[seen < 16 ? seen : 15];
				size_t sym_len = symbol_lens[seen < 16 ? seen : 15];

				str one = {0};
				str_addn(&one, sym, sym_len);
				size_t sym_w = one.data ? display_width(one.data) : 0;

				for (size_t c = 0; sym_w && c + sym_w <= columns; c += sym_w)
					str_addn(&out, one.data, one.len);
				if (sym_w)
					for (size_t c = (columns / sym_w) * sym_w; c < columns; c++)
						str_addc(&out, ' ');
				str_free(&one);

				seen++;
				i = (j < line_len) ? j + 1 : j;
			}
		}

		str_free(&visible);

		if (!nl)
			break;
		str_addc(&out, '\n');
		line_start += line_len + 1;
	}

	return str_take(&out);
}

/* Accepts both --flag=value and --flag value. */
static const char *take_flag(const char *name, int *i, int argc, char **argv)
{
	const char *arg = argv[*i];
	size_t len = strlen(name);

	if (strncmp(arg, name, len) != 0)
		return NULL;

	if (arg[len] == '=')
		return arg + len + 1;
	if (arg[len] != '\0')
		return NULL;
	if (*i + 1 >= argc)
		return "";

	(*i)++;
	return argv[*i];
}

int cmd_prompt(int argc, char **argv)
{
	context ctx;
	ctx_init(&ctx);

	bool continuation = false;
	bool right = false;
	const char *shell = getenv("CARSHIP_SHELL");

	for (int i = 0; i < argc; i++) {
		const char *v;

		if ((v = take_flag("--status", &i, argc, argv)))
			ctx.status = atoi(v);
		else if ((v = take_flag("--cmd-duration", &i, argc, argv)))
			ctx.duration_ms = *v ? atoll(v) : -1;
		else if ((v = take_flag("--jobs", &i, argc, argv)))
			ctx.jobs = atoi(v);
		else if ((v = take_flag("--keymap", &i, argc, argv))) {
			free(ctx.keymap);
			ctx.keymap = xstrdup(v);
		} else if ((v = take_flag("--terminal-width", &i, argc, argv))) {
			int w = atoi(v);
			if (w > 0)
				ctx.width = w;
		} else if ((v = take_flag("--pipestatus", &i, argc, argv))) {
			free(ctx.pipestatus);
			ctx.pipestatus = xstrdup(v);
		} else if ((v = take_flag("--shell", &i, argc, argv)))
			shell = v;
		else if (strcmp(argv[i], "--continuation") == 0)
			continuation = true;
		else if (strcmp(argv[i], "--right") == 0)
			right = true;
	}

	ctx_set_shell(&ctx, shell ? shell : "");

	config cfg;
	char err[256] = {0};
	if (!config_load(&cfg, err, sizeof err))
		fprintf(stderr, "carship: %s: %s\n", cfg.path, err);

	prompt_vars pv = {.ctx = &ctx, .root = cfg.root, .palette = cfg.palette};
	render_ctx rc = {
		.palette = cfg.palette,
		.wrap = ctx.wrap,
		.escape_percent = ctx.wrap == &esc_wrap_zsh,
		.user = &pv,
		.resolve = resolve_module,
	};

	const char *format;
	if (continuation)
		format = toml_str_at(cfg.root, "continuation_prompt", "[∙](bright-black) ");
	else if (right)
		format = toml_str_at(cfg.root, "right_format", "");
	else
		format = toml_str_at(cfg.root, "format", CARSHIP_DEFAULT_FORMAT);

	fnode *fmt = fmt_parse(format);
	str out = {0};

	if (!fmt) {
		fprintf(stderr, "carship: invalid format string\n");
		/* A usable fallback only makes sense for the prompt itself; putting
		 * it in the right or continuation prompt just prints noise. */
		if (!continuation && !right)
			str_add(&out, "carship> ");
	} else {
		if (!continuation && !right && toml_bool_at(cfg.root, "add_newline", true))
			str_addc(&out, '\n');
		fmt_render(fmt, &rc, &out);
	}

	char *final = expand_fills(out.data ? out.data : "", out.len, ctx.width, ctx.wrap);
	fputs(final, stdout);

	free(final);
	str_free(&out);
	fmt_free(fmt);
	config_free(&cfg);
	ctx_free(&ctx);
	return 0;
}

int cmd_module(int argc, char **argv)
{
	if (argc < 1) {
		fputs("carship module: missing module name\n"
		      "usage: carship module <name> | carship module --list\n",
		      stderr);
		return 2;
	}

	if (strcmp(argv[0], "--list") == 0 || strcmp(argv[0], "-l") == 0) {
		for (size_t i = 0; i < carship_module_order_count; i++)
			printf("%s\n", carship_module_order[i]);
		return 0;
	}

	if (!module_lookup(argv[0])) {
		fprintf(stderr, "carship module: unknown module '%s'\n", argv[0]);
		return 2;
	}

	context ctx;
	ctx_init(&ctx);
	ctx_set_shell(&ctx, getenv("CARSHIP_SHELL"));

	config cfg;
	char err[256] = {0};
	config_load(&cfg, err, sizeof err);

	/* Asking for a module by name is an explicit request, so say why nothing
	 * came out rather than printing a blank line. */
	str out = {0};
	const char *why = NULL;
	bool shown = module_render_ex(argv[0], &ctx, cfg.root, cfg.palette, &out, &why);

	if (shown) {
		fwrite(out.data, 1, out.len, stdout);
		fputc('\n', stdout);
	} else {
		fprintf(stderr, "carship module: %s: %s\n", argv[0],
		        why ? why : "nothing to report here");
	}

	str_free(&out);
	config_free(&cfg);
	ctx_free(&ctx);
	return shown ? 0 : 1;
}

int cmd_explain(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	context ctx;
	ctx_init(&ctx);
	ctx_set_shell(&ctx, getenv("CARSHIP_SHELL"));

	config cfg;
	char err[256] = {0};
	config_load(&cfg, err, sizeof err);

	/* Every module is listed, including the quiet ones, so that it is obvious
	 * what exists and why it is not showing. */
	printf("%-16s %s\n", "MODULE", "OUTPUT");
	for (size_t i = 0; i < carship_module_order_count; i++) {
		const char *name = carship_module_order[i];
		const char *why = NULL;
		str out = {0};

		if (module_render_ex(name, &ctx, cfg.root, cfg.palette, &out, &why)) {
			/* line_break renders a newline, which would break the table. */
			str shown = {0};
			for (size_t j = 0; j < out.len; j++)
				if (out.data[j] == '\n')
					str_add(&shown, "\\n");
				else
					str_addc(&shown, out.data[j]);
			printf("%-16s %s\n", name, shown.data ? shown.data : "");
			str_free(&shown);
		} else {
			printf("%-16s \x1b[2m- %s\x1b[0m\n", name, why ? why : "");
		}
		str_free(&out);
	}

	config_free(&cfg);
	ctx_free(&ctx);
	return 0;
}

int cmd_print_config(void)
{
	char *path = config_default_path();
	char *text = read_file(path, NULL);

	printf("# %s\n", path);
	if (text)
		fputs(text, stdout);
	else
		printf("# (no configuration file yet)\n");

	free(text);
	free(path);
	return 0;
}

int cmd_config(void)
{
	char *path = config_default_path();
	const char *editor = getenv("VISUAL");
	if (!editor || !*editor)
		editor = getenv("EDITOR");
	if (!editor || !*editor)
		editor = "vi";

	char *args[] = {(char *)editor, path, NULL};
	execvp(editor, args);

	fprintf(stderr, "carship config: cannot run '%s'\n", editor);
	free(path);
	return 1;
}
