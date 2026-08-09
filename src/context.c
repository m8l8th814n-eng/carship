#include "context.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

/* Prefers $PWD when it names the same directory as getcwd(), so that paths the
 * user reached through a symlink stay symbolic. */
static char *logical_cwd(void)
{
	char buf[4096];
	const char *pwd = getenv("PWD");

	if (pwd && pwd[0] == '/') {
		struct stat a, b;
		if (stat(pwd, &a) == 0 && stat(".", &b) == 0 && a.st_dev == b.st_dev &&
		    a.st_ino == b.st_ino)
			return xstrdup(pwd);
	}

	if (getcwd(buf, sizeof buf))
		return xstrdup(buf);
	return xstrdup("");
}

static int terminal_width(void)
{
	const char *cols = getenv("COLUMNS");
	if (cols && *cols) {
		int n = atoi(cols);
		if (n > 0)
			return n;
	}

	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
		return ws.ws_col;
	return 80;
}

void ctx_init(context *c)
{
	memset(c, 0, sizeof *c);
	c->cwd = logical_cwd();

	const char *home = getenv("HOME");
	c->home = xstrdup(home ? home : "");
	c->shell = xstrdup("");
	c->keymap = xstrdup("");
	c->wrap = &esc_wrap_none;
	c->width = terminal_width();
	c->jobs = 0;
	c->status = 0;
	c->duration_ms = -1;
}

void ctx_free(context *c)
{
	free(c->cwd);
	free(c->home);
	free(c->shell);
	free(c->keymap);
	free(c->pipestatus);
	free(c->git_root);
	free(c->git_dir);
	for (size_t i = 0; i < c->entries_n; i++)
		free(c->entries[i]);
	free(c->entries);
	memset(c, 0, sizeof *c);
}

void ctx_set_shell(context *c, const char *shell)
{
	free(c->shell);
	c->shell = xstrdup(shell ? shell : "");

	if (str_eq(c->shell, "bash"))
		c->wrap = &esc_wrap_bash;
	else if (str_eq(c->shell, "zsh"))
		c->wrap = &esc_wrap_zsh;
	else
		c->wrap = &esc_wrap_none;
}

/* Resolves the "gitdir: <path>" indirection used by worktrees and submodules. */
static char *read_gitdir_file(const char *path, const char *parent)
{
	size_t len = 0;
	char *content = read_file(path, &len);
	if (!content)
		return NULL;

	char *p = trim(content);
	char *result = NULL;

	if (has_prefix(p, "gitdir:")) {
		p = trim(p + 7);
		if (p[0] == '/') {
			result = xstrdup(p);
		} else {
			str s = {0};
			str_addf(&s, "%s/%s", parent, p);
			result = str_take(&s);
		}
	}

	free(content);
	return result;
}

static void probe_git(context *c)
{
	c->git_probed = true;

	const char *ceiling = getenv("GIT_CEILING_DIRECTORIES");
	str path = {0};
	str_add(&path, c->cwd);

	while (path.len > 0) {
		if (ceiling && *ceiling && strcmp(path.data, ceiling) == 0)
			break;

		str candidate = {0};
		str_addf(&candidate, "%s/.git", path.data);

		struct stat st;
		if (stat(candidate.data, &st) == 0) {
			c->git_root = xstrdup(path.data);
			if (S_ISDIR(st.st_mode))
				c->git_dir = str_take(&candidate);
			else
				c->git_dir = read_gitdir_file(candidate.data, path.data);
			str_free(&candidate);
			break;
		}
		str_free(&candidate);

		char *slash = strrchr(path.data, '/');
		if (!slash || slash == path.data)
			break;
		*slash = '\0';
		path.len = (size_t)(slash - path.data);
	}

	str_free(&path);
}

const char *ctx_git_root(context *c)
{
	if (!c->git_probed)
		probe_git(c);
	return c->git_root;
}

const char *ctx_git_dir(context *c)
{
	if (!c->git_probed)
		probe_git(c);
	return c->git_dir;
}

char *const *ctx_entries(context *c, size_t *n)
{
	if (!c->entries_loaded) {
		c->entries_loaded = true;

		DIR *d = opendir(c->cwd[0] ? c->cwd : ".");
		if (d) {
			size_t cap = 0;
			const struct dirent *e;
			while ((e = readdir(d))) {
				if (e->d_name[0] == '.' &&
				    (e->d_name[1] == '\0' ||
				     (e->d_name[1] == '.' && e->d_name[2] == '\0')))
					continue;
				if (c->entries_n == cap) {
					cap = cap ? cap * 2 : 64;
					c->entries = xrealloc(c->entries, cap * sizeof *c->entries);
				}
				c->entries[c->entries_n++] = xstrdup(e->d_name);
			}
			closedir(d);
		}
	}

	if (n)
		*n = c->entries_n;
	return c->entries;
}

bool ctx_has_file(context *c, const char *name)
{
	size_t n;
	char *const *entries = ctx_entries(c, &n);
	for (size_t i = 0; i < n; i++)
		if (strcmp(entries[i], name) == 0)
			return true;
	return false;
}

bool ctx_has_extension(context *c, const char *ext)
{
	size_t n;
	char *const *entries = ctx_entries(c, &n);
	size_t elen = strlen(ext);

	for (size_t i = 0; i < n; i++) {
		size_t len = strlen(entries[i]);
		if (len > elen + 1 && entries[i][len - elen - 1] == '.' &&
		    strcmp(entries[i] + len - elen, ext) == 0)
			return true;
	}
	return false;
}
