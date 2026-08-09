#include "config.h"

#include "util.h"

#include <stdlib.h>
#include <string.h>

char *config_default_path(void)
{
	const char *explicit_path = getenv("CARSHIP_CONFIG");
	if (explicit_path && *explicit_path)
		return xstrdup(explicit_path);

	str p = {0};
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && *xdg) {
		str_addf(&p, "%s/carship.toml", xdg);
		return str_take(&p);
	}

	const char *home = getenv("HOME");
	str_addf(&p, "%s/.config/carship.toml", home ? home : ".");
	return str_take(&p);
}

bool config_load(config *c, char *err, size_t errlen)
{
	memset(c, 0, sizeof *c);
	c->path = config_default_path();

	char *text = read_file(c->path, NULL);
	c->doc = toml_parse(text ? text : "", err, errlen);
	free(text);

	if (!c->doc) {
		c->doc = toml_parse("", NULL, 0);
		c->root = toml_root(c->doc);
		return false;
	}

	c->root = toml_root(c->doc);

	const char *name = toml_str_at(c->root, "palette", NULL);
	if (name) {
		const toml_value *palettes = toml_get(c->root, "palettes");
		c->palette = toml_get(palettes, name);
	}
	return true;
}

void config_free(config *c)
{
	toml_doc_free(c->doc);
	free(c->path);
	memset(c, 0, sizeof *c);
}
