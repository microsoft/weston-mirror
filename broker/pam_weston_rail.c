/*
 * pam_weston_rail.so: keeps /etc/weston-rail/ntlm.sam in sync
 *
 * Copyright © 2026 weston-mirror RAIL contributors
 * MIT licensed like the rest of weston (see COPYING).
 *
 * NLA at weston-rail-broker needs the NT hash of each user's password.
 * This module hands the password to "weston-rail-passwd --pam-sync":
 *
 *   auth      after the password check (common-auth): on every login with
 *             a password (SSH, console, su, the broker without NLA)
 *   password  after the password was changed (common-password), with the
 *             new password - pam_exec does not pass that one on
 *
 * It never influences the result of the stack (always PAM_IGNORE).
 *
 * Options: tool=/path/to/weston-rail-passwd
 *
 * weston builds with hidden symbol visibility, so the PAM entry points
 * are exported explicitly.
 */

#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#define PAM_SM_AUTH
#define PAM_SM_PASSWORD
#include <security/pam_modules.h>
#include <security/pam_ext.h>

#ifndef WESTON_RAIL_PASSWD
#define WESTON_RAIL_PASSWD "/usr/local/sbin/weston-rail-passwd"
#endif

static void
sync_password(pam_handle_t *pamh, int argc, const char **argv)
{
	const char *tool = WESTON_RAIL_PASSWD;
	const void *user = NULL, *tok = NULL;
	struct sigaction sa_old, sa_dfl = { .sa_handler = SIG_DFL };
	int fds[2], i, status;
	pid_t pid;

	for (i = 0; i < argc; i++)
		if (!strncmp(argv[i], "tool=", 5))
			tool = argv[i] + 5;

	if (pam_get_item(pamh, PAM_USER, &user) != PAM_SUCCESS || !user ||
	    pam_get_item(pamh, PAM_AUTHTOK, &tok) != PAM_SUCCESS || !tok ||
	    !*(const char *)tok)
		return;
	if (access(tool, X_OK) != 0)
		return;
	if (pipe2(fds, O_CLOEXEC) < 0)
		return;

	/* the calling application may ignore SIGCHLD; we need the status */
	sigaction(SIGCHLD, &sa_dfl, &sa_old);
	pid = fork();
	if (pid == 0) {
		char *env[] = { NULL, "PAM_TYPE=sync", NULL };
		char envuser[128];
		int devnull = open("/dev/null", O_WRONLY);

		dup2(fds[0], STDIN_FILENO);
		if (devnull >= 0) {
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
		}
		snprintf(envuser, sizeof envuser, "PAM_USER=%s", (const char *)user);
		env[0] = envuser;
		execle(tool, tool, "--pam-sync", (char *)NULL, env);
		_exit(127);
	}
	close(fds[0]);
	if (pid > 0) {
		const char *p = tok;
		size_t len = strlen(p);

		while (len > 0) {
			ssize_t n = write(fds[1], p, len);

			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				break;
			p += n;
			len -= (size_t)n;
		}
	}
	close(fds[1]);
	if (pid > 0)
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
			;
	sigaction(SIGCHLD, &sa_old, NULL);
}

__attribute__((visibility("default"))) PAM_EXTERN int
pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
	(void)flags;
	/* placed after pam_unix: PAM_AUTHTOK is the verified password */
	sync_password(pamh, argc, argv);
	return PAM_IGNORE;
}

__attribute__((visibility("default"))) PAM_EXTERN int
pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
	(void)pamh;
	(void)flags;
	(void)argc;
	(void)argv;
	return PAM_IGNORE;
}

__attribute__((visibility("default"))) PAM_EXTERN int
pam_sm_chauthtok(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
	/* the preliminary pass only checks; the new token is final in the
	 * update pass, after pam_unix stored it */
	if (flags & PAM_UPDATE_AUTHTOK)
		sync_password(pamh, argc, argv);
	return PAM_IGNORE;
}
