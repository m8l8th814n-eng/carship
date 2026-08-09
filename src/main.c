#include "cmd.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#define CARSHIP_VERSION "0.1.0"

static const char usage_text[] =
	"carship " CARSHIP_VERSION " - a fast cross-shell prompt\n"
	"\n"
	"usage: carship <command> [options]\n"
	"\n"
	"  prompt                 print the full prompt\n"
	"  init <shell>           print the shell integration script\n"
	"  preset <name>          print a built-in preset (--list to see them)\n"
	"  completion <shell>     print the shell completion script\n"
	"  module <name>          print a single module (--list to see them)\n"
	"  explain                show what each module contributes\n"
	"  config                 open the configuration file in $EDITOR\n"
	"  print-config           print the active configuration\n"
	"  time                   print the current time in milliseconds\n"
	"\n"
	"Supported shells: bash, zsh, ash (busybox)\n";

static int cmd_time(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	printf("%lld\n", (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fputs(usage_text, stdout);
		return 0;
	}

	const char *cmd = argv[1];
	int rest_argc = argc - 2;
	char **rest = argv + 2;

	if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-V") == 0) {
		printf("carship %s\n", CARSHIP_VERSION);
		return 0;
	}
	if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "help") == 0) {
		fputs(usage_text, stdout);
		return 0;
	}

	if (strcmp(cmd, "prompt") == 0)
		return cmd_prompt(rest_argc, rest);
	if (strcmp(cmd, "init") == 0)
		return cmd_init(rest_argc, rest, argv[0]);
	if (strcmp(cmd, "preset") == 0)
		return cmd_preset(rest_argc, rest, argv[0]);
	if (strcmp(cmd, "completion") == 0)
		return cmd_completion(rest_argc, rest, argv[0]);
	if (strcmp(cmd, "module") == 0)
		return cmd_module(rest_argc, rest);
	if (strcmp(cmd, "explain") == 0)
		return cmd_explain(rest_argc, rest);
	if (strcmp(cmd, "print-config") == 0)
		return cmd_print_config();
	if (strcmp(cmd, "config") == 0)
		return cmd_config();
	if (strcmp(cmd, "time") == 0)
		return cmd_time();

	fprintf(stderr, "carship: unknown command '%s'\n\n", cmd);
	fputs(usage_text, stderr);
	return 2;
}
