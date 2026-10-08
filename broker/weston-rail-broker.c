/*
 * weston-rail-broker: RDP session broker for weston-mirror RAIL
 *
 * Copyright © 2026 weston-mirror RAIL contributors
 * MIT licensed like the rest of weston (see COPYING).
 *
 * Runs as root on the RDP port and gives every user an own weston:
 *
 *  1. A new connection is terminated here (TLS). The client sends user
 *     name and password in its Client Info PDU (mstsc without NLA, with
 *     "prompt for credentials:i:1"); they are checked with PAM
 *     (service "weston-rail").
 *  2. The client is redirected (Server Redirection PDU) to this very
 *     broker with a random one-time routing token (valid 60 s).
 *  3. The client reconnects; the token is read from the X.224 connection
 *     request before TLS starts, and the untouched TCP connection is
 *     handed to the user's weston over its control socket (SCM_RIGHTS).
 *     If the user has no weston yet, one is started under the user's
 *     account with an own PAM session.
 *
 * The per-user weston runs with WESTON_RDP_CONTROL_SOCKET (no port of its
 * own) and WESTON_RDP_IDLE_EXIT_SEC, so it ends when no client is
 * connected and no application is left.
 */

#include "config.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <security/pam_appl.h>

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>
#include <freerdp/redirection.h>
#include <freerdp/settings.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>
#include <winpr/ssl.h>
#include <winpr/ntlm.h>
#include <winpr/string.h>
#include <winpr/synch.h>
#include <winpr/wlog.h>

#ifndef WESTON_BINARY
#define WESTON_BINARY "/usr/local/bin/weston"
#endif

#define BROKER_PAM_SERVICE "weston-rail"
#define TOKEN_LIFETIME_SEC 60
#define AUTH_TIMEOUT_SEC 60
#define HANDOFF_WAIT_SEC 45
#define TOKEN_HEX_LEN 32

#ifndef WINDOW_LEVEL_SUPPORTED_EX
#define WINDOW_LEVEL_SUPPORTED_EX 0x00000002
#endif

static struct {
	int port;
	char *cert;
	char *key;
	char *weston;
	int idle_exit_sec;
	bool allow_root;
	bool xwayland;
	bool verbose;
	char *names[16];	/* --name: extra DNS names for the certificate */
	int n_names;
	char *sam;		/* NT hashes for NLA/NTLM (weston-rail-passwd) */
	char *keytab;		/* Kerberos keytab for NLA in an AD domain */
	bool nla;		/* offer NLA (CredSSP) */
	bool tls_login;		/* accept logins without NLA (Client Info) */
	int user_map;		/* how NLA user/domain map to a Linux account */
} cfg = {
	.port = 3389,
	.cert = "/etc/weston-rail/tls.crt",
	.key = "/etc/weston-rail/tls.key",
	.weston = WESTON_BINARY,
	.idle_exit_sec = 60,
	.sam = "/etc/weston-rail/ntlm.sam",
	.keytab = "/etc/weston-rail/krb5.keytab",
	.nla = true,
	.tls_login = true,
	.xwayland = true,
};

enum { USER_MAP_PLAIN, USER_MAP_UPN, USER_MAP_NETBIOS };

struct pending {
	struct pending *next;
	char token[TOKEN_HEX_LEN + 1];
	char user[64];		/* Linux account */
	char client_user[128];	/* name the client authenticates with */
	char nthash[33];	/* for NLA on the reconnect (may be empty) */
	time_t expires;
};

struct login {
	char user[64];
	char client_user[128];
	char nthash[33];
};

/* RAIL sessions serve mstsc (RemoteApp), web sessions serve the browser
 * (weston-rail-web): one program, kiosk or desktop shell, ordinary RDP. */
enum session_kind { SESSION_RAIL, SESSION_WEB };

struct session {
	struct session *next;
	char user[64];
	char app[64];		/* Web-Sitzungen: eine je Benutzer und Programm */
	enum session_kind kind;
	uid_t uid;
	pid_t pid;		/* session keeper process */
	char ctl[108];		/* weston control socket */
	time_t started;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct pending *pendings;
static struct session *sessions;

/* ------------------------------------------------------------------ */

static void
logmsg(const char *fmt, ...)
{
	va_list ap;
	char ts[32];
	time_t now = time(NULL);

	strftime(ts, sizeof ts, "%H:%M:%S", localtime(&now));
	fprintf(stderr, "[%s] ", ts);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static void
debugmsg(const char *fmt, ...)
{
	va_list ap;

	if (!cfg.verbose)
		return;
	va_start(ap, fmt);
	fprintf(stderr, "    ");
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static bool
valid_username(const char *u)
{
	size_t i;

	if (!u || !*u || strlen(u) >= 64 || u[0] == '-')
		return false;
	for (i = 0; u[i]; i++) {
		unsigned char c = (unsigned char)u[i];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
			return false;
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* PAM                                                                 */
/* ------------------------------------------------------------------ */

struct pam_creds {
	const char *user;
	const char *password;
};

static int
pam_conv_cb(int num, const struct pam_message **msg, struct pam_response **resp,
	    void *data)
{
	struct pam_creds *c = data;
	struct pam_response *r;
	int i;

	if (num <= 0 || num > PAM_MAX_NUM_MSG)
		return PAM_CONV_ERR;
	r = calloc(num, sizeof *r);
	if (!r)
		return PAM_BUF_ERR;
	for (i = 0; i < num; i++) {
		switch (msg[i]->msg_style) {
		case PAM_PROMPT_ECHO_OFF:
			r[i].resp = strdup(c->password ? c->password : "");
			break;
		case PAM_PROMPT_ECHO_ON:
			r[i].resp = strdup(c->user ? c->user : "");
			break;
		case PAM_ERROR_MSG:
		case PAM_TEXT_INFO:
			debugmsg("PAM: %s", msg[i]->msg);
			break;
		default:
			free(r);
			return PAM_CONV_ERR;
		}
	}
	*resp = r;
	return PAM_SUCCESS;
}

static bool
pam_check(const char *user, const char *password, const char *rhost)
{
	struct pam_creds creds = { user, password };
	struct pam_conv conv = { pam_conv_cb, &creds };
	pam_handle_t *pamh = NULL;
	int rc;

	rc = pam_start(BROKER_PAM_SERVICE, user, &conv, &pamh);
	if (rc != PAM_SUCCESS) {
		logmsg("PAM: pam_start failed: %s", pam_strerror(pamh, rc));
		return false;
	}
	if (rhost)
		pam_set_item(pamh, PAM_RHOST, rhost);
	rc = pam_authenticate(pamh, PAM_DISALLOW_NULL_AUTHTOK);
	if (rc == PAM_SUCCESS)
		rc = pam_acct_mgmt(pamh, PAM_DISALLOW_NULL_AUTHTOK);
	if (rc != PAM_SUCCESS)
		logmsg("login of '%s' from %s refused: %s", user,
		       rhost ? rhost : "?", pam_strerror(pamh, rc));
	pam_end(pamh, rc);
	return rc == PAM_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* tokens                                                              */
/* ------------------------------------------------------------------ */

static bool
token_create(const struct login *login, char out[TOKEN_HEX_LEN + 1])
{
	unsigned char raw[TOKEN_HEX_LEN / 2];
	struct pending *p;
	int i;

	if (getrandom(raw, sizeof raw, 0) != (ssize_t)sizeof raw)
		return false;
	for (i = 0; i < (int)sizeof raw; i++)
		sprintf(out + 2 * i, "%02x", raw[i]);
	out[TOKEN_HEX_LEN] = '\0';

	p = calloc(1, sizeof *p);
	if (!p)
		return false;
	memcpy(p->token, out, sizeof p->token);
	snprintf(p->user, sizeof p->user, "%s", login->user);
	snprintf(p->client_user, sizeof p->client_user, "%s", login->client_user);
	snprintf(p->nthash, sizeof p->nthash, "%s", login->nthash);
	p->expires = time(NULL) + TOKEN_LIFETIME_SEC;
	pthread_mutex_lock(&lock);
	p->next = pendings;
	pendings = p;
	pthread_mutex_unlock(&lock);
	return true;
}

/* one-time: a matching token is removed */
static bool
token_take(const char *token, struct login *login)
{
	struct pending **pp, *p;
	time_t now = time(NULL);
	bool found = false;

	pthread_mutex_lock(&lock);
	for (pp = &pendings; (p = *pp);) {
		if (p->expires < now) {
			*pp = p->next;
			free(p);
			continue;
		}
		if (!found && strcmp(p->token, token) == 0) {
			memset(login, 0, sizeof *login);
			snprintf(login->user, sizeof login->user, "%s", p->user);
			snprintf(login->client_user, sizeof login->client_user, "%s", p->client_user);
			snprintf(login->nthash, sizeof login->nthash, "%s", p->nthash);
			*pp = p->next;
			free(p);
			found = true;
			continue;
		}
		pp = &p->next;
	}
	pthread_mutex_unlock(&lock);
	return found;
}

/* ------------------------------------------------------------------ */
/* per-user weston                                                     */
/* ------------------------------------------------------------------ */

static void
read_default_lang(char *out, size_t size)
{
	FILE *f = fopen("/etc/default/locale", "re");
	char line[256];

	snprintf(out, size, "C.UTF-8");
	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		char *v;

		if (strncmp(line, "LANG=", 5) != 0)
			continue;
		v = line + 5;
		v[strcspn(v, "\r\n")] = '\0';
		if (*v == '"' || *v == '\'') {
			v++;
			v[strcspn(v, "\"'")] = '\0';
		}
		if (*v)
			snprintf(out, size, "%s", v);
	}
	fclose(f);
}

static bool
copy_file_for_user(const char *src, const char *dst, uid_t uid, gid_t gid)
{
	char buf[8192];
	int in, out;
	ssize_t n;
	bool ok = true;

	in = open(src, O_RDONLY | O_CLOEXEC);
	if (in < 0)
		return false;
	unlink(dst);
	out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (out < 0) {
		close(in);
		return false;
	}
	while ((n = read(in, buf, sizeof buf)) > 0)
		if (write(out, buf, n) != n)
			ok = false;
	if (fchown(out, uid, gid) < 0)
		ok = false;
	close(in);
	close(out);
	return ok;
}


/*
 * The program a web session starts, from the same allowlist the RAIL
 * sessions use (/etc/weston-rail/apps.conf, [app] name/command).
 */
static bool
lookup_app(const char *name, char *command, size_t size)
{
	const char *conf = getenv("WESTON_RAIL_APPS_CONF");
	char line[4096], cur_name[128] = "", cur_cmd[2048] = "";
	bool in_app = false, found = false;
	FILE *f;

	if (!conf || !*conf)
		conf = "/etc/weston-rail/apps.conf";
	f = fopen(conf, "re");
	if (!f)
		return false;
	while (!found && fgets(line, sizeof line, f)) {
		char *l = line, *eq, *end;

		while (*l == ' ' || *l == '\t')
			l++;
		end = l + strlen(l);
		while (end > l && strchr(" \t\r\n", end[-1]))
			*--end = '\0';
		if (!*l || *l == '#' || *l == ';')
			continue;
		if (*l == '[') {
			if (in_app && !strcasecmp(cur_name, name) && cur_cmd[0]) {
				found = true;
				break;
			}
			in_app = !strcmp(l, "[app]");
			cur_name[0] = cur_cmd[0] = '\0';
			continue;
		}
		eq = strchr(l, '=');
		if (!eq || !in_app)
			continue;
		*eq++ = '\0';
		end = l + strlen(l);
		while (end > l && (end[-1] == ' ' || end[-1] == '\t'))
			*--end = '\0';
		while (*eq == ' ' || *eq == '\t')
			eq++;
		if (!strcmp(l, "name"))
			snprintf(cur_name, sizeof cur_name, "%s", eq);
		else if (!strcmp(l, "command"))
			snprintf(cur_cmd, sizeof cur_cmd, "%s", eq);
	}
	if (!found && in_app && !strcasecmp(cur_name, name) && cur_cmd[0])
		found = true;
	fclose(f);
	if (found)
		snprintf(command, size, "%s", cur_cmd);
	return found;
}

/* split a command line like the shell would, without a shell */
static char **
split_command(const char *cmd)
{
	size_t len = strlen(cmd);
	char **argv = calloc(len / 2 + 2, sizeof *argv);
	char *buf = malloc(len + 1), *out;
	int argc = 0;
	char quote = 0;
	bool in_arg = false;

	if (!argv || !buf) {
		free(argv);
		free(buf);
		return NULL;
	}
	out = buf;
	for (const char *p = cmd; *p; p++) {
		char c = *p;

		if (quote) {
			if (c == quote)
				quote = 0;
			else
				*out++ = c;
			continue;
		}
		if (c == ' ' || c == '\t') {
			if (in_arg) {
				*out++ = '\0';
				in_arg = false;
			}
			continue;
		}
		if (!in_arg) {
			argv[argc++] = out;
			in_arg = true;
		}
		if (c == '\'' || c == '"')
			quote = c;
		else
			*out++ = c;
	}
	*out = '\0';
	if (argc == 0) {
		free(buf);
		free(argv);
		return NULL;
	}
	argv[argc] = NULL;
	return argv;
}

struct pam_open_job {
	pam_handle_t *pamh;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool done;
	int rc;
};

static void *
pam_open_thread(void *data)
{
	struct pam_open_job *job = data;
	int rc = pam_open_session(job->pamh, PAM_SILENT);

	pthread_mutex_lock(&job->lock);
	job->rc = rc;
	job->done = true;
	pthread_cond_signal(&job->cond);
	pthread_mutex_unlock(&job->lock);
	return NULL;
}

static volatile pid_t keeper_child;

/* weston-rail-sessions --logoff: end the user's weston (its whole process
 * group: dbus-run-session, dbus-daemon, weston). The applications notice
 * that their compositor is gone and exit. */
static void
keeper_sigterm(int sig)
{
	(void)sig;
	if (keeper_child > 0)
		kill(-keeper_child, SIGTERM);
}

/* runs in the session keeper process (root), never returns */
static void
session_keeper(const struct passwd *pw, const char *ctl, enum session_kind kind,
	       const char *app, const char *app_command, const char *shell)
{
	struct pam_creds creds = { pw->pw_name, NULL };
	struct pam_conv conv = { pam_conv_cb, &creds };
	pam_handle_t *pamh = NULL;
	char rundir[64], logpath[128], certpath[128], keypath[128], lang[64];
	char ini_path[160] = "", config_opt[200] = "";
	char **penv = NULL;
	bool session_open = false, pam_done = false, pam_pending = false;
	pthread_t pam_thread;
	struct stat st;
	pid_t pid;
	int rc, status = 0;

	setsid();

	/*
	 * Geerbte Dateideskriptoren schliessen. Der Wächter lebt so lange wie
	 * die Sitzung und ruft selbst kein exec auf; eine mitgeerbte offene
	 * Verwaltungsverbindung würde den Aufrufer (weston-rail-web) warten
	 * lassen, bis die Sitzung endet.
	 */
	closefrom(3);

	/*
	 * pam_systemd blocks for 25 s when systemd-logind does not answer (e.g.
	 * an LXC container without "nesting"), longer than the client waits for
	 * its session. Open the PAM session in a thread and give it 5 s; after
	 * that weston starts anyway (the runtime directory is created below)
	 * and the session is closed later if it opened after all.
	 */
	rc = pam_start(BROKER_PAM_SERVICE, pw->pw_name, &conv, &pamh);
	if (rc == PAM_SUCCESS) {
		struct pam_open_job job = { .pamh = pamh };
		struct timespec deadline;
		pthread_t tid;

		pam_set_item(pamh, PAM_TTY, "weston-rail");
		pam_setcred(pamh, PAM_ESTABLISH_CRED);
		pthread_mutex_init(&job.lock, NULL);
		pthread_cond_init(&job.cond, NULL);
		if (pthread_create(&tid, NULL, pam_open_thread, &job) == 0) {
			clock_gettime(CLOCK_REALTIME, &deadline);
			deadline.tv_sec += 5;
			pthread_mutex_lock(&job.lock);
			while (!job.done &&
			       pthread_cond_timedwait(&job.cond, &job.lock, &deadline) == 0)
				;
			pam_done = job.done;
			pthread_mutex_unlock(&job.lock);
			if (pam_done) {
				pthread_join(tid, NULL);
				session_open = job.rc == PAM_SUCCESS;
				if (session_open)
					penv = pam_getenvlist(pamh);
				else
					logmsg("session for %s: no PAM session (continuing)",
					       pw->pw_name);
			} else {
				pam_thread = tid;
				pam_pending = true;
				logmsg("session for %s: PAM session does not answer (is "
				       "systemd-logind running? LXC: Options -> Features -> "
				       "nesting), starting without it", pw->pw_name);
			}
		}
	}

	/* pam_systemd creates the runtime directory; without logind do it */
	snprintf(rundir, sizeof rundir, "/run/user/%u", (unsigned)pw->pw_uid);
	if (stat(rundir, &st) < 0) {
		mkdir("/run/user", 0755);
		if (mkdir(rundir, 0700) == 0 && chown(rundir, pw->pw_uid, pw->pw_gid) < 0)
			logmsg("session for %s: cannot chown %s", pw->pw_name, rundir);
	}

	/* same TLS identity as the broker, so the client sees one certificate */
	snprintf(certpath, sizeof certpath, "%s/weston-rail-tls.crt", rundir);
	snprintf(keypath, sizeof keypath, "%s/weston-rail-tls.key", rundir);
	if (!copy_file_for_user(cfg.cert, certpath, pw->pw_uid, pw->pw_gid) ||
	    !copy_file_for_user(cfg.key, keypath, pw->pw_uid, pw->pw_gid))
		logmsg("session for %s: cannot provide the TLS certificate", pw->pw_name);
	if (kind == SESSION_WEB)
		snprintf(logpath, sizeof logpath, "%s/weston-rail-web-%s.log", rundir,
			 app && *app ? app : "sitzung");
	if (kind == SESSION_WEB && shell && !strcmp(shell, "desktop")) {
		/* desktop-shell ohne Panel und mit schwarzem Hintergrund: der
		 * Browser-Tab soll die Anwendung zeigen, keinen Schreibtisch */
		FILE *f;

		snprintf(ini_path, sizeof ini_path, "%s/weston-rail-web-%s.ini", rundir,
			 app && *app ? app : "sitzung");
		f = fopen(ini_path, "we");
		if (f) {
			fprintf(f, "[shell]\npanel-position=none\n"
				   "background-color=0xff000000\n"
				   "animation=none\nstartup-animation=none\n"
				   "close-animation=none\nfocus-animation=none\n");
			fclose(f);
			if (chown(ini_path, pw->pw_uid, pw->pw_gid) < 0)
				logmsg("web session for %s: cannot chown the configuration",
				       pw->pw_name);
			snprintf(config_opt, sizeof config_opt, "--config=%s", ini_path);
		}
	}
	else
		snprintf(logpath, sizeof logpath, "%s/weston-rail.log", rundir);
	read_default_lang(lang, sizeof lang);

	signal(SIGTERM, keeper_sigterm);
	pid = fork();
	if (pid == 0) {
		char *argv[16];

		setpgid(0, 0);
		signal(SIGTERM, SIG_DFL);
		char cert_opt[160], key_opt[160], log_opt[160], idle[16];
		char wayland_socket[128];
		int argc = 0, fd, i;

		if (initgroups(pw->pw_name, pw->pw_gid) < 0 ||
		    setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0 ||
		    setuid(0) == 0) {
			logmsg("session for %s: cannot drop privileges", pw->pw_name);
			_exit(1);
		}

		clearenv();
		for (i = 0; penv && penv[i]; i++)
			putenv(penv[i]);
		setenv("HOME", pw->pw_dir, 1);
		setenv("USER", pw->pw_name, 1);
		setenv("LOGNAME", pw->pw_name, 1);
		setenv("SHELL", pw->pw_shell && *pw->pw_shell ? pw->pw_shell : "/bin/sh", 1);
		setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 1);
		setenv("LANG", lang, 1);
		setenv("XDG_RUNTIME_DIR", rundir, 1);
		setenv("XDG_SESSION_TYPE", "wayland", 1);
		setenv("WESTON_RDP_CONTROL_SOCKET", ctl, 1);
		{
			char sam[128];

			snprintf(sam, sizeof sam, "%s/weston-rail-ntlm.sam", rundir);
			setenv("WESTON_RDP_NLA_SAM", sam, 1);
		}
		snprintf(idle, sizeof idle, "%d", cfg.idle_exit_sec);
		setenv("WESTON_RDP_IDLE_EXIT_SEC", idle, 1);

		if (chdir(pw->pw_dir) < 0 && chdir("/") < 0)
			_exit(1);
		fd = open(logpath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
		if (fd >= 0) {
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
		}
		fd = open("/dev/null", O_RDONLY);
		if (fd >= 0)
			dup2(fd, STDIN_FILENO);

		if (kind == SESSION_WEB) {
			/* one program, no RemoteApp: ordinary RDP, which every
			 * client understands (e.g. guacd for the browser) */
			setenv("WESTON_RDP_WEB_SESSION", "1", 1);
			if (app_command && *app_command)
				setenv("WESTON_AUTOLAUNCH", app_command, 1);
		}
		snprintf(cert_opt, sizeof cert_opt, "--rdp-tls-cert=%s", certpath);
		snprintf(key_opt, sizeof key_opt, "--rdp-tls-key=%s", keypath);
		snprintf(log_opt, sizeof log_opt, "--log=%s", logpath);
		if (access("/usr/bin/dbus-run-session", X_OK) == 0) {
			argv[argc++] = "/usr/bin/dbus-run-session";
			argv[argc++] = "--";
		}
		argv[argc++] = cfg.weston;
		argv[argc++] = "--backend=rdp-backend.so";
		if (kind == SESSION_WEB) {
			argv[argc++] = shell && !strcmp(shell, "desktop") ?
				"--shell=desktop-shell.so" : "--shell=kiosk-shell.so";
			snprintf(wayland_socket, sizeof wayland_socket,
				 "--socket=weston-rail-web-%s", app && *app ? app : "sitzung");
			argv[argc++] = wayland_socket;
			if (config_opt[0])
				argv[argc++] = config_opt;
		} else {
			argv[argc++] = "--shell=rdprail-shell.so";
		}
		argv[argc++] = "--logger-scopes=log,rdp-backend,rdprail-shell";
		/* X11 only programs (Java/Swing, older toolkits) need Xwayland;
		 * weston then exports DISPLAY to the programs it starts */
		if (cfg.xwayland)
			argv[argc++] = "--xwayland";
		argv[argc++] = log_opt;
		if (access(certpath, R_OK) == 0 && access(keypath, R_OK) == 0) {
			argv[argc++] = cert_opt;
			argv[argc++] = key_opt;
		}
		argv[argc] = NULL;
		execv(argv[0], argv);
		logmsg("session for %s: cannot start %s: %s", pw->pw_name, argv[0],
		       strerror(errno));
		_exit(127);
	}

	if (pid > 0) {
		keeper_child = pid;
		setpgid(pid, pid);	/* also from here, avoids a race */
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
			;
	}
	unlink(ctl);
	{
		char path[160];

		snprintf(path, sizeof path, "%s/weston-rail-ntlm.sam", rundir);
		if (kind == SESSION_RAIL)
			unlink(path);
		if (ini_path[0])
			unlink(ini_path);
	}
	unlink(certpath);
	unlink(keypath);
	if (pam_pending) {
		/* the late PAM session: wait for it before closing */
		pthread_join(pam_thread, NULL);
		session_open = true;
	}
	if (pamh) {
		if (session_open)
			pam_close_session(pamh, PAM_SILENT);
		pam_setcred(pamh, PAM_DELETE_CRED);
		pam_end(pamh, PAM_SUCCESS);
	}
	_exit(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
}

static struct session *
session_find_locked(const char *user, enum session_kind kind, const char *app)
{
	struct session *s;

	for (s = sessions; s; s = s->next)
		if (!strcmp(s->user, user) && s->kind == kind &&
		    (kind != SESSION_WEB || !strcmp(s->app, app ? app : "")))
			return s;
	return NULL;
}

/* returns the control socket path of the user's (possibly new) weston */
static bool
session_get_kind(const char *user, enum session_kind kind, const char *app,
		 const char *app_command, const char *shell, char ctl[108])
{
	struct passwd pwbuf, *pw = NULL;
	char buf[4096];
	struct session *s;
	pid_t pid;

	pthread_mutex_lock(&lock);
	s = session_find_locked(user, kind, app);
	if (s) {
		memcpy(ctl, s->ctl, 108);
		pthread_mutex_unlock(&lock);
		return true;
	}

	if (getpwnam_r(user, &pwbuf, buf, sizeof buf, &pw) != 0 || !pw) {
		pthread_mutex_unlock(&lock);
		logmsg("no such user '%s'", user);
		return false;
	}
	if (pw->pw_uid == 0 && !cfg.allow_root) {
		pthread_mutex_unlock(&lock);
		logmsg("sessions for root are not allowed (--allow-root)");
		return false;
	}

	s = calloc(1, sizeof *s);
	if (!s) {
		pthread_mutex_unlock(&lock);
		return false;
	}
	snprintf(s->user, sizeof s->user, "%s", user);
	s->uid = pw->pw_uid;
	s->started = time(NULL);
	s->kind = kind;
	snprintf(s->app, sizeof s->app, "%s", app ? app : "");
	if (kind == SESSION_WEB)
		snprintf(s->ctl, sizeof s->ctl, "/run/user/%u/weston-rail-web-%s.sock",
			 (unsigned)pw->pw_uid, s->app);
	else
		snprintf(s->ctl, sizeof s->ctl, "/run/user/%u/weston-rail.sock",
			 (unsigned)pw->pw_uid);

	pid = fork();
	if (pid == 0)
		session_keeper(pw, s->ctl, kind, s->app, app_command, shell);	/* no return */
	if (pid < 0) {
		pthread_mutex_unlock(&lock);
		free(s);
		return false;
	}
	s->pid = pid;
	s->next = sessions;
	sessions = s;
	memcpy(ctl, s->ctl, 108);
	pthread_mutex_unlock(&lock);
	if (kind == SESSION_WEB)
		logmsg("web session for %s (%s) started (uid %u)", user, s->app,
		       (unsigned)pw->pw_uid);
	else
		logmsg("session for %s started (uid %u)", user, (unsigned)pw->pw_uid);
	return true;
}

static bool
session_get(const char *user, char ctl[108])
{
	return session_get_kind(user, SESSION_RAIL, NULL, NULL, NULL, ctl);
}

static void *
reaper_thread(void *data)
{
	(void)data;
	for (;;) {
		int status;
		pid_t pid = waitpid(-1, &status, 0);

		if (pid < 0) {
			sleep(1);
			continue;
		}
		pthread_mutex_lock(&lock);
		for (struct session **pp = &sessions, *s; (s = *pp); pp = &s->next) {
			if (s->pid == pid) {
				logmsg("session for %s ended", s->user);
				*pp = s->next;
				free(s);
				break;
			}
		}
		pthread_mutex_unlock(&lock);
	}
	return NULL;
}

/* hand the TCP connection to the user's weston */
static bool
send_fd(const char *ctl, int fd)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	union {
		struct cmsghdr hdr;
		char buf[CMSG_SPACE(sizeof(int))];
	} c;
	char byte = 'F', reply[8];
	struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
	struct msghdr msg = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = c.buf, .msg_controllen = sizeof c.buf,
	};
	struct cmsghdr *cmsg;
	time_t deadline = time(NULL) + HANDOFF_WAIT_SEC;
	int s = -1;

	snprintf(addr.sun_path, sizeof addr.sun_path, "%s", ctl);
	/* a freshly started weston needs a moment to create the socket */
	while (time(NULL) < deadline) {
		s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (s < 0)
			return false;
		if (connect(s, (struct sockaddr *)&addr, sizeof addr) == 0)
			break;
		close(s);
		s = -1;
		usleep(200 * 1000);
	}
	if (s < 0)
		return false;

	memset(c.buf, 0, sizeof c.buf);
	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
	if (sendmsg(s, &msg, 0) != 1) {
		close(s);
		return false;
	}
	if (read(s, reply, sizeof reply) < 2 || strncmp(reply, "OK", 2) != 0) {
		close(s);
		return false;
	}
	close(s);
	return true;
}


static bool
nt_hash_hex(const char *password, char out[33])
{
	WCHAR *wide;
	size_t wlen = 0;
	BYTE hash[16];
	bool ok;
	int i;

	out[0] = '\0';
	wide = ConvertUtf8ToWCharAlloc(password, &wlen);
	if (!wide)
		return false;
	ok = NTOWFv1W(wide, (UINT32)(wlen * sizeof(WCHAR)), hash);
	memset(wide, 0, wlen * sizeof(WCHAR));
	free(wide);
	if (!ok)
		return false;
	for (i = 0; i < 16; i++)
		sprintf(out + 2 * i, "%02X", hash[i]);
	memset(hash, 0, sizeof hash);
	return true;
}

/*
 * The user's own NT hash for NLA on the reconnect, in the private runtime
 * directory. Written to a temporary name and renamed: rename() replaces a
 * symlink the user may have placed instead of following it.
 */
static void
write_session_sam(const struct login *login)
{
	struct passwd pwbuf, *pw = NULL;
	char buf[4096], path[128], tmp[160], line[256];
	int fd;

	if (!login->nthash[0] || !login->client_user[0] ||
	    strchr(login->client_user, ':') || strchr(login->client_user, '\n'))
		return;
	if (getpwnam_r(login->user, &pwbuf, buf, sizeof buf, &pw) != 0 || !pw)
		return;
	snprintf(path, sizeof path, "/run/user/%u/weston-rail-ntlm.sam", (unsigned)pw->pw_uid);
	snprintf(tmp, sizeof tmp, "%s.%d", path, (int)getpid());
	unlink(tmp);
	fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return;
	/*
	 * NTLM looks the user up by the name the client sends, which depends
	 * on how it was typed: "lars", "lars@realm" (then the user part may be
	 * sent alone) or "DOMAIN\lars". Write every variant, all with the
	 * user's own hash; the domain is left empty (WinPR falls back to it).
	 */
	{
		char variants[4][128];
		int nv = 0, v;
		const char *cu = login->client_user;
		const char *at = strchr(cu, '@'), *bs = strrchr(cu, '\\');

		snprintf(variants[nv++], sizeof variants[0], "%s", bs ? bs + 1 : cu);
		if (at && !bs)
			snprintf(variants[nv++], sizeof variants[0], "%.*s", (int)(at - cu), cu);
		if (strcmp(variants[0], login->user) && (nv < 2 || strcmp(variants[1], login->user)))
			snprintf(variants[nv++], sizeof variants[0], "%s", login->user);
		for (v = 0; v < nv; v++) {
			snprintf(line, sizeof line, "%s:::%s:::\n", variants[v], login->nthash);
			if (write(fd, line, strlen(line)) != (ssize_t)strlen(line)) {
				close(fd);
				unlink(tmp);
				return;
			}
		}
	}
	if (fchown(fd, pw->pw_uid, pw->pw_gid) < 0) {
		close(fd);
		unlink(tmp);
		return;
	}
	close(fd);
	if (rename(tmp, path) < 0)
		unlink(tmp);
}

static void
handoff(int fd, const struct login *login)
{
	char ctl[108];

	if (!session_get(login->user, ctl)) {
		logmsg("could not start a session for %s", login->user);
	} else {
		write_session_sam(login);
		if (!send_fd(ctl, fd))
			logmsg("could not hand the connection of %s to the session", login->user);
		else
			logmsg("connection of %s handed to the session", login->user);
	}
	close(fd);
}

/* ------------------------------------------------------------------ */
/* authentication phase (FreeRDP server)                               */
/* ------------------------------------------------------------------ */

struct auth_ctx {
	rdpContext _p;
	char rhost[64];
	char local_addr[64];
	bool redirected;
	bool failed;
	char nla_user[64];	/* Linux user authenticated by NLA */
};

static void
split_user(const char *in, char *out, size_t size)
{
	const char *p = strrchr(in, '\\');	/* DOMAIN\user */

	if (p)
		in = p + 1;
	snprintf(out, size, "%s", in);
	out[strcspn(out, "@")] = '\0';		/* user@domain */
}


/* NLA user and domain -> Linux account name */
static void
map_nla_user(const char *user, const char *domain, char *out, size_t size)
{
	char u[128], d[128];
	const char *at;

	snprintf(u, sizeof u, "%s", user);
	snprintf(d, sizeof d, "%s", domain ? domain : "");
	/* "user@realm" typed as user name */
	at = strchr(u, '@');
	if (at && !*d) {
		snprintf(d, sizeof d, "%s", at + 1);
		u[at - u] = '\0';
	}
	switch (cfg.user_map) {
	case USER_MAP_UPN:
		if (*d)
			snprintf(out, size, "%s@%s", u, d);
		else
			snprintf(out, size, "%s", u);
		break;
	case USER_MAP_NETBIOS:
		if (*d)
			snprintf(out, size, "%s\\%s", d, u);
		else
			snprintf(out, size, "%s", u);
		break;
	default:
		snprintf(out, size, "%s", u);
		break;
	}
}

static void
wide_to_utf8(const UINT16 *w, UINT32 len, char *out, size_t size)
{
	out[0] = '\0';
	if (w && len)
		if (ConvertWCharNToUtf8((const WCHAR *)w, len, out, size) < 0)
			out[0] = '\0';
}

/*
 * Called by FreeRDP once NLA (CredSSP) succeeded: NTLM against the NT
 * hash file, or Kerberos against the keytab. The client then delegates
 * its password (TSPasswordCreds), which is checked with PAM as well, so
 * locked or expired accounts are refused and the session gets a proper
 * PAM login.
 */
static BOOL
auth_logon(freerdp_peer *peer, const SEC_WINNT_AUTH_IDENTITY *identity, BOOL automatic)
{
	struct auth_ctx *ctx = (struct auth_ctx *)peer->context;
	char user[128], domain[128], password[512], linux_user[160];
	BOOL ok = FALSE;

	if (!automatic || !identity)
		return TRUE;	/* not NLA: credentials follow in the Client Info PDU */

	if (ctx->nla_user[0])
		return TRUE;	/* FreeRDP may report the logon more than once */

	user[0] = domain[0] = password[0] = '\0';
	if (identity->UserLength) {
		if (identity->Flags & SEC_WINNT_AUTH_IDENTITY_UNICODE) {
			wide_to_utf8(identity->User, identity->UserLength, user, sizeof user);
			wide_to_utf8(identity->Domain, identity->DomainLength, domain, sizeof domain);
			wide_to_utf8(identity->Password, identity->PasswordLength, password,
				     sizeof password);
		} else {
			snprintf(user, sizeof user, "%.*s", (int)identity->UserLength,
				 (const char *)identity->User);
			snprintf(domain, sizeof domain, "%.*s", (int)identity->DomainLength,
				 (const char *)identity->Domain);
			snprintf(password, sizeof password, "%.*s", (int)identity->PasswordLength,
				 (const char *)identity->Password);
		}
	} else {
		/* FreeRDP 3 stores the delegated credentials (TSPasswordCreds)
		 * in the settings, the identity stays empty */
		rdpSettings *settings = peer->context->settings;
		const char *v;

		if ((v = freerdp_settings_get_string(settings, FreeRDP_Username)))
			snprintf(user, sizeof user, "%s", v);
		if ((v = freerdp_settings_get_string(settings, FreeRDP_Domain)))
			snprintf(domain, sizeof domain, "%s", v);
		if ((v = freerdp_settings_get_string(settings, FreeRDP_Password)))
			snprintf(password, sizeof password, "%s", v);
	}
	map_nla_user(user, domain, linux_user, sizeof linux_user);

	if (!*password) {
		/* e.g. Restricted Admin / Remote Credential Guard: no password */
		logmsg("NLA user %s from %s delegated no password, refused", linux_user,
		       ctx->rhost);
	} else if (!valid_username(linux_user) &&
		   cfg.user_map == USER_MAP_PLAIN) {
		logmsg("NLA user '%s' from %s: invalid user name", linux_user, ctx->rhost);
	} else if (pam_check(linux_user, password, ctx->rhost)) {
		snprintf(ctx->nla_user, sizeof ctx->nla_user, "%s", linux_user);
		logmsg("user %s authenticated with NLA from %s", linux_user, ctx->rhost);
		ok = TRUE;
	}
	memset(password, 0, sizeof password);
	if (!ok)
		ctx->failed = true;
	return ok;
}

static BOOL
auth_post_connect(freerdp_peer *peer)
{
	struct auth_ctx *ctx = (struct auth_ctx *)peer->context;
	rdpSettings *settings = peer->context->settings;
	const char *u = freerdp_settings_get_string(settings, FreeRDP_Username);
	const char *pw = freerdp_settings_get_string(settings, FreeRDP_Password);
	char user[64], token[TOKEN_HEX_LEN + 1], lb[64];
	rdpRedirection *r;
	UINT32 missing = 0;
	BOOL ok;

	if (ctx->nla_user[0]) {
		/* already authenticated by NLA + PAM (auth_logon) */
		snprintf(user, sizeof user, "%s", ctx->nla_user);
	} else if (freerdp_settings_get_bool(settings, FreeRDP_NlaSecurity)) {
		/* NLA was negotiated but did not produce a user */
		ctx->failed = true;
		return FALSE;
	} else {
		if (!cfg.tls_login) {
			logmsg("client %s: login without NLA refused (--nla-only)", ctx->rhost);
			ctx->failed = true;
			return FALSE;
		}
		if (!u || !*u || !pw || !*pw) {
			logmsg("client %s sent no credentials", ctx->rhost);
			ctx->failed = true;
			return FALSE;
		}
		split_user(u, user, sizeof user);
		if (!valid_username(user)) {
			logmsg("client %s: invalid user name", ctx->rhost);
			ctx->failed = true;
			return FALSE;
		}
		if (!pam_check(user, pw, ctx->rhost)) {
			ctx->failed = true;
			return FALSE;
		}
		logmsg("user %s authenticated from %s", user, ctx->rhost);
	}

	{
		struct login login = { 0 };
		const char *cu = freerdp_settings_get_string(settings, FreeRDP_Username);
		const char *cp = freerdp_settings_get_string(settings, FreeRDP_Password);

		snprintf(login.user, sizeof login.user, "%s", user);
		/* user name as the client sends it in NTLM (without domain) */
		if (cu && *cu) {
			const char *bs = strrchr(cu, '\\');

			snprintf(login.client_user, sizeof login.client_user, "%s",
				 bs ? bs + 1 : cu);
		} else {
			snprintf(login.client_user, sizeof login.client_user, "%s", user);
		}
		if (cp && *cp)
			nt_hash_hex(cp, login.nthash);
		if (!token_create(&login, token)) {
			memset(&login, 0, sizeof login);
			ctx->failed = true;
			return FALSE;
		}
		memset(&login, 0, sizeof login);
	}
	/* FreeRDP writes the load balance info as "Cookie: msts=<value>\r\n",
	 * which the client sends back verbatim as X.224 routing token */
	snprintf(lb, sizeof lb, "%s", token);

	r = redirection_new();
	if (!r) {
		ctx->failed = true;
		return FALSE;
	}
	/* no target address: the client reconnects to the address it used,
	 * i.e. this broker (MS-RDPBCGR passes the routing token only then) */
	redirection_set_flags(r, LB_LOAD_BALANCE_INFO | LB_USERNAME);
	redirection_set_session_id(r, 0);
	redirection_set_byte_option(r, LB_LOAD_BALANCE_INFO, (const BYTE *)lb, strlen(lb));
	redirection_set_string_option(r, LB_USERNAME, u && *u ? u : user);
	if (!redirection_settings_are_valid(r, &missing))
		debugmsg("redirection settings incomplete (0x%x)", missing);
	ok = peer->SendServerRedirection(peer, r);
	redirection_free(r);
	if (!ok) {
		logmsg("sending the redirection to %s failed", ctx->rhost);
		ctx->failed = true;
		return FALSE;
	}
	debugmsg("redirected %s with a routing token", user);
	ctx->redirected = true;
	return TRUE;
}

static BOOL
auth_activate(freerdp_peer *peer)
{
	(void)peer;
	return TRUE;
}

static bool
load_tls(rdpSettings *settings)
{
	rdpCertificate *cert = freerdp_certificate_new_from_file(cfg.cert);
	rdpPrivateKey *key = freerdp_key_new_from_file(cfg.key);

	if (!cert || !key) {
		if (cert)
			freerdp_certificate_free(cert);
		if (key)
			freerdp_key_free(key);
		return false;
	}
	if (!freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerCertificate, cert, 1)) {
		freerdp_certificate_free(cert);
		freerdp_key_free(key);
		return false;
	}
	if (!freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerRsaKey, key, 1)) {
		freerdp_key_free(key);
		return false;
	}
	return true;
}

static void
addr_to_string(const struct sockaddr_storage *ss, char *out, size_t size)
{
	char buf[INET6_ADDRSTRLEN] = "?";

	if (ss->ss_family == AF_INET)
		inet_ntop(AF_INET, &((const struct sockaddr_in *)ss)->sin_addr, buf, sizeof buf);
	else if (ss->ss_family == AF_INET6)
		inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)ss)->sin6_addr, buf, sizeof buf);
	/* IPv4 clients on the dual-stack socket */
	if (!strncmp(buf, "::ffff:", 7) && strchr(buf + 7, '.'))
		memmove(buf, buf + 7, strlen(buf + 7) + 1);
	snprintf(out, size, "%s", buf);
}

/* NLA needs something to check the client's proof against */
static bool
nla_available(void)
{
	return cfg.nla && (access(cfg.sam, R_OK) == 0 || access(cfg.keytab, R_OK) == 0);
}

static void
authenticate(int fd)
{
	struct sockaddr_storage ss;
	socklen_t sl = sizeof ss;
	freerdp_peer *peer;
	struct auth_ctx *ctx;
	rdpSettings *settings;
	time_t deadline = time(NULL) + AUTH_TIMEOUT_SEC, redirected_at = 0;

	peer = freerdp_peer_new(fd);
	if (!peer) {
		close(fd);
		return;
	}
	peer->ContextSize = sizeof(struct auth_ctx);
	if (!freerdp_peer_context_new(peer)) {
		freerdp_peer_free(peer);
		return;
	}
	ctx = (struct auth_ctx *)peer->context;
	settings = peer->context->settings;

	if (getpeername(fd, (struct sockaddr *)&ss, &sl) == 0)
		addr_to_string(&ss, ctx->rhost, sizeof ctx->rhost);
	sl = sizeof ss;
	if (getsockname(fd, (struct sockaddr *)&ss, &sl) == 0)
		addr_to_string(&ss, ctx->local_addr, sizeof ctx->local_addr);

	if (!load_tls(settings)) {
		logmsg("cannot load %s / %s", cfg.cert, cfg.key);
		goto out;
	}
	freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE);
	freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, cfg.tls_login);
	freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, nla_available());
	if (nla_available()) {
		if (access(cfg.sam, R_OK) == 0)
			freerdp_settings_set_string(settings, FreeRDP_NtlmSamFile, cfg.sam);
		if (access(cfg.keytab, R_OK) == 0)
			freerdp_settings_set_string(settings, FreeRDP_KerberosKeytab, cfg.keytab);
	}
	freerdp_settings_set_uint32(settings, FreeRDP_OsMajorType, OSMAJORTYPE_UNIX);
	freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);
	/* a RemoteApp client must see a RemoteApp capable server */
	freerdp_settings_set_bool(settings, FreeRDP_RemoteApplicationMode, TRUE);
	freerdp_settings_set_uint32(settings, FreeRDP_RemoteApplicationSupportLevel,
				    RAIL_LEVEL_SUPPORTED | RAIL_LEVEL_HANDSHAKE_EX_SUPPORTED);
	freerdp_settings_set_uint32(settings, FreeRDP_RemoteWndSupportLevel,
				    WINDOW_LEVEL_SUPPORTED_EX);
	freerdp_settings_set_uint32(settings, FreeRDP_RemoteAppNumIconCaches, 3);
	freerdp_settings_set_uint32(settings, FreeRDP_RemoteAppNumIconCacheEntries, 12);
	freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, TRUE);

	peer->Logon = auth_logon;
	peer->PostConnect = auth_post_connect;
	peer->Activate = auth_activate;

	if (!peer->Initialize(peer)) {
		logmsg("client %s: initialization failed", ctx->rhost);
		goto out;
	}

	while (time(NULL) < deadline) {
		HANDLE handles[32];
		DWORD n = peer->GetEventHandles(peer, handles, 32);

		if (n == 0)
			break;
		WaitForMultipleObjects(n, handles, FALSE, 1000);
		if (!peer->CheckFileDescriptor(peer))
			break;
		if (ctx->failed)
			break;
		if (ctx->redirected) {
			/* the client disconnects itself after the redirection */
			if (!redirected_at)
				redirected_at = time(NULL);
			else if (time(NULL) - redirected_at > 10)
				break;
		}
	}
out:
	peer->Disconnect(peer);
	freerdp_peer_context_free(peer);
	freerdp_peer_free(peer);
}


/* ------------------------------------------------------------------ */
/* admin interface: /run/weston-rail-broker.sock (root only)           */
/* ------------------------------------------------------------------ */

#define ADMIN_SOCKET "/run/weston-rail-broker.sock"

/* ask a session's weston: client connected? programs running? */
static bool
session_status(const char *ctl, int *connected, int *apps)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	struct timeval tv = { .tv_sec = 1 };
	char byte = 'S', reply[64];
	int s;
	ssize_t n;

	*connected = *apps = -1;
	snprintf(addr.sun_path, sizeof addr.sun_path, "%s", ctl);
	s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (s < 0)
		return false;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	if (connect(s, (struct sockaddr *)&addr, sizeof addr) < 0 ||
	    write(s, &byte, 1) != 1) {
		close(s);
		return false;
	}
	n = read(s, reply, sizeof reply - 1);
	close(s);
	if (n <= 0)
		return false;
	reply[n] = '\0';
	return sscanf(reply, "STATUS connected=%d apps=%d", connected, apps) == 2;
}

static void
admin_reply(int fd, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int len;

	va_start(ap, fmt);
	len = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (len > 0 && write(fd, buf, (size_t)len) < 0)
		return;
}

static void
admin_list(int fd)
{
	struct snap {
		char user[64];
		unsigned uid;
		int pid;
		long started;
		char ctl[108];
	} snaps[256];
	struct session *s;
	int n = 0, i;

	pthread_mutex_lock(&lock);
	for (s = sessions; s && n < 256; s = s->next, n++) {
		snprintf(snaps[n].user, sizeof snaps[n].user, "%s", s->user);
		snaps[n].uid = (unsigned)s->uid;
		snaps[n].pid = (int)s->pid;
		snaps[n].started = (long)s->started;
		memcpy(snaps[n].ctl, s->ctl, sizeof snaps[n].ctl);
	}
	pthread_mutex_unlock(&lock);

	/* user uid pid started(epoch) connected(1/0/-1) apps(-1 unknown) */
	for (i = 0; i < n; i++) {
		int connected, apps;

		session_status(snaps[i].ctl, &connected, &apps);
		admin_reply(fd, "%s\t%u\t%d\t%ld\t%d\t%d\n", snaps[i].user, snaps[i].uid,
			    snaps[i].pid, snaps[i].started, connected, apps);
	}
	admin_reply(fd, "END\n");
}

static void
admin_logoff(int fd, char *user)
{
	struct session *s;
	pid_t pid = 0;
	char *app = strstr(user, " app=");

	/* "LOGOFF <user>" beendet die RemoteApp-Sitzung,
	 * "LOGOFF <user> app=<name>" die Browser-Sitzung dieses Programms */
	if (app) {
		*app = '\0';
		app += 5;
	}
	if (!valid_username(user)) {
		admin_reply(fd, "ERR invalid user name\n");
		return;
	}

	pthread_mutex_lock(&lock);
	if (app)
		s = session_find_locked(user, SESSION_WEB, app);
	else
		s = session_find_locked(user, SESSION_RAIL, NULL);
	if (s)
		pid = s->pid;
	pthread_mutex_unlock(&lock);
	if (!pid) {
		admin_reply(fd, "ERR no session for %s\n", user);
		return;
	}
	logmsg("session for %s%s%s: logoff requested", user, app ? " " : "", app ? app : "");
	kill(pid, SIGTERM);	/* the keeper ends weston's process group */
	admin_reply(fd, "OK\n");
}


/*
 * Hand-over without redirection, for clients that cannot follow a Server
 * Redirection PDU (guacd/Apache Guacamole, see weston-rail-web).
 *
 * "CONNECT <user> [<uid>]" at the admin socket starts the user's session
 * if needed and opens a one-time listener on 127.0.0.1. The first
 * connection within 60 s is handed to that session, then the listener is
 * closed. With <uid> only that local user may connect (e.g. the uid guacd
 * runs as); without it any local process may, so the port is the secret.
 * The caller must already have authenticated the user - the admin socket
 * is root only.
 */
struct oneshot {
	char user[64];
	char app[64];
	char app_command[2048];
	char shell[16];
	enum session_kind kind;
	int fd;
	uid_t allow_uid;
	bool restrict_uid;
};

static void *
oneshot_thread(void *data)
{
	struct oneshot *o = data;
	struct pollfd pfd = { .fd = o->fd, .events = POLLIN };
	char ctl[108];
	int conn = -1;

	if (poll(&pfd, 1, 60 * 1000) > 0)
		conn = accept4(o->fd, NULL, NULL, SOCK_CLOEXEC);
	close(o->fd);
	if (conn < 0) {
		logmsg("direct connection for %s: nobody connected", o->user);
		free(o);
		return NULL;
	}
	if (o->restrict_uid) {
		struct ucred cred;
		socklen_t cl = sizeof cred;

		if (getsockopt(conn, SOL_SOCKET, SO_PEERCRED, &cred, &cl) < 0 ||
		    (cred.uid != o->allow_uid && cred.uid != 0)) {
			logmsg("direct connection for %s refused (uid %u)", o->user,
			       (unsigned)cred.uid);
			close(conn);
			free(o);
			return NULL;
		}
	}
	if (session_get_kind(o->user, o->kind, o->app, o->app_command, o->shell, ctl) &&
	    send_fd(ctl, conn))
		logmsg("direct connection of %s handed to the session", o->user);
	else
		logmsg("could not hand the direct connection of %s to the session", o->user);
	close(conn);
	free(o);
	return NULL;
}

static void
admin_connect(int fd, char *args)
{
	struct sockaddr_in addr = { .sin_family = AF_INET };
	socklen_t len = sizeof addr;
	struct oneshot *o;
	/* desktop: kiosk-shell liefert in dieser Betriebsart keine
	 * Mausereignisse an die Anwendung */
	char ctl[108], command[2048] = "", shell[16] = "desktop", app[64] = "";
	char *user = args, *tok, *save = NULL;
	enum session_kind kind = SESSION_RAIL;
	uid_t allow_uid = 0;
	bool restrict_uid = false;
	pthread_t tid;
	int lfd;

	/* CONNECT <user> [uid=N] [app=NAME] [shell=kiosk|desktop] */
	tok = strchr(args, ' ');
	if (tok)
		*tok++ = '\0';
	if (!valid_username(user)) {
		admin_reply(fd, "ERR invalid user name\n");
		return;
	}
	for (tok = tok ? strtok_r(tok, " ", &save) : NULL; tok;
	     tok = strtok_r(NULL, " ", &save)) {
		if (!strncmp(tok, "uid=", 4)) {
			allow_uid = (uid_t)strtoul(tok + 4, NULL, 10);
			restrict_uid = true;
		} else if (!strncmp(tok, "app=", 4)) {
			if (!lookup_app(tok + 4, command, sizeof command)) {
				admin_reply(fd, "ERR '%s' is not published\n", tok + 4);
				return;
			}
			snprintf(app, sizeof app, "%s", tok + 4);
			kind = SESSION_WEB;
		} else if (!strncmp(tok, "shell=", 6)) {
			snprintf(shell, sizeof shell, "%s", tok + 6);
		} else if (!strchr(tok, '=')) {
			/* old form: CONNECT <user> <uid> */
			allow_uid = (uid_t)strtoul(tok, NULL, 10);
			restrict_uid = true;
		}
	}

	/* start the session now, so the client finds it right away */
	if (!session_get_kind(user, kind, app, command, shell, ctl)) {
		admin_reply(fd, "ERR no session for %s\n", user);
		return;
	}

	lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (lfd < 0 || bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0 ||
	    listen(lfd, 1) < 0 || getsockname(lfd, (struct sockaddr *)&addr, &len) < 0) {
		if (lfd >= 0)
			close(lfd);
		admin_reply(fd, "ERR cannot open a port\n");
		return;
	}

	o = calloc(1, sizeof *o);
	if (!o) {
		close(lfd);
		admin_reply(fd, "ERR out of memory\n");
		return;
	}
	snprintf(o->user, sizeof o->user, "%s", user);
	snprintf(o->app, sizeof o->app, "%s", app);
	snprintf(o->app_command, sizeof o->app_command, "%s", command);
	snprintf(o->shell, sizeof o->shell, "%s", shell);
	o->kind = kind;
	o->fd = lfd;
	o->allow_uid = allow_uid;
	o->restrict_uid = restrict_uid;
	if (pthread_create(&tid, NULL, oneshot_thread, o) != 0) {
		close(lfd);
		free(o);
		admin_reply(fd, "ERR cannot start the handover\n");
		return;
	}
	pthread_detach(tid);
	logmsg("direct connection for %s (%s): waiting on 127.0.0.1:%u", user,
	       kind == SESSION_WEB ? app : "RemoteApp", (unsigned)ntohs(addr.sin_port));
	admin_reply(fd, "PORT %u\n", (unsigned)ntohs(addr.sin_port));
}

static void *
admin_thread(void *data)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int lfd;

	(void)data;
	unlink(ADMIN_SOCKET);
	lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	snprintf(addr.sun_path, sizeof addr.sun_path, "%s", ADMIN_SOCKET);
	if (lfd < 0 || bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0 ||
	    listen(lfd, 8) < 0) {
		logmsg("admin interface unavailable: %s", strerror(errno));
		return NULL;
	}
	chmod(ADMIN_SOCKET, 0600);

	for (;;) {
		struct ucred cred;
		socklen_t cl = sizeof cred;
		struct timeval tv = { .tv_sec = 5 };
		char line[256];
		ssize_t n;
		int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);

		if (fd < 0)
			continue;
		if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cl) < 0 || cred.uid != 0) {
			close(fd);
			continue;
		}
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
		n = read(fd, line, sizeof line - 1);
		if (n > 0) {
			char *password;

			line[n] = '\0';
			/* "AUTH <user>\n<password>\n": check a login for
			 * weston-rail-web, so PAM stays in one place */
			password = strchr(line, '\n');
			if (password)
				*password++ = '\0';
			line[strcspn(line, "\r\n")] = '\0';
			if (!strncmp(line, "AUTH ", 5)) {
				char rhost[64] = "";
				const char *user = line + 5;

				if (password)
					password[strcspn(password, "\r\n")] = '\0';
				if (valid_username(user) && password && *password &&
				    pam_check(user, password, rhost[0] ? rhost : NULL))
					admin_reply(fd, "OK\n");
				else
					admin_reply(fd, "ERR login refused\n");
				if (password)
					memset(password, 0, strlen(password));
			}
			else if (!strcmp(line, "LIST"))
				admin_list(fd);
			else if (!strncmp(line, "LOGOFF ", 7))
				admin_logoff(fd, line + 7);
			else if (!strncmp(line, "CONNECT ", 8))
				admin_connect(fd, line + 8);
			else
				admin_reply(fd, "ERR unknown command\n");
		}
		close(fd);
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* connection dispatch                                                 */
/* ------------------------------------------------------------------ */

/*
 * Peek at the X.224 connection request without consuming it. A routing
 * token "Cookie: msts=<token>" marks the second connection after the
 * redirection.
 */
static bool
peek_token(int fd, char token[TOKEN_HEX_LEN + 1])
{
	unsigned char buf[1024];
	size_t need = 4, have = 0, i;
	time_t deadline = time(NULL) + 10;
	const char *p;

	while (have < need && time(NULL) < deadline) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		ssize_t n;

		if (poll(&pfd, 1, 1000) <= 0)
			continue;
		n = recv(fd, buf, sizeof buf, MSG_PEEK);
		if (n <= 0)
			return false;
		have = (size_t)n;
		if (have >= 4 && buf[0] == 3)	/* TPKT */
			need = ((size_t)buf[2] << 8 | buf[3]);
		if (need > sizeof buf)
			need = sizeof buf;
		if (have < need)
			usleep(20 * 1000);
	}
	if (cfg.verbose) {
		char dump[200];
		size_t k, m = 0;

		for (k = 0; k < have && m + 5 < sizeof dump; k++)
			m += (size_t)snprintf(dump + m, sizeof dump - m,
					      (buf[k] >= 32 && buf[k] < 127) ? "%c" : "\\x%02x", buf[k]);
		debugmsg("connection request (%zu bytes): %s", have, dump);
	}
	if (have < 11)
		return false;

	/* search "msts=" in the variable part of the X.224 CR */
	for (i = 0; i + 5 < have; i++) {
		if (memcmp(buf + i, "msts=", 5) != 0)
			continue;
		p = (const char *)buf + i + 5;
		if (i + 5 + TOKEN_HEX_LEN > have)
			return false;
		memcpy(token, p, TOKEN_HEX_LEN);
		token[TOKEN_HEX_LEN] = '\0';
		for (int k = 0; k < TOKEN_HEX_LEN; k++)
			if (!strchr("0123456789abcdef", token[k]))
				return false;
		return true;
	}
	return false;
}

static void *
connection_thread(void *data)
{
	int fd = (int)(intptr_t)data;
	char token[TOKEN_HEX_LEN + 1];
	struct login login;

	if (peek_token(fd, token)) {
		if (token_take(token, &login)) {
			handoff(fd, &login);
			memset(&login, 0, sizeof login);
			return NULL;
		}
		logmsg("unknown or expired routing token, authenticating again");
	}
	authenticate(fd);
	return NULL;
}

/* ------------------------------------------------------------------ */

static void
san_add(char *san, size_t size, const char *type, const char *value)
{
	char entry[300];

	if (!value || !*value)
		return;
	snprintf(entry, sizeof entry, "%s:%s", type, value);
	/* no duplicates */
	if (strstr(san, entry))
		return;
	if (*san)
		strncat(san, ",", size - strlen(san) - 1);
	strncat(san, entry, size - strlen(san) - 1);
}

/*
 * Self-signed certificate valid for every name the server is reached by:
 * host name, fully qualified name, all local IP addresses and the names
 * given with --name (e.g. an external DNS name). mstsc checks the name it
 * connected to against the subjectAltName entries.
 */
static bool
ensure_certificate(void)
{
	char hostname[256] = "weston-rail", subj[300], san[4096] = "", ext[4200];
	struct addrinfo hints = { .ai_flags = AI_CANONNAME }, *ai = NULL;
	struct ifaddrs *ifa = NULL, *i;
	pid_t pid;
	int status, k;

	if (access(cfg.cert, R_OK) == 0 && access(cfg.key, R_OK) == 0)
		return true;

	gethostname(hostname, sizeof hostname - 1);
	san_add(san, sizeof san, "DNS", hostname);
	if (getaddrinfo(hostname, NULL, &hints, &ai) == 0 && ai && ai->ai_canonname)
		san_add(san, sizeof san, "DNS", ai->ai_canonname);
	if (ai)
		freeaddrinfo(ai);
	for (k = 0; k < cfg.n_names; k++)
		san_add(san, sizeof san, "DNS", cfg.names[k]);
	if (getifaddrs(&ifa) == 0) {
		for (i = ifa; i; i = i->ifa_next) {
			char buf[INET6_ADDRSTRLEN];

			if (!i->ifa_addr)
				continue;
			if (i->ifa_addr->sa_family == AF_INET)
				inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr,
					  buf, sizeof buf);
			else if (i->ifa_addr->sa_family == AF_INET6 &&
				 !IN6_IS_ADDR_LINKLOCAL(&((struct sockaddr_in6 *)i->ifa_addr)->sin6_addr))
				inet_ntop(AF_INET6, &((struct sockaddr_in6 *)i->ifa_addr)->sin6_addr,
					  buf, sizeof buf);
			else
				continue;
			san_add(san, sizeof san, "IP", buf);
		}
		freeifaddrs(ifa);
	}

	/* the name users type first, so that it shows up as the subject */
	snprintf(subj, sizeof subj, "/CN=%s", cfg.n_names ? cfg.names[0] : hostname);
	snprintf(ext, sizeof ext, "subjectAltName=%s", san);
	logmsg("creating a self-signed certificate %s for %s", cfg.cert, san);
	mkdir("/etc/weston-rail", 0755);
	pid = fork();
	if (pid == 0) {
		int devnull = open("/dev/null", O_WRONLY);

		if (devnull >= 0) {
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
		}
		umask(077);
		execlp("openssl", "openssl", "req", "-x509", "-newkey", "rsa:2048",
		       "-nodes", "-days", "3650", "-subj", subj,
		       "-addext", ext,
		       "-addext", "extendedKeyUsage=serverAuth",
		       "-keyout", cfg.key, "-out", cfg.cert, (char *)NULL);
		_exit(127);
	}
	if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0) {
		logmsg("cannot create the certificate (is openssl installed?)");
		return false;
	}
	chmod(cfg.key, 0600);
	chmod(cfg.cert, 0644);
	return true;
}

static void
usage(void)
{
	fprintf(stderr,
		"usage: weston-rail-broker [options]\n"
		"  -p, --port=PORT        RDP port (3389)\n"
		"  -c, --cert=FILE        TLS certificate (/etc/weston-rail/tls.crt)\n"
		"  -k, --key=FILE         TLS private key (/etc/weston-rail/tls.key)\n"
		"  -w, --weston=PATH      weston binary (%s)\n"
		"  -i, --idle-exit=SEC    end a session this long after the last client\n"
		"                         and the last application are gone (60)\n"
		"  -n, --name=DNSNAME     extra name for the generated certificate, e.g. the\n"
		"                         external DNS name (repeatable; delete\n"
		"                         /etc/weston-rail/tls.* to regenerate)\n"
		"      --sam=FILE         NT hashes for NLA (/etc/weston-rail/ntlm.sam,\n"
		"                         maintained with weston-rail-passwd)\n"
		"      --keytab=FILE      Kerberos keytab for NLA in an AD domain\n"
		"                         (/etc/weston-rail/krb5.keytab)\n"
		"      --user-map=MODE    NLA user -> Linux account: plain (lars),\n"
		"                         upn (lars@realm) or netbios (DOMAIN\\lars)\n"
		"      --no-nla           do not offer NLA\n"
		"      --nla-only         refuse logins without NLA\n"
		"      --no-xwayland      start the sessions without Xwayland (X11 programs\n"
		"                         such as Java/Swing then do not work)\n"
		"      --allow-root       allow sessions for root\n"
		"  -v, --verbose\n", WESTON_BINARY);
}

int
main(int argc, char *argv[])
{
	static const struct option opts[] = {
		{ "port", required_argument, NULL, 'p' },
		{ "cert", required_argument, NULL, 'c' },
		{ "key", required_argument, NULL, 'k' },
		{ "weston", required_argument, NULL, 'w' },
		{ "idle-exit", required_argument, NULL, 'i' },
		{ "allow-root", no_argument, NULL, 'R' },
		{ "no-xwayland", no_argument, NULL, 'X' },
		{ "name", required_argument, NULL, 'n' },
		{ "sam", required_argument, NULL, 'S' },
		{ "keytab", required_argument, NULL, 'K' },
		{ "user-map", required_argument, NULL, 'M' },
		{ "no-nla", no_argument, NULL, 'N' },
		{ "nla-only", no_argument, NULL, 'O' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	struct sockaddr_in6 addr = { .sin6_family = AF_INET6 };
	pthread_t tid;
	int lfd, on = 1, off = 0, opt;
	bool bound = false;

	while ((opt = getopt_long(argc, argv, "p:c:k:w:i:n:vh", opts, NULL)) != -1) {
		switch (opt) {
		case 'p': cfg.port = atoi(optarg); break;
		case 'c': cfg.cert = optarg; break;
		case 'k': cfg.key = optarg; break;
		case 'w': cfg.weston = optarg; break;
		case 'i': cfg.idle_exit_sec = atoi(optarg); break;
		case 'R': cfg.allow_root = true; break;
		case 'X': cfg.xwayland = false; break;
		case 'S': cfg.sam = optarg; break;
		case 'K': cfg.keytab = optarg; break;
		case 'M':
			if (!strcmp(optarg, "upn"))
				cfg.user_map = USER_MAP_UPN;
			else if (!strcmp(optarg, "netbios"))
				cfg.user_map = USER_MAP_NETBIOS;
			else
				cfg.user_map = USER_MAP_PLAIN;
			break;
		case 'N': cfg.nla = false; break;
		case 'O': cfg.tls_login = false; break;
		case 'n':
			if (cfg.n_names < 16)
				cfg.names[cfg.n_names++] = optarg;
			break;
		case 'v': cfg.verbose = true; break;
		default: usage(); return opt == 'h' ? 0 : 2;
		}
	}
	if (geteuid() != 0) {
		fprintf(stderr, "weston-rail-broker must run as root\n");
		return 1;
	}
	signal(SIGPIPE, SIG_IGN);
	setvbuf(stderr, NULL, _IOLBF, 0);

	/* systemd starts services without HOME; FreeRDP derives its
	 * configuration path from it and refuses to create a peer context */
	if (!getenv("HOME") || !*getenv("HOME")) {
		struct passwd *pw = getpwuid(geteuid());

		setenv("HOME", pw && pw->pw_dir ? pw->pw_dir : "/root", 1);
	}
	winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT);

	/*
	 * FreeRDP reports the normal end of the first connection after the
	 * redirection (ERRINFO_LOGOFF_BY_USER, BIO_read retries exceeded) as
	 * errors. The broker logs what matters itself; show FreeRDP's peer and
	 * transport messages only with --verbose (or an explicit WLOG_FILTER).
	 */
	if (!cfg.verbose && !getenv("WLOG_FILTER"))
		WLog_AddStringLogFilters("com.freerdp.core.peer:FATAL,"
					 "com.freerdp.core.transport:FATAL,"
					 "com.winpr.path.shell:ERROR");

	if (!ensure_certificate())
		return 1;
	if (access(cfg.weston, X_OK) != 0) {
		logmsg("weston not found at %s (--weston)", cfg.weston);
		return 1;
	}

	/* dual stack if IPv6 is available, IPv4 only otherwise (e.g. an
	 * LXC container without IPv6) */
	lfd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (lfd >= 0) {
		setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
		setsockopt(lfd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
		addr.sin6_addr = in6addr_any;
		addr.sin6_port = htons(cfg.port);
		bound = bind(lfd, (struct sockaddr *)&addr, sizeof addr) == 0;
		if (!bound) {
			close(lfd);
			lfd = -1;
		}
	}
	if (lfd < 0) {
		struct sockaddr_in addr4 = { .sin_family = AF_INET };

		lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (lfd < 0) {
			perror("socket");
			return 1;
		}
		setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
		addr4.sin_addr.s_addr = htonl(INADDR_ANY);
		addr4.sin_port = htons(cfg.port);
		bound = bind(lfd, (struct sockaddr *)&addr4, sizeof addr4) == 0;
	}
	if (!bound || listen(lfd, 32) < 0) {
		logmsg("cannot listen on port %d: %s", cfg.port, strerror(errno));
		return 1;
	}
	pthread_create(&tid, NULL, reaper_thread, NULL);
	pthread_detach(tid);
	pthread_create(&tid, NULL, admin_thread, NULL);
	pthread_detach(tid);
	logmsg("weston-rail-broker listening on port %d", cfg.port);
	logmsg("NLA: %s%s%s; login without NLA: %s",
	       !cfg.nla ? "off" : nla_available() ? "on" : "off (no NT hash file, no keytab)",
	       nla_available() && access(cfg.sam, R_OK) == 0 ? ", NTLM via " : "",
	       nla_available() && access(cfg.sam, R_OK) == 0 ? cfg.sam : "",
	       cfg.tls_login ? "allowed" : "refused");
	if (nla_available() && access(cfg.keytab, R_OK) == 0)
		logmsg("NLA: Kerberos via %s", cfg.keytab);

	for (;;) {
		int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);

		if (fd < 0) {
			if (errno != EINTR)
				usleep(100 * 1000);
			continue;
		}
		/* the connection is passed to weston later: no CLOEXEC there,
		 * weston receives its own duplicate via SCM_RIGHTS anyway */
		if (pthread_create(&tid, NULL, connection_thread, (void *)(intptr_t)fd) != 0) {
			close(fd);
			continue;
		}
		pthread_detach(tid);
	}
}
