#include "module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool probe_git_branch(module_state *st)
{
	const char *git_dir = ctx_git_dir(st->ctx);
	if (!git_dir)
		return false;

	str head_path = {0};
	str_addf(&head_path, "%s/HEAD", git_dir);
	char *head = read_file(head_path.data, NULL);
	str_free(&head_path);
	if (!head)
		return false;

	char *line = trim(head);
	char *branch;

	if (has_prefix(line, "ref: ")) {
		const char *ref = line + 5;
		const char *slash = strrchr(ref, '/');
		branch = xstrdup(slash ? slash + 1 : ref);
	} else {
		/* Detached HEAD: show the abbreviated commit instead. */
		size_t len = strlen(line);
		branch = xstrndup(line, len > 7 ? 7 : len);
	}
	free(head);

	long long truncation_length = toml_int_at(st->cfg, "truncation_length", 0);
	if (truncation_length > 0 && (long long)strlen(branch) > truncation_length) {
		const char *symbol = toml_str_at(st->cfg, "truncation_symbol", "…");
		str s = {0};
		str_addn(&s, branch, (size_t)truncation_length);
		str_add(&s, symbol);
		free(branch);
		branch = str_take(&s);
	}

	var_addz(&st->vars, "symbol", toml_str_at(st->cfg, "symbol", "\ue0a0 "));
	var_add(&st->vars, "branch", branch);
	return true;
}

typedef struct {
	int conflicted, staged, deleted, renamed, modified, untracked, stashed;
	long long ahead, behind;
	bool has_upstream;
} git_counts;

static void parse_porcelain(const char *out, git_counts *c)
{
	for (const char *line = out; line && *line;) {
		const char *eol = strchr(line, '\n');
		size_t len = eol ? (size_t)(eol - line) : strlen(line);

		if (len >= 2 && line[0] == '#') {
			if (strncmp(line, "# branch.ab ", 12) == 0) {
				c->has_upstream = true;
				sscanf(line + 12, "%lld %lld", &c->ahead, &c->behind);
				if (c->behind < 0)
					c->behind = -c->behind;
			}
		} else if (len >= 4 && (line[0] == '1' || line[0] == '2') && line[1] == ' ') {
			char x = line[2], y = line[3];
			if (x == 'R' || x == 'C')
				c->renamed++;
			else if (x == 'D')
				c->deleted++;
			else if (x != '.')
				c->staged++;

			if (y == 'D')
				c->deleted++;
			else if (y != '.')
				c->modified++;
		} else if (len >= 1 && line[0] == 'u') {
			c->conflicted++;
		} else if (len >= 1 && line[0] == '?') {
			c->untracked++;
		}

		line = eol ? eol + 1 : NULL;
	}
}

static int count_stashes(const char *git_dir)
{
	str path = {0};
	str_addf(&path, "%s/logs/refs/stash", git_dir);
	char *content = read_file(path.data, NULL);
	str_free(&path);
	if (!content)
		return 0;

	int n = 0;
	for (const char *p = content; *p; p++)
		if (*p == '\n')
			n++;
	free(content);
	return n;
}

/* Renders a status symbol template such as "!${count}" for a given count, or
 * NULL when the count is zero so that ( ) groups around it disappear. */
static char *render_status(module_state *st, const char *key, const char *dflt, long long count)
{
	if (count <= 0)
		return NULL;

	const char *tmpl = toml_str_at(st->cfg, key, dflt);
	char num[32];
	snprintf(num, sizeof num, "%lld", count);

	str out = {0};
	for (const char *p = tmpl; *p;) {
		if (has_prefix(p, "${count}")) {
			str_add(&out, num);
			p += 8;
		} else if (has_prefix(p, "$count")) {
			str_add(&out, num);
			p += 6;
		} else {
			str_addc(&out, *p++);
		}
	}
	return str_take(&out);
}

static bool probe_git_status(module_state *st)
{
	const char *root = ctx_git_root(st->ctx);
	const char *git_dir = ctx_git_dir(st->ctx);
	if (!root || !git_dir)
		return false;

	char *argv[8];
	int argc = 0;
	argv[argc++] = (char *)"git";
	argv[argc++] = (char *)"status";
	argv[argc++] = (char *)"--porcelain=2";
	argv[argc++] = (char *)"--branch";
	if (toml_bool_at(st->cfg, "ignore_submodules", false))
		argv[argc++] = (char *)"--ignore-submodules=all";
	argv[argc] = NULL;

	long long timeout = toml_int_at(st->cfg, "command_timeout", 500);
	char *out = capture(root, argv, (int)timeout);
	if (!out)
		return false;

	git_counts c = {0};
	parse_porcelain(out, &c);
	free(out);
	c.stashed = count_stashes(git_dir);

	/* Each variable holds its rendered symbol, not a number, and stays unset
	 * at zero so that ( ) groups around it disappear. $all_status is simply
	 * their concatenation, in starship's order. */
	const struct {
		const char *name;
		const char *dflt;
		long long count;
	} parts[] = {
		{"conflicted", "=", c.conflicted}, {"stashed", "$", c.stashed},
		{"deleted", "✘", c.deleted},       {"renamed", "»", c.renamed},
		{"modified", "!", c.modified},     {"staged", "+", c.staged},
		{"untracked", "?", c.untracked},
	};

	str all = {0};
	for (size_t i = 0; i < sizeof parts / sizeof *parts; i++) {
		char *rendered = render_status(st, parts[i].name, parts[i].dflt, parts[i].count);
		str_add(&all, rendered);
		var_add(&st->vars, parts[i].name, rendered);
	}

	char *ahead = render_status(st, "ahead", "⇡", c.ahead);
	char *behind = render_status(st, "behind", "⇣", c.behind);
	char *ab = NULL;

	if (c.ahead > 0 && c.behind > 0)
		ab = render_status(st, "diverged", "⇕", 1);
	else if (ahead)
		ab = xstrdup(ahead);
	else if (behind)
		ab = xstrdup(behind);
	else if (c.has_upstream)
		ab = render_status(st, "up_to_date", "", 1);

	var_add(&st->vars, "ahead", ahead);
	var_add(&st->vars, "behind", behind);

	bool any = all.len > 0 || (ab && *ab);

	var_add(&st->vars, "all_status", str_take(&all));
	var_add(&st->vars, "ahead_behind", ab);

	return any;
}

static char *git_dir_line(const char *git_dir, const char *relative)
{
	str path = {0};
	str_addf(&path, "%s/%s", git_dir, relative);
	char *content = read_file(path.data, NULL);
	str_free(&path);
	if (!content)
		return NULL;

	char *line = trim(content);
	char *eol = strchr(line, '\n');
	if (eol)
		*eol = '\0';

	char *result = xstrdup(line);
	free(content);
	return result;
}

/* Reads the marker files git leaves behind mid-operation rather than shelling
 * out; each one names the state and, where applicable, its progress. */
static bool probe_git_state(module_state *st)
{
	const char *git_dir = ctx_git_dir(st->ctx);
	if (!git_dir)
		return false;

	str dir = {0};
	const char *state = NULL;
	char *current = NULL, *total = NULL;

	str_addf(&dir, "%s/rebase-merge", git_dir);
	bool rebase_merge = file_exists(dir.data);
	str_clear(&dir);
	str_addf(&dir, "%s/rebase-apply", git_dir);
	bool rebase_apply = file_exists(dir.data);
	str_free(&dir);

	if (rebase_merge || rebase_apply) {
		const char *sub = rebase_merge ? "rebase-merge" : "rebase-apply";
		state = toml_str_at(st->cfg, "rebase", "REBASING");

		str rel = {0};
		str_addf(&rel, "%s/msgnum", sub);
		current = git_dir_line(git_dir, rel.data);
		str_clear(&rel);
		str_addf(&rel, "%s/end", sub);
		total = git_dir_line(git_dir, rel.data);
		str_free(&rel);
	} else if (dir_has_entry(git_dir, "MERGE_HEAD")) {
		state = toml_str_at(st->cfg, "merge", "MERGING");
	} else if (dir_has_entry(git_dir, "CHERRY_PICK_HEAD")) {
		state = toml_str_at(st->cfg, "cherry_pick", "CHERRY-PICKING");
	} else if (dir_has_entry(git_dir, "REVERT_HEAD")) {
		state = toml_str_at(st->cfg, "revert", "REVERTING");
	} else if (dir_has_entry(git_dir, "BISECT_LOG")) {
		state = toml_str_at(st->cfg, "bisect", "BISECTING");
	}

	if (!state) {
		free(current);
		free(total);
		return false;
	}

	var_addz(&st->vars, "state", state);
	var_add(&st->vars, "progress_current", current);
	var_add(&st->vars, "progress_total", total);
	return true;
}

static bool probe_git_metrics(module_state *st)
{
	const char *root = ctx_git_root(st->ctx);
	if (!root)
		return false;

	char *argv[8];
	int argc = 0;
	argv[argc++] = (char *)"git";
	argv[argc++] = (char *)"diff";
	argv[argc++] = (char *)"--shortstat";
	if (toml_bool_at(st->cfg, "ignore_submodules", false))
		argv[argc++] = (char *)"--ignore-submodules=all";
	argv[argc] = NULL;

	char *out = capture(root, argv, (int)toml_int_at(st->cfg, "command_timeout", 500));
	if (!out)
		return false;

	/* "N files changed, A insertions(+), D deletions(-)" — either half of the
	 * pair is omitted when it is zero. */
	long long added = 0, deleted = 0;
	const char *ins = strstr(out, " insertion");
	const char *del = strstr(out, " deletion");

	if (ins) {
		const char *p = ins;
		while (p > out && (p[-1] == ' ' || (p[-1] >= '0' && p[-1] <= '9')))
			p--;
		added = atoll(p);
	}
	if (del) {
		const char *p = del;
		while (p > out && (p[-1] == ' ' || (p[-1] >= '0' && p[-1] <= '9')))
			p--;
		deleted = atoll(p);
	}
	free(out);

	bool only_nonzero = toml_bool_at(st->cfg, "only_nonzero_diffs", true);
	if (only_nonzero && added == 0 && deleted == 0)
		return false;

	if (!only_nonzero || added > 0)
		var_addf(&st->vars, "added", "%lld", added);
	if (!only_nonzero || deleted > 0)
		var_addf(&st->vars, "deleted", "%lld", deleted);

	var_addz(&st->vars, "added_style", toml_str_at(st->cfg, "added_style", "bold green"));
	var_addz(&st->vars, "deleted_style", toml_str_at(st->cfg, "deleted_style", "bold red"));
	return true;
}

const module_def git_modules[] = {
	{.name = "git_branch", .format = "on [$symbol$branch(:$remote_branch)]($style) ",
	 .style = "bold purple", .probe = probe_git_branch},
	{.name = "git_status", .format = "([\\[$all_status$ahead_behind\\]]($style) )",
	 .style = "red bold", .probe = probe_git_status},
	{.name = "git_state", .format = "\\([$state( $progress_current/$progress_total)]($style)\\) ",
	 .style = "bold yellow", .probe = probe_git_state},
	{.name = "git_metrics",
	 .format = "([+$added]($added_style) )([-$deleted]($deleted_style) )", .style = "",
	 .disabled = true, .probe = probe_git_metrics},
};

const size_t git_modules_count = sizeof git_modules / sizeof *git_modules;
