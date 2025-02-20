// SPDX-License-Identifier: BSD-3-Clause

#include "cmd.h"

#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../util/parser/parser.h"
#include "utils.h"

#define READ 0
#define WRITE 1
#define _POSIX_C_SOURCE 200809L

/**
 * Internal change-directory command.
 */
static bool shell_cd(word_t *dir)
{
	/* TODO: Execute cd. */
	char *path = NULL;

	if (dir && dir->next_word == NULL)
		path = get_word(dir);
	else
		return false;
	int rc = chdir(path);

	if (rc) {
		free(path);
		return false;
	}
	free(path);
	return true;
}

/**
 * Internal exit/quit command.
 */
static int shell_exit(void)
{
	/* TODO: Execute exit/quit. */
	exit(EXIT_SUCCESS);

	return 0;
}

/**
 * Parse a simple command (internal, environment variable assignment,
 * external command).
 */
static int parse_simple(simple_command_t *s, int level, command_t *father)
{
	if (strcmp(s->verb->string, "cd") == 0) {
		if (s->out && s->io_flags == 0) {
			char *out_file = get_word(s->out);

			open(out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		}
		bool ok = shell_cd(s->params);

		if (ok == true)
			return 0;
		return 1;
	} else if (strcmp(s->verb->string, "exit") == 0 ||
			   strcmp(s->verb->string, "quit") == 0) {
		return shell_exit();
	}
	int status = 0;
	int nr = 0;

	char **args = get_argv(s, &nr);

	if (strchr(args[0], '=')) {
		char *env_var = strdup(s->verb->string);
		word_t *p = s->verb->next_part->next_part;
		char *env_value = strdup(get_word(p));

		if (setenv(env_var, env_value, 1))
			return 1;
		return 0;

	} else {
		pid_t pid = fork();

		if (pid < 0) {
			perror("fork");
			exit(EXIT_FAILURE);
		}

		if (pid == 0) {
			if (s->in) {
				char *in_file = get_word(s->in);
				int fd = open(in_file, O_RDONLY);

				dup2(fd, 0);
				close(fd);
				free(in_file);
			}
			int both = 1;

			if (s->out && s->err &&
				strcmp(s->out->string, s->err->string) == 0 &&
				s->io_flags == 0) {
				char *out_err_file = get_word(s->out);
				int fd =
					open(out_err_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);

				dup2(fd, 1);
				dup2(fd, 2);
				close(fd);
				free(out_err_file);
				both = 0;
			}

			if (s->out && s->io_flags == 0 && both) {
				char *out_file = get_word(s->out);
				int fd = open(out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);

				dup2(fd, 1);
				close(fd);
				free(out_file);
			}

			if (s->err && s->io_flags == 0 && both) {
				char *err_file = get_word(s->err);
				int fd = open(err_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);

				dup2(fd, 2);
				close(fd);
				free(err_file);
			}

			if (s->out && s->io_flags == 1) {
				char *out_append_file = get_word(s->out);
				int fd = open(out_append_file,
							  O_WRONLY | O_CREAT | O_APPEND, 0644);

				dup2(fd, 1);
				close(fd);
				free(out_append_file);
			}

			if (s->err && s->io_flags == 2) {
				char *err_append_file = get_word(s->err);
				int fd = open(err_append_file,
							  O_WRONLY | O_CREAT | O_APPEND, 0644);

				dup2(fd, 2);
				close(fd);
				free(err_append_file);
			}

			execvp(args[0], args);
			char *error = (char *)malloc(100 * sizeof(char));

			strcat(error, "Execution failed for '");
			strcat(error, args[0]);
			strcat(error, "'\n");
			fprintf(stderr, "%s", error);
			exit(EXIT_FAILURE);
		} else {
			waitpid(pid, &status, 0);
			if (WIFEXITED(status))
				status = WEXITSTATUS(status);
			else
				status = 1;
		}

		for (int i = 0; i <= nr; i++)
			free(args[i]);
		free(args);
		return status;
	}
}

/**
 * Process two commands in parallel, by creating two children.
 */
static bool run_in_parallel(command_t *cmd1, command_t *cmd2, int level,
							command_t *father)
{
	pid_t pid1 = fork();

	int status1 = 0, status2 = 0;

	if (pid1 == 0)
		exit(parse_command(cmd1, level + 1, father));

	pid_t pid2 = fork();

	if (pid2 == 0)
		exit(parse_command(cmd2, level + 1, father));

	waitpid(pid1, &status1, 0);
	waitpid(pid2, &status2, 0);

	if (WIFEXITED(status1))
		status1 = WEXITSTATUS(status1);
	else
		status1 = 1;

	if (WIFEXITED(status2))
		status2 = WEXITSTATUS(status2);
	else
		status2 = 1;

	return (status1 == 0 && status2 == 0);
}

/**
 * Run commands by creating an anonymous pipe (cmd1 | cmd2).
 */
static bool run_on_pipe(command_t *cmd1, command_t *cmd2, int level,
						command_t *father)
{
	int fds[2], status1 = 0, status2 = 0;

	if (pipe(fds) < 0) {
		perror("pipe");
		exit(1);
	}

	pid_t pid1 = fork();

	if (pid1 == 0) {
		dup2(fds[1], 1);
		close(fds[0]);
		close(fds[1]);
		exit(parse_command(cmd1, level + 1, father));
	}

	pid_t pid2 = fork();

	if (pid2 == 0) {
		dup2(fds[0], 0);
		close(fds[0]);
		close(fds[1]);
		exit(parse_command(cmd2, level + 1, father));
	}

	close(fds[0]);
	close(fds[1]);
	waitpid(pid1, &status1, 0);
	waitpid(pid2, &status2, 0);

	if (WIFEXITED(status1))
		status1 = WEXITSTATUS(status1);
	else
		status1 = 1;

	if (WIFEXITED(status2))
		status2 = WEXITSTATUS(status2);
	else
		status2 = 1;

	return (status2 == 0);
}
/**
 * Parse and execute a command.
 */
int parse_command(command_t *c, int level, command_t *father)
{
	/* TODO: sanity checks */
	int rc;

	if (c->op == OP_NONE) {
		/* TODO: Execute a simple command. */
		return parse_simple(c->scmd, level, father);
	}

	switch (c->op) {
	case OP_SEQUENTIAL:
			/* TODO: Execute the commands one after the other. */
			if (c->cmd1)
				rc = parse_command(c->cmd1, level + 1, c);
			if (c->cmd2)
				rc = parse_command(c->cmd2, level + 1, c);
			break;

	case OP_PARALLEL: {
			/* TODO: Execute the commands simultaneously. */
			bool ok = run_in_parallel(c->cmd1, c->cmd2, level, c);

			if (ok == false)
				return 1;
			break;
		}

	case OP_CONDITIONAL_NZERO:
			/* TODO: Execute the second command only if the first one
			 * returns non zero.
			 */
			if (c->cmd1) {
				rc = parse_command(c->cmd1, level + 1, c);
				if (rc == 0)
					return 0;
			}
			if (c->cmd2) {
				rc = parse_command(c->cmd2, level + 1, c);
				if (rc == 0)
					return 0;
			}
			return 1;

	case OP_CONDITIONAL_ZERO:
			/* TODO: Execute the second command only if the first one
			 * returns zero.
			 */
			if (c->cmd1) {
				rc = parse_command(c->cmd1, level + 1, c);
				if (rc)
					return 1;
			}
			if (c->cmd2) {
				rc = parse_command(c->cmd2, level + 1, c);
				if (rc)
					return 1;
			}
			break;

	case OP_PIPE: {
			/* TODO: Redirect the output of the first command to the
			 * input of the second.
			 */
			bool end = run_on_pipe(c->cmd1, c->cmd2, level + 1, c);

			if (end == false)
				return 1;
			break;
		}

	default:
			return SHELL_EXIT;
	}
	return 0;
}
