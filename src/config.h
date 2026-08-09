#ifndef CARSHIP_CONFIG_H
#define CARSHIP_CONFIG_H

#include "toml.h"

#define CARSHIP_DEFAULT_FORMAT                                                                \
	"$os$username$hostname$localip$shlvl$container$nix_shell$directory$git_branch"        \
	"$git_state$git_status$git_metrics$bun$c$cpp$elixir$elm$golang$gradle$haskell$java"   \
	"$julia$kotlin$maven$nim$nodejs$php$python$rust$scala$conda$pixi$docker_context"      \
	"$env_var$sudo$jobs$cmd_duration$status$time$shell$line_break$character"

typedef struct {
	char *path;
	toml_doc *doc;
	const toml_value *root;
	const toml_value *palette;
} config;

char *config_default_path(void);
/* Always yields a usable config; a missing file simply means all defaults. */
bool config_load(config *c, char *err, size_t errlen);
void config_free(config *c);

#endif
