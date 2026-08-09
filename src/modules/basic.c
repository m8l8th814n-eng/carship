#include "module.h"

#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static bool over_ssh(void)
{
	return getenv("SSH_CONNECTION") || getenv("SSH_CLIENT") || getenv("SSH_TTY");
}

/* Symbols are written as escapes so the source stays pure ASCII; they are
 * Nerd Font code points and only serve as defaults for [os.symbols]. */
static const struct {
	const char *id;
	const char *name;
	const char *symbol;
} os_table[] = {
	{"almalinux", "Alma", "\U000f111b"},
	{"alpine", "Alpine", ""},
	{"arch", "Arch", "\U000f08c7"},
	{"artix", "Artix", "\U000f08c7"},
	{"cachyos", "CachyOS", "\U000f08c7"},
	{"centos", "CentOS", ""},
	{"debian", "Debian", "\U000f08da"},
	{"endeavouros", "EndeavourOS", ""},
	{"fedora", "Fedora", "\U000f08db"},
	{"garuda", "Garuda", "\U000f06d3"},
	{"gentoo", "Gentoo", "\U000f08e8"},
	{"kali", "Kali", ""},
	{"linuxmint", "Mint", "\U000f08ed"},
	{"manjaro", "Manjaro", ""},
	{"nixos", "NixOS", ""},
	{"opensuse", "openSUSE", ""},
	{"opensuse-leap", "openSUSE", ""},
	{"opensuse-tumbleweed", "openSUSE", ""},
	{"pop", "Pop", ""},
	{"raspbian", "Raspbian", "\U000f043f"},
	{"rhel", "Redhat", "\U000f111b"},
	{"rocky", "Rocky", ""},
	{"solus", "Solus", "\U000f0833"},
	{"ubuntu", "Ubuntu", "\U000f0548"},
	{"void", "Void", ""},
};

#define OS_GENERIC_SYMBOL "\U000f033d"

static void read_os_release_key(const char *content, const char *key, char *out, size_t outlen)
{
	size_t keylen = strlen(key);
	out[0] = '\0';

	for (const char *line = content; line && *line;) {
		const char *eol = strchr(line, '\n');
		size_t linelen = eol ? (size_t)(eol - line) : strlen(line);

		if (linelen > keylen && strncmp(line, key, keylen) == 0 && line[keylen] == '=') {
			const char *v = line + keylen + 1;
			size_t len = linelen - keylen - 1;
			if (len >= 2 && (v[0] == '"' || v[0] == '\'')) {
				len -= 2;
				v++;
			}
			if (len >= outlen)
				len = outlen - 1;
			memcpy(out, v, len);
			out[len] = '\0';
			return;
		}
		line = eol ? eol + 1 : NULL;
	}
}

static bool lookup_os(const char *id, char **name, const char **symbol)
{
	if (!id[0])
		return false;

	for (size_t i = 0; i < sizeof os_table / sizeof *os_table; i++) {
		if (strcmp(os_table[i].id, id) == 0) {
			*name = xstrdup(os_table[i].name);
			*symbol = os_table[i].symbol;
			return true;
		}
	}
	return false;
}

/* Maps /etc/os-release onto starship's OS names, which is what [os.symbols]
 * keys are written against. Unknown distributions fall back to ID_LIKE so
 * derivatives still get their parent's icon. */
static void detect_os(char **name, const char **symbol)
{
	*name = NULL;
	*symbol = OS_GENERIC_SYMBOL;

	char *content = read_file("/etc/os-release", NULL);
	if (!content) {
		*name = xstrdup("Linux");
		return;
	}

	char id[64], id_like[128];
	read_os_release_key(content, "ID", id, sizeof id);
	read_os_release_key(content, "ID_LIKE", id_like, sizeof id_like);
	free(content);

	if (lookup_os(id, name, symbol))
		return;

	for (char *tok = strtok(id_like, " "); tok; tok = strtok(NULL, " ")) {
		const char *parent_symbol;
		char *parent_name = NULL;
		if (lookup_os(tok, &parent_name, &parent_symbol)) {
			free(parent_name);
			*name = xstrdup(id[0] ? id : "Linux");
			*symbol = parent_symbol;
			return;
		}
	}

	*name = xstrdup(id[0] ? id : "Linux");
}

static bool probe_os(module_state *st)
{
	char *name = NULL;
	const char *symbol = "";
	detect_os(&name, &symbol);

	const toml_value *symbols = toml_get(st->cfg, "symbols");
	const char *override = toml_str_at(symbols, name, NULL);

	var_addz(&st->vars, "symbol", override ? override : symbol);
	var_add(&st->vars, "name", name);
	var_addz(&st->vars, "type", name);
	return true;
}

static bool probe_username(module_state *st)
{
	uid_t uid = geteuid();
	bool root = (uid == 0);
	bool show = root || over_ssh() || toml_bool_at(st->cfg, "show_always", false);
	if (!show)
		return false;

	const char *user = getenv("USER");
	if (!user || !*user) {
		const struct passwd *pw = getpwuid(uid);
		user = pw ? pw->pw_name : "?";
	}

	if (root)
		var_addz(&st->vars, "style", toml_str_at(st->cfg, "style_root", "bold red"));
	else
		var_addz(&st->vars, "style", toml_str_at(st->cfg, "style_user", "bold yellow"));

	var_addz(&st->vars, "user", user);
	return true;
}

static bool probe_hostname(module_state *st)
{
	bool ssh = over_ssh();
	if (toml_bool_at(st->cfg, "ssh_only", true) && !ssh)
		return false;

	char host[256];
	if (gethostname(host, sizeof host) != 0)
		return false;
	host[sizeof host - 1] = '\0';

	const char *trim_at = toml_str_at(st->cfg, "trim_at", ".");
	if (trim_at && *trim_at) {
		char *cut = strstr(host, trim_at);
		if (cut)
			*cut = '\0';
	}

	var_addz(&st->vars, "hostname", host);
	var_addz(&st->vars, "ssh_symbol", ssh ? toml_str_at(st->cfg, "ssh_symbol", "🌐 ") : "");
	return true;
}

static bool probe_jobs(module_state *st)
{
	int jobs = st->ctx->jobs;
	long long symbol_threshold = toml_int_at(st->cfg, "symbol_threshold", 1);
	long long number_threshold = toml_int_at(st->cfg, "number_threshold", 2);

	if (jobs < symbol_threshold)
		return false;

	var_addz(&st->vars, "symbol", toml_str_at(st->cfg, "symbol", "✦"));
	if (jobs >= number_threshold)
		var_addf(&st->vars, "number", "%d", jobs);
	else
		var_addz(&st->vars, "number", "");
	return true;
}

static char *format_duration(long long ms, bool show_ms)
{
	str out = {0};
	long long total_s = ms / 1000;
	long long h = total_s / 3600;
	long long m = (total_s % 3600) / 60;
	long long s = total_s % 60;
	long long rem_ms = ms % 1000;

	if (h)
		str_addf(&out, "%lldh", h);
	if (h || m)
		str_addf(&out, "%lldm", m);
	if (show_ms) {
		str_addf(&out, "%lld.%03llds", s, rem_ms);
	} else {
		str_addf(&out, "%llds", s);
	}
	return str_take(&out);
}

static bool probe_cmd_duration(module_state *st)
{
	long long ms = st->ctx->duration_ms;
	if (ms < 0)
		return false;

	long long min_time = toml_int_at(st->cfg, "min_time", 2000);
	if (ms < min_time)
		return false;

	bool show_ms = toml_bool_at(st->cfg, "show_milliseconds", false);
	var_add(&st->vars, "duration", format_duration(ms, show_ms));
	return true;
}

static bool probe_time(module_state *st)
{
	const char *fmt = toml_str_at(st->cfg, "time_format", "%T");
	bool utc = toml_get(st->cfg, "utc_time_offset") != NULL;

	time_t now = time(NULL);
	struct tm tm_buf;
	const struct tm *tm = utc ? gmtime_r(&now, &tm_buf) : localtime_r(&now, &tm_buf);
	if (!tm)
		return false;

	char buf[256];
	if (strftime(buf, sizeof buf, fmt, tm) == 0)
		return false;

	var_addz(&st->vars, "time", buf);
	return true;
}

static const struct {
	int number;
	const char *name;
	const char *meaning;
} signal_table[] = {
	{1, "HUP", "Hangup"},        {2, "INT", "Interrupt"},
	{3, "QUIT", "Quit"},         {4, "ILL", "Illegal instruction"},
	{6, "ABRT", "Abort"},        {8, "FPE", "Floating point exception"},
	{9, "KILL", "Killed"},       {11, "SEGV", "Segmentation fault"},
	{13, "PIPE", "Broken pipe"}, {14, "ALRM", "Alarm"},
	{15, "TERM", "Terminated"},
};

static bool probe_status(module_state *st)
{
	int code = st->ctx->status;
	bool success = (code == 0);

	if (success && !toml_bool_at(st->cfg, "success_symbol_enabled", false))
		return false;

	const char *symbol = toml_str_at(st->cfg, "symbol", "✖");

	/* Shells report a signal as 128 + the signal number. */
	if (toml_bool_at(st->cfg, "map_symbol", false)) {
		if (success) {
			symbol = toml_str_at(st->cfg, "success_symbol", "");
		} else if (code == 126) {
			symbol = toml_str_at(st->cfg, "not_executable_symbol", "🚫");
		} else if (code == 127) {
			symbol = toml_str_at(st->cfg, "not_found_symbol", "🔍");
		} else if (code == 130) {
			symbol = toml_str_at(st->cfg, "sigint_symbol", "🧱");
		} else if (code > 128 && code < 165) {
			symbol = toml_str_at(st->cfg, "signal_symbol", "⚡");
		}
	}

	var_addz(&st->vars, "symbol", symbol);
	var_addf(&st->vars, "status", "%d", code);
	var_addf(&st->vars, "int", "%d", code);
	if (success)
		var_addz(&st->vars, "maybe_int", "");
	else
		var_addf(&st->vars, "maybe_int", "%d", code);

	if (code > 128 && code < 165) {
		int signum = code - 128;
		var_addf(&st->vars, "signal_number", "%d", signum);
		for (size_t i = 0; i < sizeof signal_table / sizeof *signal_table; i++) {
			if (signal_table[i].number == signum) {
				var_addz(&st->vars, "signal_name", signal_table[i].name);
				var_addz(&st->vars, "common_meaning", signal_table[i].meaning);
				break;
			}
		}
	} else if (code == 126) {
		var_addz(&st->vars, "common_meaning", "Command not executable");
	} else if (code == 127) {
		var_addz(&st->vars, "common_meaning", "Command not found");
	}

	if (st->ctx->pipestatus)
		var_addz(&st->vars, "pipestatus", st->ctx->pipestatus);

	return true;
}

static bool probe_line_break(module_state *st)
{
	(void)st;
	return true;
}

/* starship spells these "vimcmd_*"; "vicmd_symbol" is accepted as an alias so
 * older configs keep working. */
static const char *vim_symbol(module_state *st, const char *keymap)
{
	const char *key = "vimcmd_symbol";

	if (str_eq(keymap, "visual"))
		key = "vimcmd_visual_symbol";
	else if (str_eq(keymap, "vireplace"))
		key = "vimcmd_replace_symbol";
	else if (str_eq(keymap, "vireplace-one"))
		key = "vimcmd_replace_one_symbol";

	const char *symbol = toml_str_at(st->cfg, key, NULL);
	if (!symbol && key != NULL)
		symbol = toml_str_at(st->cfg, "vimcmd_symbol", NULL);
	if (!symbol)
		symbol = toml_str_at(st->cfg, "vicmd_symbol", "[❮](bold green)");
	return symbol;
}

static bool probe_character(module_state *st)
{
	const char *keymap = st->ctx->keymap;
	const char *symbol;

	if (str_eq(keymap, "vicmd") || str_eq(keymap, "main-vicmd") || str_eq(keymap, "visual") ||
	    str_eq(keymap, "vireplace") || str_eq(keymap, "vireplace-one"))
		symbol = vim_symbol(st, keymap);
	else if (st->ctx->status != 0)
		symbol = toml_str_at(st->cfg, "error_symbol", "[❯](bold red)");
	else
		symbol = toml_str_at(st->cfg, "success_symbol", "[❯](bold green)");

	var_add_raw(&st->vars, "symbol", module_render_sub(st, symbol));
	return true;
}

const module_def basic_modules[] = {
	{.name = "os", .format = "[$symbol]($style)", .style = "bold white", .disabled = true,
	 .probe = probe_os},
	{.name = "username", .format = "[$user]($style) in ", .style = "bold yellow",
	 .probe = probe_username},
	{.name = "hostname", .format = "[$ssh_symbol$hostname]($style) in ",
	 .style = "bold dimmed green", .probe = probe_hostname},
	{.name = "jobs", .format = "[$symbol$number]($style) ", .style = "bold blue",
	 .probe = probe_jobs},
	{.name = "cmd_duration", .format = "took [$duration]($style) ", .style = "yellow bold",
	 .probe = probe_cmd_duration},
	{.name = "time", .format = "at [$time]($style) ", .style = "bold yellow", .disabled = true,
	 .probe = probe_time},
	{.name = "status", .format = "[$symbol$status]($style) ", .style = "bold red",
	 .disabled = true, .probe = probe_status},
	{.name = "line_break", .format = "\n", .style = "", .probe = probe_line_break},
	{.name = "character", .format = "$symbol ", .style = "", .probe = probe_character},
};

const size_t basic_modules_count = sizeof basic_modules / sizeof *basic_modules;
