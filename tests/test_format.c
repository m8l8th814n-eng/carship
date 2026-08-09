#include "format.h"
#include "style.h"
#include "toml.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

static int fails;

static const struct {
	const char *name;
	const char *value;
} vars[] = {
	{"a", "A"},
	{"branch", "main"},
	{"empty", ""},
	{"style", "bold green"},
	{"symbol", " "},
};

static bool resolve(void *user, const char *name, str *out)
{
	(void)user;
	for (size_t i = 0; i < sizeof vars / sizeof *vars; i++) {
		if (strcmp(vars[i].name, name) == 0) {
			str_add(out, vars[i].value);
			return vars[i].value[0] != '\0';
		}
	}
	return false;
}

static toml_doc *palette_doc;

static void check(const char *what, const char *fmt, const char *want, const esc_wrap *wrap)
{
	fnode *n = fmt_parse(fmt);
	if (!n && want) {
		printf("FAIL %s: parse error\n", what);
		fails++;
		return;
	}

	render_ctx ctx = {
		.palette = toml_path(toml_root(palette_doc), "palettes.p"),
		.wrap = wrap,
		.resolve = resolve,
	};

	str out = {0};
	fmt_render(n, &ctx, &out);
	const char *got = out.data ? out.data : "";

	if (strcmp(got, want) != 0) {
		printf("FAIL %s\n  fmt  [%s]\n  got  [%s]\n  want [%s]\n", what, fmt, got, want);
		fails++;
	} else {
		printf("ok   %s\n", what);
	}

	str_free(&out);
	fmt_free(n);
}

int main(void)
{
	char err[256];
	palette_doc = toml_parse("[palettes.p]\npeach = \"#fab387\"\nmauve = \"#cba6f7\"\n"
	                         "alias = \"peach\"\n",
	                         err, sizeof err);
	if (!palette_doc) {
		printf("palette parse failed: %s\n", err);
		return 1;
	}

	check("literal text", "hello", "hello", &esc_wrap_none);
	check("variable", "x$a y", "xA y", &esc_wrap_none);
	check("braced variable", "${a}bc", "Abc", &esc_wrap_none);
	check("escaped dollar", "\\$a", "$a", &esc_wrap_none);
	check("unknown variable drops", "x${nope}y", "xy", &esc_wrap_none);

	check("optional kept", "($a)", "A", &esc_wrap_none);
	check("optional dropped when empty", "x($empty)y", "xy", &esc_wrap_none);
	check("optional dropped when unknown", "x($nope)y", "xy", &esc_wrap_none);
	check("optional without vars kept", "x(lit)y", "xlity", &esc_wrap_none);
	check("optional mixed keeps all", "($a-$empty)", "A-", &esc_wrap_none);

	check("named style", "[$a](bold red)", "\x1b[1;31mA\x1b[0m", &esc_wrap_none);
	check("truecolor from palette", "[hi](fg:peach)", "\x1b[38;2;250;179;135mhi\x1b[0m",
	      &esc_wrap_none);
	check("palette alias", "[hi](fg:alias)", "\x1b[38;2;250;179;135mhi\x1b[0m", &esc_wrap_none);
	check("powerline separator", "[\xee\x82\xb0](fg:peach bg:mauve)",
	      "\x1b[38;2;250;179;135;48;2;203;166;247m\xee\x82\xb0\x1b[0m", &esc_wrap_none);
	check("style variable", "[x]($style)", "\x1b[1;32mx\x1b[0m", &esc_wrap_none);
	check("ansi 256", "[x](fg:213)", "\x1b[38;5;213mx\x1b[0m", &esc_wrap_none);
	check("bright color", "[x](bright-blue)", "\x1b[94mx\x1b[0m", &esc_wrap_none);
	check("empty group emits nothing", "[$empty](bold red)", "", &esc_wrap_none);

	check("nested group inherits bg", "[a[b](fg:peach)](bg:mauve)",
	      "\x1b[48;2;203;166;247ma\x1b[38;2;250;179;135;48;2;203;166;247mb\x1b[0m",
	      &esc_wrap_none);
	check("style re-asserted after nested reset", "[a[b](fg:peach)c](bg:mauve)",
	      "\x1b[48;2;203;166;247ma\x1b[38;2;250;179;135;48;2;203;166;247mb\x1b[0m"
	      "\x1b[48;2;203;166;247mc\x1b[0m",
	      &esc_wrap_none);

	check("bash wrapping", "[$a](red)", "\001\x1b[31m\002A\001\x1b[0m\002", &esc_wrap_bash);
	check("zsh wrapping", "[$a](red)", "%{\x1b[31m%}A%{\x1b[0m%}", &esc_wrap_zsh);

	check("module format shape", "[$symbol$branch ]($style)", "\x1b[1;32m main \x1b[0m",
	      &esc_wrap_none);

	if (fmt_parse("[unclosed") || fmt_parse("[a]") || fmt_parse("[a](red")) {
		printf("FAIL syntax errors not detected\n");
		fails++;
	} else {
		printf("ok   syntax errors detected\n");
	}

	toml_doc_free(palette_doc);
	printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
	return fails != 0;
}
