#ifndef CARSHIP_CONTEXT_H
#define CARSHIP_CONTEXT_H

#include "style.h"
#include "util.h"

typedef struct {
	char *cwd;  /* logical, preserving symlinks the shell walked through */
	char *home;
	char *shell;
	const esc_wrap *wrap;

	int status;
	char *pipestatus;
	long long duration_ms;
	int jobs;
	char *keymap;
	int width;

	bool git_probed;
	char *git_root; /* worktree root, NULL outside a repository */
	char *git_dir;  /* the .git directory itself */

	bool entries_loaded;
	char **entries; /* names in cwd, read once and shared by all modules */
	size_t entries_n;
} context;

void ctx_init(context *c);
void ctx_free(context *c);
void ctx_set_shell(context *c, const char *shell);

const char *ctx_git_root(context *c);
const char *ctx_git_dir(context *c);

/* Directory listing of the working directory, loaded on first use. */
char *const *ctx_entries(context *c, size_t *n);
bool ctx_has_file(context *c, const char *name);
bool ctx_has_extension(context *c, const char *ext);

#endif
