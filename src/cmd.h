#ifndef CARSHIP_CMD_H
#define CARSHIP_CMD_H

char *self_path(const char *argv0);

int cmd_init(int argc, char **argv, const char *argv0);
int cmd_preset(int argc, char **argv, const char *argv0);
int cmd_completion(int argc, char **argv, const char *argv0);
int cmd_prompt(int argc, char **argv);
int cmd_module(int argc, char **argv);
int cmd_explain(int argc, char **argv);
int cmd_print_config(void);
int cmd_config(void);

#endif
