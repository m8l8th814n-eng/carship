#include "module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	const char *const *extensions;
	const char *const *files;
	const char *const *folders;
	const char *const *cmd;
	const char *symbol;
	/* Some tools print an unrelated banner first; pick the line that starts
	 * with this instead of the first one. */
	const char *version_line;
} lang_spec;

static bool detected(context *ctx, const lang_spec *spec)
{
	for (const char *const *e = spec->extensions; e && *e; e++)
		if (ctx_has_extension(ctx, *e))
			return true;
	for (const char *const *f = spec->files; f && *f; f++)
		if (ctx_has_file(ctx, *f))
			return true;
	for (const char *const *d = spec->folders; d && *d; d++)
		if (ctx_has_file(ctx, *d))
			return true;
	return false;
}

static const char *select_line(const char *out, const char *prefix)
{
	if (!prefix)
		return out;

	size_t len = strlen(prefix);
	for (const char *line = out; line && *line;) {
		if (strncmp(line, prefix, len) == 0)
			return line;
		const char *eol = strchr(line, '\n');
		line = eol ? eol + 1 : NULL;
	}
	return out;
}

/* Pulls the first dotted number run out of a --version banner, which covers
 * "v20.11.0", "go version go1.22.0 …" and "rustc 1.72.0 (…)" alike. */
static char *extract_version(const char *out)
{
	for (const char *p = out; *p && *p != '\n'; p++) {
		if (*p < '0' || *p > '9')
			continue;

		const char *start = p;
		while ((*p >= '0' && *p <= '9') || (*p == '.' && p[1] >= '0' && p[1] <= '9'))
			p++;
		if (p - start >= 1)
			return xstrndup(start, (size_t)(p - start));
	}
	return NULL;
}

static char *apply_version_format(const char *fmt, const char *raw)
{
	char major[32] = "", minor[32] = "", patch[32] = "";
	sscanf(raw, "%31[^.].%31[^.].%31s", major, minor, patch);

	str out = {0};
	for (const char *p = fmt; *p;) {
		if (has_prefix(p, "${raw}")) {
			str_add(&out, raw);
			p += 6;
		} else if (has_prefix(p, "${major}")) {
			str_add(&out, major);
			p += 8;
		} else if (has_prefix(p, "${minor}")) {
			str_add(&out, minor);
			p += 8;
		} else if (has_prefix(p, "${patch}")) {
			str_add(&out, patch);
			p += 8;
		} else {
			str_addc(&out, *p++);
		}
	}
	return str_take(&out);
}

static bool probe_lang(module_state *st)
{
	const lang_spec *spec = st->def->data;
	if (!detected(st->ctx, spec))
		return false;

	var_addz(&st->vars, "symbol", toml_str_at(st->cfg, "symbol", spec->symbol));

	if (!toml_bool_at(st->cfg, "show_version", true))
		return true;

	long long timeout = toml_int_at(st->cfg, "command_timeout", 500);
	char *out = capture(st->ctx->cwd, (char *const *)spec->cmd, (int)timeout);
	if (!out)
		return true;

	char *raw = extract_version(select_line(out, spec->version_line));
	free(out);
	if (!raw)
		return true;

	const char *fmt = toml_str_at(st->cfg, "version_format", "v${raw}");
	var_add(&st->vars, "version", apply_version_format(fmt, raw));
	free(raw);
	return true;
}

static bool probe_conda(module_state *st)
{
	const char *env = getenv("CONDA_DEFAULT_ENV");
	if (!env || !*env)
		return false;

	long long truncation = toml_int_at(st->cfg, "truncation_length", 1);
	const char *shown = env;
	if (truncation > 0) {
		const char *slash = strrchr(env, '/');
		if (slash && slash[1])
			shown = slash + 1;
	}

	var_addz(&st->vars, "symbol", toml_str_at(st->cfg, "symbol", "🅒 "));
	var_addz(&st->vars, "environment", shown);
	return true;
}

static bool probe_docker_context(module_state *st)
{
	const char *ctx_name = getenv("DOCKER_CONTEXT");
	char *owned = NULL;

	if (!ctx_name || !*ctx_name) {
		const char *home = st->ctx->home;
		if (!home || !*home)
			return false;

		str path = {0};
		str_addf(&path, "%s/.docker/config.json", home);
		char *content = read_file(path.data, NULL);
		str_free(&path);
		if (!content)
			return false;

		char *key = strstr(content, "\"currentContext\"");
		if (key) {
			char *colon = strchr(key + 16, ':');
			char *open = colon ? strchr(colon, '"') : NULL;
			char *close = open ? strchr(open + 1, '"') : NULL;
			if (close)
				owned = xstrndup(open + 1, (size_t)(close - open - 1));
		}
		free(content);

		if (!owned)
			return false;
		ctx_name = owned;
	}

	if (strcmp(ctx_name, "default") == 0) {
		free(owned);
		return false;
	}

	var_addz(&st->vars, "symbol", toml_str_at(st->cfg, "symbol", "\uf308 "));
	var_addz(&st->vars, "context", ctx_name);
	free(owned);
	return true;
}

#define LIST(...) ((const char *const[]){__VA_ARGS__, NULL})

static const lang_spec spec_c = {
	.extensions = LIST("c", "h"),
	.cmd = LIST("cc", "--version"),
	.symbol = "C ",
};

static const lang_spec spec_golang = {
	.extensions = LIST("go"),
	.files = LIST("go.mod", "go.sum", "go.work", "glide.yaml", "Gopkg.yml", "Gopkg.lock",
	              ".go-version"),
	.folders = LIST("Godeps"),
	.cmd = LIST("go", "version"),
	.symbol = "🐹 ",
};

static const lang_spec spec_haskell = {
	.extensions = LIST("hs", "cabal", "hs-boot"),
	.files = LIST("stack.yaml", "cabal.project"),
	.cmd = LIST("ghc", "--numeric-version"),
	.symbol = "λ ",
};

static const lang_spec spec_java = {
	.extensions = LIST("java", "class", "gradle", "jar", "cljs", "cljc"),
	.files = LIST("pom.xml", "build.gradle.kts", "build.sbt", ".java-version", "deps.edn",
	              "project.clj", "build.boot", ".sdkmanrc"),
	.cmd = LIST("java", "--version"),
	.symbol = "☕ ",
};

static const lang_spec spec_kotlin = {
	.extensions = LIST("kt", "kts"),
	.cmd = LIST("kotlin", "-version"),
	.symbol = "🅺 ",
};

static const lang_spec spec_nodejs = {
	.extensions = LIST("js", "mjs", "cjs", "ts", "mts", "cts"),
	.files = LIST("package.json", ".node-version", ".nvmrc"),
	.folders = LIST("node_modules"),
	.cmd = LIST("node", "--version"),
	.symbol = "\ue718 ",
};

static const lang_spec spec_php = {
	.extensions = LIST("php"),
	.files = LIST("composer.json", ".php-version"),
	.cmd = LIST("php", "--version"),
	.symbol = "🐘 ",
};

static const lang_spec spec_python = {
	.extensions = LIST("py"),
	.files = LIST("requirements.txt", ".python-version", "pyproject.toml", "Pipfile", "tox.ini",
	              "setup.py", "__init__.py"),
	.cmd = LIST("python3", "--version"),
	.symbol = "🐍 ",
};

static const lang_spec spec_rust = {
	.extensions = LIST("rs"),
	.files = LIST("Cargo.toml"),
	.cmd = LIST("rustc", "--version"),
	.symbol = "🦀 ",
};

static const lang_spec spec_bun = {
	.files = LIST("bun.lockb", "bun.lock", "bunfig.toml"),
	.cmd = LIST("bun", "--version"),
	.symbol = "🍞 ",
};

static const lang_spec spec_cpp = {
	.extensions = LIST("cpp", "cc", "cxx", "hpp", "hh", "hxx"),
	.cmd = LIST("c++", "--version"),
	.symbol = "C++ ",
};

static const lang_spec spec_elixir = {
	.extensions = LIST("ex", "exs"),
	.files = LIST("mix.exs"),
	.cmd = LIST("elixir", "--version"),
	.symbol = "💧 ",
	.version_line = "Elixir",
};

static const lang_spec spec_elm = {
	.extensions = LIST("elm"),
	.files = LIST("elm.json", "elm-package.json", ".elm-version"),
	.folders = LIST("elm-stuff"),
	.cmd = LIST("elm", "--version"),
	.symbol = "🌳 ",
};

static const lang_spec spec_julia = {
	.extensions = LIST("jl"),
	.files = LIST("Project.toml", "Manifest.toml"),
	.cmd = LIST("julia", "--version"),
	.symbol = "ஃ ",
};

static const lang_spec spec_maven = {
	.files = LIST("pom.xml", ".mvn"),
	.cmd = LIST("mvn", "--version"),
	.symbol = "🅼 ",
	.version_line = "Apache Maven",
};

static const lang_spec spec_nim = {
	.extensions = LIST("nim", "nims", "nimble"),
	.files = LIST("nim.cfg"),
	.cmd = LIST("nim", "--version"),
	.symbol = "👑 ",
	.version_line = "Nim Compiler",
};

static const lang_spec spec_scala = {
	.extensions = LIST("scala", "sbt"),
	.files = LIST(".scalaenv", ".sbtenv", "build.sbt"),
	.cmd = LIST("scalac", "-version"),
	.symbol = "🆂 ",
};

/* Gradle's own --version takes seconds to start a JVM, so read the version the
 * wrapper pins instead. */
static bool probe_gradle(module_state *st)
{
	context *ctx = st->ctx;
	if (!ctx_has_file(ctx, "build.gradle") && !ctx_has_file(ctx, "build.gradle.kts") &&
	    !ctx_has_file(ctx, "settings.gradle") && !ctx_has_file(ctx, "settings.gradle.kts") &&
	    !ctx_has_file(ctx, "gradle"))
		return false;

	var_addz(&st->vars, "symbol", toml_str_at(st->cfg, "symbol", "🅶 "));

	if (!toml_bool_at(st->cfg, "show_version", true))
		return true;

	str path = {0};
	str_addf(&path, "%s/gradle/wrapper/gradle-wrapper.properties", ctx->cwd);
	char *props = read_file(path.data, NULL);
	str_free(&path);
	if (!props)
		return true;

	char *marker = strstr(props, "gradle-");
	char *raw = marker ? extract_version(marker) : NULL;
	free(props);
	if (!raw)
		return true;

	const char *fmt = toml_str_at(st->cfg, "version_format", "v${raw}");
	var_add(&st->vars, "version", apply_version_format(fmt, raw));
	free(raw);
	return true;
}

static bool probe_pixi(module_state *st)
{
	const char *env = getenv("PIXI_ENVIRONMENT_NAME");
	if (!env && !ctx_has_file(st->ctx, "pixi.toml"))
		return false;

	var_addz(&st->vars, "symbol", toml_str_at(st->cfg, "symbol", "🧚 "));
	var_addz(&st->vars, "environment", env ? env : "");

	if (toml_bool_at(st->cfg, "show_version", true)) {
		char *out = capture(st->ctx->cwd, (char *const[]){(char *)"pixi", (char *)"--version",
		                                                  NULL},
		                    (int)toml_int_at(st->cfg, "command_timeout", 500));
		if (out) {
			char *raw = extract_version(out);
			free(out);
			if (raw) {
				const char *fmt =
					toml_str_at(st->cfg, "version_format", "v${raw}");
				var_add(&st->vars, "version", apply_version_format(fmt, raw));
				free(raw);
			}
		}
	}
	return true;
}

#define LANG(mod, spec, default_style)                                                       \
	{                                                                                    \
		.name = mod, .format = "via [$symbol($version )]($style)",                    \
		.style = default_style, .data = &spec, .probe = probe_lang                    \
	}

const module_def lang_modules[] = {
	LANG("bun", spec_bun, "bold red"),
	LANG("c", spec_c, "149 bold"),
	LANG("cpp", spec_cpp, "149 bold"),
	LANG("elixir", spec_elixir, "bold purple"),
	LANG("elm", spec_elm, "cyan bold"),
	LANG("golang", spec_golang, "bold cyan"),
	LANG("haskell", spec_haskell, "bold purple"),
	LANG("java", spec_java, "red dimmed"),
	LANG("julia", spec_julia, "bold purple"),
	LANG("kotlin", spec_kotlin, "bold blue"),
	LANG("maven", spec_maven, "red dimmed"),
	LANG("nim", spec_nim, "yellow bold"),
	LANG("nodejs", spec_nodejs, "bold green"),
	LANG("php", spec_php, "147 bold"),
	LANG("python", spec_python, "yellow bold"),
	LANG("rust", spec_rust, "bold red"),
	LANG("scala", spec_scala, "red dimmed"),
	{.name = "gradle", .format = "via [$symbol($version )]($style)", .style = "bold bright-cyan",
	 .probe = probe_gradle},
	{.name = "pixi", .format = "via [$symbol($version )(\\($environment\\) )]($style)",
	 .style = "yellow bold", .probe = probe_pixi},
	{.name = "conda", .format = "via [$symbol$environment]($style) ", .style = "green bold",
	 .probe = probe_conda},
	{.name = "docker_context", .format = "via [$symbol$context]($style) ",
	 .style = "blue bold", .probe = probe_docker_context},
};

const size_t lang_modules_count = sizeof lang_modules / sizeof *lang_modules;
