/*
 * weston-rail-passwd: NT hashes for NLA logins at weston-rail-broker
 *
 * Copyright © 2026 weston-mirror RAIL contributors
 * MIT licensed like the rest of weston (see COPYING).
 *
 * NLA (CredSSP/NTLM) needs the NT hash of a user's password on the server,
 * which Linux does not keep. This tool maintains them in
 * /etc/weston-rail/ntlm.sam (WinPR SAM format "user:::NTHASH:::", root only).
 *
 *   weston-rail-passwd USER        set the entry; the password is checked
 *                                  against the Linux password with PAM, so
 *                                  both stay identical
 *   weston-rail-passwd -d USER     remove the entry
 *   weston-rail-passwd -l          list users with an entry
 *   weston-rail-passwd --pam-sync  for pam_exec (expose_authtok): user in
 *                                  PAM_USER, password on stdin; keeps the
 *                                  file current on login / password change
 *
 * An NT hash is an unsalted MD4 of the password and must be protected like
 * a password.
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <security/pam_appl.h>

#include <winpr/ntlm.h>
#include <winpr/ssl.h>
#include <winpr/string.h>

static const char *sam_path = "/etc/weston-rail/ntlm.sam";

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

static bool
nt_hash_hex(const char *password, char out[33])
{
	WCHAR *wide;
	size_t wlen = 0;
	BYTE hash[16];
	int i;

	wide = ConvertUtf8ToWCharAlloc(password, &wlen);
	if (!wide)
		return false;
	if (!NTOWFv1W(wide, (UINT32)(wlen * sizeof(WCHAR)), hash)) {
		memset(wide, 0, wlen * sizeof(WCHAR));
		free(wide);
		return false;
	}
	memset(wide, 0, wlen * sizeof(WCHAR));
	free(wide);
	for (i = 0; i < 16; i++)
		sprintf(out + 2 * i, "%02X", hash[i]);
	out[32] = '\0';
	memset(hash, 0, sizeof hash);
	return true;
}

/*
 * Rewrite the file under an exclusive lock: drop USER's line, append the
 * new one if hash != NULL.
 */
static int
sam_update(const char *user, const char *hash)
{
	char tmp[256], line[512], prefix[80];
	FILE *in, *out;
	int lockfd, fd;

	mkdir("/etc/weston-rail", 0755);
	lockfd = open(sam_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (lockfd < 0) {
		fprintf(stderr, "cannot open %s: %s\n", sam_path, strerror(errno));
		return 1;
	}
	if (flock(lockfd, LOCK_EX) < 0) {
		close(lockfd);
		return 1;
	}
	snprintf(tmp, sizeof tmp, "%s.tmp", sam_path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	out = fd >= 0 ? fdopen(fd, "w") : NULL;
	if (!out) {
		fprintf(stderr, "cannot write %s\n", tmp);
		close(lockfd);
		return 1;
	}
	snprintf(prefix, sizeof prefix, "%s:", user);
	in = fdopen(dup(lockfd), "r");
	if (in) {
		while (fgets(line, sizeof line, in))
			if (strncmp(line, prefix, strlen(prefix)) != 0)
				fputs(line, out);
		fclose(in);
	}
	if (hash)
		fprintf(out, "%s:::%s:::\n", user, hash);
	if (fflush(out) != 0 || fsync(fileno(out)) != 0) {
		fclose(out);
		unlink(tmp);
		close(lockfd);
		return 1;
	}
	fclose(out);
	if (rename(tmp, sam_path) < 0) {
		unlink(tmp);
		close(lockfd);
		return 1;
	}
	close(lockfd);	/* releases the lock */
	return 0;
}

static int
sam_list(void)
{
	FILE *f = fopen(sam_path, "re");
	char line[512];

	if (!f)
		return 0;
	while (fgets(line, sizeof line, f)) {
		char *c = strchr(line, ':');

		if (line[0] == '#' || !c)
			continue;
		*c = '\0';
		puts(line);
	}
	fclose(f);
	return 0;
}

/* ---- PAM password check ---- */

struct creds {
	const char *password;
};

static int
conv(int num, const struct pam_message **msg, struct pam_response **resp, void *data)
{
	struct creds *c = data;
	struct pam_response *r = calloc(num, sizeof *r);
	int i;

	if (!r)
		return PAM_BUF_ERR;
	for (i = 0; i < num; i++)
		if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF)
			r[i].resp = strdup(c->password);
	*resp = r;
	return PAM_SUCCESS;
}

static bool
pam_ok(const char *user, const char *password)
{
	struct creds c = { password };
	struct pam_conv pc = { conv, &c };
	pam_handle_t *pamh;
	int rc;

	if (pam_start("weston-rail", user, &pc, &pamh) != PAM_SUCCESS)
		return false;
	rc = pam_authenticate(pamh, PAM_DISALLOW_NULL_AUTHTOK);
	pam_end(pamh, rc);
	return rc == PAM_SUCCESS;
}

static bool
read_password(const char *prompt, char *buf, size_t size)
{
	struct termios old, noecho;
	bool tty = isatty(STDIN_FILENO);

	if (tty) {
		fputs(prompt, stderr);
		tcgetattr(STDIN_FILENO, &old);
		noecho = old;
		noecho.c_lflag &= ~ECHO;
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &noecho);
	}
	if (!fgets(buf, (int)size, stdin))
		buf[0] = '\0';
	if (tty) {
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
		fputc('\n', stderr);
	}
	buf[strcspn(buf, "\r\n")] = '\0';
	return buf[0] != '\0';
}

static int
set_user(const char *user)
{
	char pw[512], pw2[512], hash[33], prompt[128];
	int rc;

	if (!getpwnam(user)) {
		fprintf(stderr, "Unbekannter Benutzer: %s\n", user);
		return 1;
	}
	snprintf(prompt, sizeof prompt, "Linux-Passwort von %s: ", user);
	if (!read_password(prompt, pw, sizeof pw)) {
		fprintf(stderr, "Kein Passwort eingegeben.\n");
		return 1;
	}
	if (isatty(STDIN_FILENO)) {
		read_password("Wiederholen: ", pw2, sizeof pw2);
		if (strcmp(pw, pw2) != 0) {
			fprintf(stderr, "Die Passwörter stimmen nicht überein.\n");
			return 1;
		}
		memset(pw2, 0, sizeof pw2);
	}
	/* the NLA password must be the Linux password: the broker checks the
	 * delegated password with PAM as well */
	if (!pam_ok(user, pw)) {
		memset(pw, 0, sizeof pw);
		fprintf(stderr, "Das Passwort stimmt nicht mit dem Linux-Passwort von %s "
			"überein.\n", user);
		return 1;
	}
	if (!nt_hash_hex(pw, hash)) {
		memset(pw, 0, sizeof pw);
		fprintf(stderr, "NT-Hash konnte nicht berechnet werden.\n");
		return 1;
	}
	memset(pw, 0, sizeof pw);
	rc = sam_update(user, hash);
	if (rc == 0)
		printf("NLA-Anmeldung für %s eingerichtet.\n", user);
	return rc;
}

/* pam_exec expose_authtok: never fail the login, only log */
static int
pam_sync(void)
{
	const char *user = getenv("PAM_USER");
	const char *type = getenv("PAM_TYPE");
	char pw[512], hash[33];
	size_t n;

	if (!user || !valid_username(user) || !getpwnam(user))
		return 0;
	if (type && strcmp(type, "auth") != 0 && strcmp(type, "password") != 0 &&
	    strcmp(type, "sync") != 0)
		return 0;
	n = fread(pw, 1, sizeof pw - 1, stdin);
	pw[n] = '\0';
	pw[strcspn(pw, "\n")] = '\0';	/* pam_exec passes a NUL terminated token */
	if (!pw[0])
		return 0;
	if (nt_hash_hex(pw, hash))
		sam_update(user, hash);
	memset(pw, 0, sizeof pw);
	return 0;
}

static void
usage(void)
{
	fprintf(stderr,
		"usage: weston-rail-passwd BENUTZER       NLA-Anmeldung einrichten\n"
		"       weston-rail-passwd -d BENUTZER    Eintrag entfernen\n"
		"       weston-rail-passwd -l             Einträge auflisten\n"
		"       weston-rail-passwd --pam-sync     für pam_exec expose_authtok\n"
		"       Option -f DATEI                   andere Datei (%s)\n", sam_path);
}

int
main(int argc, char *argv[])
{
	static const struct option opts[] = {
		{ "delete", no_argument, NULL, 'd' },
		{ "list", no_argument, NULL, 'l' },
		{ "file", required_argument, NULL, 'f' },
		{ "pam-sync", no_argument, NULL, 'P' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	bool del = false, list = false, sync = false;
	int opt;

	while ((opt = getopt_long(argc, argv, "dlf:h", opts, NULL)) != -1) {
		switch (opt) {
		case 'd': del = true; break;
		case 'l': list = true; break;
		case 'f': sam_path = optarg; break;
		case 'P': sync = true; break;
		default: usage(); return opt == 'h' ? 0 : 2;
		}
	}
	umask(077);
	/* MD4 lives in the OpenSSL 3 legacy provider, which WinPR loads here */
	winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT);
	if (geteuid() != 0) {
		fprintf(stderr, "weston-rail-passwd muss als root laufen (sudo).\n");
		return sync ? 0 : 1;
	}
	if (sync)
		return pam_sync();
	if (list)
		return sam_list();
	if (optind != argc - 1 || !valid_username(argv[optind])) {
		usage();
		return 2;
	}
	if (del) {
		int rc = sam_update(argv[optind], NULL);

		if (rc == 0)
			printf("Eintrag für %s entfernt.\n", argv[optind]);
		return rc;
	}
	return set_user(argv[optind]);
}
