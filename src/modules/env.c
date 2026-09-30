#include "module.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static bool over_ssh(void)
{
	return getenv("SSH_CONNECTION") || getenv("SSH_CLIENT") || getenv("SSH_TTY");
}

static bool probe_shlvl(module_state *st)
{
	const char *raw = getenv("SHLVL");
	if (!raw || !*raw)
		return false;

	long long level = atoll(raw);
	if (level < toml_int_at(st->cfg, "threshold", 2))
		return false;

	var_addz(&st->vars, "symbol", toml_str_at(st->cfg, "symbol", "↕️  "));
	var_addf(&st->vars, "shlvl", "%lld", level);

	if (toml_bool_at(st->cfg, "repeat", false)) {
		const char *sym = toml_str_at(st->cfg, "symbol", "↕️  ");
		str s = {0};
		for (long long i = 0; i < level && i < 32; i++)
			str_add(&s, sym);
		var_add(&st->vars, "symbol", str_take(&s));
	}
	return true;
}

static const struct {
	const char *shell;
	const char *key;
	const char *dflt;
} shell_indicators[] = {
	{"bash", "bash_indicator", "bsh"},
	{"zsh", "zsh_indicator", "zsh"},
	{"ash", "ash_indicator", "ash"},
};

static bool probe_shell(module_state *st)
{
	const char *shell = st->ctx->shell;
	if (!shell || !*shell)
		return false;

	const char *indicator = shell;
	for (size_t i = 0; i < sizeof shell_indicators / sizeof *shell_indicators; i++) {
		if (strcmp(shell_indicators[i].shell, shell) == 0) {
			indicator = toml_str_at(st->cfg, shell_indicators[i].key,
			                        shell_indicators[i].dflt);
			break;
		}
	}

	var_addz(&st->vars, "indicator", indicator);
	return true;
}

static bool probe_nix_shell(module_state *st)
{
	const char *in_shell = getenv("IN_NIX_SHELL");
	const char *name = getenv("name");
	if (!in_shell || !*in_shell)
		return false;

	bool impure = strcmp(in_shell, "impure") == 0;
	var_addz(&st->vars, "symbol", toml_str_at(st->cfg, "symbol", "❄️  "));
	var_addz(&st->vars, "state",
	         impure ? toml_str_at(st->cfg, "impure_msg", "impure")
	                : toml_str_at(st->cfg, "pure_msg", "pure"));
	var_addz(&st->vars, "name", name ? name : "");
	return true;
}

/* Container detection mirrors what systemd-detect-virt looks at, without
 * spawning it. */
static bool probe_container(module_state *st)
{
	char *name = NULL;

	if (file_exists("/run/.containerenv")) {
		char *content = read_file("/run/.containerenv", NULL);
		if (content) {
			char *key = strstr(content, "image=\"");
			if (key) {
				char *start = key + 7;
				char *end = strchr(start, '"');
				if (end)
					name = xstrndup(start, (size_t)(end - start));
			}
			free(content);
		}
		if (!name)
			name = xstrdup("podman");
	} else if (file_exists("/.dockerenv")) {
		name = xstrdup("docker");
	} else {
		char *content = read_file("/run/systemd/container", NULL);
		if (content) {
			name = xstrdup(trim(content));
			free(content);
		}
	}

	if (!name)
		return false;

	var_addz(&st->vars, "symbol", toml_str_at(st->cfg, "symbol", "⬢"));
	var_add(&st->vars, "name", name);
	return true;
}

static bool probe_env_var(module_state *st)
{
	const char *variable = toml_str_at(st->cfg, "variable", NULL);
	if (!variable)
		return false;

	const char *value = getenv(variable);
	if (!value || !*value)
		value = toml_str_at(st->cfg, "default", NULL);
	if (!value || !*value)
		return false;

	var_addz(&st->vars, "symbol", toml_str_at(st->cfg, "symbol", ""));
	var_addz(&st->vars, "env_value", value);
	return true;
}

/* `sudo -n true` succeeds only while a credential cache is still valid; it
 * never prompts. */
static bool probe_sudo(module_state *st)
{
	if (!toml_bool_at(st->cfg, "allow_windows", true))
		return false;

	char *const argv[] = {(char *)"sudo", (char *)"-n", (char *)"true", NULL};
	char *out = capture(st->ctx->cwd, argv, (int)toml_int_at(st->cfg, "command_timeout", 500));
	if (!out)
		return false;
	free(out);

	var_addz(&st->vars, "symbol", toml_str_at(st->cfg, "symbol", "🧙 "));
	return true;
}

/* The first IPv4 address on an interface that is up and not loopback. */
static bool first_ipv4(char *out, size_t outlen)
{
	struct ifaddrs *addrs = NULL;
	if (getifaddrs(&addrs) != 0)
		return false;

	out[0] = '\0';
	for (const struct ifaddrs *ifa = addrs; ifa; ifa = ifa->ifa_next) {
		if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
			continue;
		if (ifa->ifa_flags & IFF_LOOPBACK)
			continue;
		if (!(ifa->ifa_flags & IFF_UP))
			continue;

		const struct sockaddr_in *sin = (const struct sockaddr_in *)(void *)ifa->ifa_addr;
		if (inet_ntop(AF_INET, &sin->sin_addr, out, (socklen_t)outlen))
			break;
	}
	freeifaddrs(addrs);
	return out[0] != '\0';
}

static bool probe_localip(module_state *st)
{
	if (toml_bool_at(st->cfg, "ssh_only", true) && !over_ssh())
		return false;

	char found[INET_ADDRSTRLEN];
	if (!first_ipv4(found, sizeof found))
		return false;

	var_addz(&st->vars, "localipv4", found);
	return true;
}

/* A short localip for use in a segment: shown everywhere, not just over ssh. */
static bool probe_ip(module_state *st)
{
	char found[INET_ADDRSTRLEN];
	if (!first_ipv4(found, sizeof found))
		return false;

	var_addz(&st->vars, "symbol", toml_str_at(st->cfg, "symbol", "\U000f0a60"));
	var_addz(&st->vars, "ip", found);
	return true;
}

/* The controlling terminal, the way zsh's %l reports it: "/dev/" comes off,
 * and a "/dev/tty" prefix comes off entirely so tty1 shows as "1". stdout is a
 * pipe whenever the shell captures the prompt, so stdin and stderr are tried
 * first. */
static bool probe_tty(module_state *st)
{
	const char *name = NULL;
	const int fds[] = {STDIN_FILENO, STDERR_FILENO, STDOUT_FILENO};

	for (size_t i = 0; i < sizeof fds / sizeof *fds && !name; i++)
		if (isatty(fds[i]))
			name = ttyname(fds[i]);

	if (!name)
		return false;

	if (has_prefix(name, "/dev/tty"))
		name += 8;
	else if (has_prefix(name, "/dev/"))
		name += 5;

	if (!*name)
		return false;

	var_addz(&st->vars, "symbol", toml_str_at(st->cfg, "symbol", ""));
	var_addz(&st->vars, "tty", name);
	return true;
}

/* Emits a marker that the prompt renderer expands once the rest of the line's
 * width is known. */
static bool probe_fill(module_state *st)
{
	const char *symbol = toml_str_at(st->cfg, "symbol", ".");
	str marker = {0};
	str_addc(&marker, FILL_MARKER);
	str_add(&marker, symbol);
	str_addc(&marker, FILL_MARKER);
	var_add_raw(&st->vars, "symbol", str_take(&marker));
	return true;
}

const module_def env_modules[] = {
	{.name = "shlvl", .format = "[$symbol$shlvl]($style) ", .style = "bold yellow",
	 .disabled = true, .probe = probe_shlvl},
	{.name = "shell", .format = "[$indicator]($style) ", .style = "white bold",
	 .disabled = true, .probe = probe_shell},
	{.name = "nix_shell", .format = "via [$symbol$state( \\($name\\))]($style) ",
	 .style = "bold blue", .probe = probe_nix_shell},
	{.name = "container", .format = "[$symbol \\[$name\\]]($style) ",
	 .style = "red bold dimmed", .probe = probe_container},
	{.name = "env_var", .format = "with [$env_value]($style) ", .style = "black bold dimmed",
	 .disabled = true, .probe = probe_env_var},
	{.name = "sudo", .format = "[as $symbol]($style)", .style = "bold blue", .disabled = true,
	 .probe = probe_sudo},
	{.name = "localip", .format = "[$localipv4]($style) ", .style = "yellow bold",
	 .disabled = true, .probe = probe_localip},
	{.name = "ip", .format = "[$symbol $ip]($style) ", .style = "yellow", .probe = probe_ip},
	{.name = "tty", .format = "[$symbol$tty]($style) ", .style = "dimmed white",
	 .disabled = true, .probe = probe_tty},
	{.name = "fill", .format = "$symbol", .style = "black bold", .probe = probe_fill},
};

const size_t env_modules_count = sizeof env_modules / sizeof *env_modules;
