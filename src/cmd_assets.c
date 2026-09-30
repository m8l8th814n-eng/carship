#include "assets.h"
#include "cmd.h"
#include "config.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char *self_path(const char *argv0)
{
	char buf[4096];
	ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
	if (n > 0) {
		buf[n] = '\0';
		return xstrdup(buf);
	}
	return xstrdup(argv0 ? argv0 : "carship");
}

static char *asset_text(const char *name)
{
	const struct asset *a = asset_find(name);
	return a ? xstrndup(a->data, a->len) : NULL;
}

/* Substitutes the running binary's absolute path for the @CARSHIP@ marker so
 * the emitted scripts do not depend on PATH. */
static char *expand_self(const char *text, const char *self)
{
	str out = {0};
	for (const char *p = text; *p;) {
		if (has_prefix(p, "@CARSHIP@")) {
			str_add(&out, self);
			p += 9;
		} else {
			str_addc(&out, *p++);
		}
	}
	return str_take(&out);
}

static const char *const known_shells[] = {"bash", "zsh", "ash", NULL};

static bool shell_known(const char *shell)
{
	for (const char *const *s = known_shells; *s; s++)
		if (strcmp(*s, shell) == 0)
			return true;
	return false;
}

int cmd_init(int argc, char **argv, const char *argv0)
{
	const char *shell = NULL;
	bool full = false;

	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--print-full-init") == 0)
			full = true;
		else if (!shell)
			shell = argv[i];
	}

	if (!shell) {
		fputs("carship init: missing shell\n"
		      "usage: carship init <bash|zsh|ash> [--print-full-init]\n",
		      stderr);
		return 2;
	}
	if (!shell_known(shell)) {
		fprintf(stderr, "carship init: unsupported shell '%s'\n", shell);
		return 2;
	}

	char *self = self_path(argv0);

	if (!full) {
		/* The stub sources the real script so that a shell without process
		 * substitution still gets a working prompt. */
		if (strcmp(shell, "bash") == 0) {
			printf("__carship_init() {\n"
			       "  local major=\"${BASH_VERSINFO[0]}\"\n"
			       "  local minor=\"${BASH_VERSINFO[1]}\"\n"
			       "  if ((major > 4)) || { ((major == 4)) && ((minor >= 1)); }; then\n"
			       "    source <(\"%s\" init bash --print-full-init)\n"
			       "  else\n"
			       "    source /dev/stdin <<<\"$(\"%s\" init bash --print-full-init)\"\n"
			       "  fi\n"
			       "}\n"
			       "__carship_init\n"
			       "unset -f __carship_init\n",
			       self, self);
		} else if (strcmp(shell, "zsh") == 0) {
			printf("source <(\"%s\" init zsh --print-full-init)\n", self);
		} else {
			printf("eval \"$(\"%s\" init ash --print-full-init)\"\n", self);
		}
		free(self);
		return 0;
	}

	str name = {0};
	str_addf(&name, "shell/%s.sh", shell);
	char *text = asset_text(name.data);
	str_free(&name);

	if (!text) {
		free(self);
		fprintf(stderr, "carship init: no script for '%s'\n", shell);
		return 1;
	}

	char *expanded = expand_self(text, self);
	fputs(expanded, stdout);
	free(expanded);
	free(text);

	/* RPROMPT costs a whole extra process on every prompt, so only wire it up
	 * when the configuration actually defines a right_format.
	 *
	 * It is a plain string rather than a live '$(...)' on purpose. zsh
	 * re-expands both prompts on every redisplay, and the line editor
	 * redisplays from inside widgets: zle-line-finish, keymap changes, and
	 * whatever zsh-syntax-highlighting and friends hook onto them. Forking a
	 * command substitution there is what produces
	 *
	 *     azhw:zle-line-finish:9: job table full or recursion limit exceeded
	 *
	 * so the right prompt is rendered once per prompt in precmd instead. The
	 * cost is that an animated right_format only advances between commands. */
	if (strcmp(shell, "zsh") == 0) {
		config cfg;
		char err[256] = {0};
		config_load(&cfg, err, sizeof err);
		const char *right = toml_str_at(cfg.root, "right_format", NULL);

		if (right && *right)
			printf("\ncarship_rprompt() {\n"
			       "\tRPROMPT=$(\"%s\" prompt --shell=zsh --right"
			       " --terminal-width=\"$COLUMNS\" --status=\"$CARSHIP_STATUS\""
			       " --jobs=\"$CARSHIP_JOBS\" --keymap=\"$CARSHIP_KEYMAP\""
			       " --cmd-duration=\"$CARSHIP_DURATION\")\n"
			       "}\n"
			       "add-zsh-hook precmd carship_rprompt\n",
			       self);
		config_free(&cfg);
	}

	/* Completions ride along in the same output rather than costing another
	 * process at shell startup. */
	str comp_name = {0};
	str_addf(&comp_name, "shell/completion-%s.sh", shell);
	char *comp = asset_text(comp_name.data);
	str_free(&comp_name);

	if (comp) {
		printf("\nif [ -z \"${CARSHIP_NO_COMPLETION:-}\" ]; then\n");
		fputs(comp, stdout);
		printf("\nfi\n");
		free(comp);
	}

	free(self);
	return 0;
}

int cmd_completion(int argc, char **argv, const char *argv0)
{
	(void)argv0;
	if (argc < 1) {
		fputs("carship completion: missing shell\n"
		      "usage: carship completion <bash|zsh>\n",
		      stderr);
		return 2;
	}

	str name = {0};
	str_addf(&name, "shell/completion-%s.sh", argv[0]);
	char *text = asset_text(name.data);
	str_free(&name);

	if (!text) {
		fprintf(stderr, "carship completion: unsupported shell '%s'\n", argv[0]);
		return 2;
	}

	fputs(text, stdout);
	free(text);
	return 0;
}

static void list_presets(void)
{
	for (size_t i = 0; i < carship_assets_count; i++) {
		const char *name = carship_assets[i].name;
		if (!has_prefix(name, "presets/"))
			continue;

		const char *base = name + 8;
		size_t len = strlen(base);
		if (len > 5 && strcmp(base + len - 5, ".toml") == 0)
			printf("%.*s\n", (int)(len - 5), base);
	}
}

/* "preset set" copies a preset over verbatim, so the active one can be named
 * again by comparing the configuration against every embedded preset. */
static char *match_preset(const char *text, size_t len)
{
	for (size_t i = 0; i < carship_assets_count; i++) {
		const struct asset *a = &carship_assets[i];

		if (!has_prefix(a->name, "presets/"))
			continue;
		if (a->len != len || memcmp(a->data, text, len) != 0)
			continue;

		const char *base = a->name + 8;
		size_t n = strlen(base);
		if (n > 5)
			return xstrndup(base, n - 5);
	}
	return NULL;
}

static int print_current_preset(void)
{
	char *path = config_default_path();
	size_t len = 0;
	char *text = read_file(path, &len);

	if (!text) {
		fprintf(stderr, "carship preset: no configuration at %s\n", path);
		free(path);
		return 1;
	}

	char *name = match_preset(text, len);
	if (name)
		printf("%s\n", name);
	else
		fprintf(stderr,
		        "carship preset: %s does not match any preset, so it is either "
		        "hand-written or an edited copy\n",
		        path);

	int rc = name ? 0 : 1;
	free(name);
	free(text);
	free(path);
	return rc;
}

/* Keeps one copy of whatever the config file held, so that "preset set" is
 * recoverable. */
static bool backup_config(const char *path)
{
	char *existing = read_file(path, NULL);
	if (!existing)
		return true;

	str backup = {0};
	str_addf(&backup, "%s.bak", path);

	FILE *f = fopen(backup.data, "w");
	bool ok = f != NULL;
	if (f) {
		fputs(existing, f);
		fclose(f);
		fprintf(stderr, "carship: previous configuration saved to %s\n", backup.data);
	}

	str_free(&backup);
	free(existing);
	return ok;
}

int cmd_preset(int argc, char **argv, const char *argv0)
{
	(void)argv0;
	const char *name = NULL;
	const char *output = NULL;
	char *config_path = NULL;

	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--list") == 0 || strcmp(argv[i], "-l") == 0) {
			list_presets();
			return 0;
		}
		if (strcmp(argv[i], "--current") == 0 || strcmp(argv[i], "-c") == 0) {
			free(config_path);
			return print_current_preset();
		}
		if (strcmp(argv[i], "set") == 0 && !name && !config_path) {
			config_path = config_default_path();
			output = config_path;
		} else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
			if (++i >= argc) {
				fputs("carship preset: --output needs a file\n", stderr);
				free(config_path);
				return 2;
			}
			output = argv[i];
		} else if (!name) {
			name = argv[i];
		}
	}

	if (!name) {
		fputs("carship preset: missing preset name\n"
		      "usage: carship preset <name> [-o <file>]\n"
		      "       carship preset set <name>      write it to the config file\n"
		      "       carship preset --current       name the active preset\n"
		      "       carship preset --list\n",
		      stderr);
		free(config_path);
		return 2;
	}

	str path = {0};
	str_addf(&path, "presets/%s.toml", name);
	char *text = asset_text(path.data);
	str_free(&path);

	if (!text) {
		fprintf(stderr, "carship preset: unknown preset '%s'\n", name);
		free(config_path);
		return 2;
	}

	int rc = 0;
	if (output) {
		if (config_path)
			backup_config(config_path);

		FILE *f = fopen(output, "w");
		if (!f) {
			fprintf(stderr, "carship preset: cannot write '%s'\n", output);
			rc = 1;
		} else {
			fputs(text, f);
			fclose(f);
			if (config_path)
				fprintf(stderr, "carship: %s is now the active preset (%s)\n", name,
				        output);
		}
	} else {
		fputs(text, stdout);
	}

	free(text);
	free(config_path);
	return rc;
}
