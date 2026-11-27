/* cli/launch_values.h - the values launch options take (--restart,
 * --restart-delay, --detach-keys, -e, --env-file), parsed and checked. */
#pragma once
#ifndef BACKMUX_CLI_LAUNCH_VALUES_H
#define BACKMUX_CLI_LAUNCH_VALUES_H
#include "backmux/config.h"

int cli_parse_restart_policy_arg(const char *arg, char out[64]);
bool cli_restart_policy_arg_enabled(const char *arg);
int cli_parse_restart_delay_arg(const char *arg, int *out);
int cli_configure_detach_keys_option(const char *value);
int cli_append_env_assignment(const char *arg, char ***env_out, int *envc_out);
int cli_append_env_file(const char *path, char ***env_out, int *envc_out);

#endif
