#include "toml.h"
#include "util.h"
#include <stdio.h>
#include <string.h>

static int fails;

static void check(const char *what, const char *got, const char *want)
{
	if (!got || strcmp(got, want) != 0) {
		printf("FAIL %s:\n  got  [%s]\n  want [%s]\n", what, got ? got : "(null)", want);
		fails++;
	} else {
		printf("ok   %s\n", what);
	}
}

int main(void)
{
	static const char doc[] =
	    "\"$schema\" = 'https://starship.rs/config-schema.json'\n"
	    "\n"
	    "format = \"\"\"\n"
	    "[](surface0)\\\n"
	    "$username\\\n"
	    "[](bg:peach fg:surface0)\\\n"
	    "$directory\\\n"
	    "$line_break$character\"\"\"\n"
	    "\n"
	    "palette = 'catppuccin_mocha'\n"
	    "add_newline = false\n"
	    "scan_timeout = 30\n"
	    "\n"
	    "[palettes.catppuccin_mocha]\n"
	    "peach = \"#fab387\"\n"
	    "surface0 = \"#313244\"\n"
	    "\n"
	    "[directory]\n"
	    "style = \"fg:mantle bg:peach\"\n"
	    "truncation_length = 3\n"
	    "\n"
	    "[directory.substitutions]\n"
	    "\"Documents\" = \"\\U000F0219 \"\n"
	    "\n"
	    "[character]\n"
	    "success_symbol = '[\\$](bold green)'\n"
	    "\n"
	    "[[battery.display]]\n"
	    "threshold = 10\n"
	    "style = 'red bold'\n"
	    "\n"
	    "[[battery.display]]\n"
	    "threshold = 30\n"
	    "\n"
	    "[git_status]\n"
	    "ignore_submodules = true\n"
	    "inline = { a = 1, b.c = 'deep' }\n"
	    "list = [1, 2, 3,]\n";

	char err[256] = {0};
	toml_doc *tdoc = toml_parse(doc, err, sizeof err);
	if (!tdoc) {
		printf("PARSE ERROR: %s\n", err);
		return 1;
	}

	const toml_value *root = toml_root(tdoc);

	check("schema", toml_str_at(root, "$schema", NULL),
	      "https://starship.rs/config-schema.json");
	check("format line-continuation",
	      toml_str_at(root, "format", NULL),
	      "[](surface0)$username[](bg:peach fg:surface0)$directory$line_break$character");
	check("palette", toml_str_at(root, "palette", NULL), "catppuccin_mocha");
	check("nested palette entry",
	      toml_str(toml_path(root, "palettes.catppuccin_mocha.peach"), NULL), "#fab387");
	check("directory style", toml_str(toml_path(root, "directory.style"), NULL),
	      "fg:mantle bg:peach");
	check("quoted key + \\U escape",
	      toml_str(toml_path(root, "directory.substitutions.Documents"), NULL),
	      "\xf3\xb0\x88\x99 ");
	check("literal string keeps backslash",
	      toml_str(toml_path(root, "character.success_symbol"), NULL), "[\\$](bold green)");
	check("inline table dotted key",
	      toml_str(toml_path(root, "git_status.inline.b.c"), NULL), "deep");

	if (toml_int(toml_path(root, "directory.truncation_length"), -1) != 3) {
		printf("FAIL truncation_length\n");
		fails++;
	} else {
		printf("ok   truncation_length\n");
	}

	if (toml_bool(toml_path(root, "add_newline"), true) != false ||
	    toml_bool(toml_path(root, "git_status.ignore_submodules"), false) != true) {
		printf("FAIL booleans\n");
		fails++;
	} else {
		printf("ok   booleans\n");
	}

	const toml_value *disp = toml_path(root, "battery.display");
	if (!disp || disp->type != TOML_ARRAY || disp->arr.n != 2 ||
	    toml_int_at(disp->arr.items[1], "threshold", -1) != 30) {
		printf("FAIL array of tables\n");
		fails++;
	} else {
		printf("ok   array of tables\n");
	}

	const toml_value *list = toml_path(root, "git_status.list");
	if (!list || list->type != TOML_ARRAY || list->arr.n != 3) {
		printf("FAIL array trailing comma\n");
		fails++;
	} else {
		printf("ok   array trailing comma\n");
	}

	toml_doc_free(tdoc);

	if (toml_parse("key = [1, 2", err, sizeof err) != NULL) {
		printf("FAIL error detection\n");
		fails++;
	} else {
		printf("ok   error detection (%s)\n", err);
	}

	printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
	return fails != 0;
}
