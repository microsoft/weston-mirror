/*
 * CUPS backend "rdpprint" (weston-mirror RAIL).
 *
 * Hands a print job to the weston RDP session of the job's user, which
 * forwards it over the rdpdr channel to the printer on the RDP client.
 *
 *   device URI: rdpprint:/run/user/<uid>/rdp-print-<pid>.sock?device=<id>
 *
 * Installed with mode 0700 so that CUPS runs it as root: the session
 * socket lives in the user's private runtime directory. Because of that
 * the socket is only used if it lies directly in /run/user/<uid> of the
 * job's user, is a socket and is owned by that user.
 *
 * Protocol: "RDPPRINT 1 <device>\n", the job data, shutdown(SHUT_WR),
 * then the session answers "OK\n" or "ERR <reason>\n".
 *
 * Copyright © 2026 weston-mirror RAIL contributors, MIT licensed like
 * the rest of weston.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* cups/backend.h exit codes, without depending on libcups */
#define CUPS_BACKEND_OK 0
#define CUPS_BACKEND_FAILED 1
#define CUPS_BACKEND_STOP 4

static int
fail(int code, const char *fmt, const char *arg)
{
	fprintf(stderr, "ERROR: rdpprint: ");
	fprintf(stderr, fmt, arg ? arg : "");
	fprintf(stderr, "\n");
	return code;
}

static int
write_all(int fd, const char *buf, size_t len)
{
	while (len) {
		ssize_t n = write(fd, buf, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		buf += n;
		len -= (size_t)n;
	}
	return 0;
}

int
main(int argc, char *argv[])
{
	const char *uri = getenv("DEVICE_URI");
	char path[PATH_MAX], expected_dir[64];
	const char *q, *dev;
	struct passwd *pw;
	struct stat st;
	struct sockaddr_un addr;
	char buf[65536], reply[256];
	unsigned long device;
	size_t plen;
	ssize_t n;
	int in = 0, fd;

	signal(SIGPIPE, SIG_IGN);

	if (argc == 1) {
		/* device discovery: nothing to announce, queues are created by
		 * the session itself */
		return CUPS_BACKEND_OK;
	}
	if (argc < 6 || argc > 7)
		return fail(CUPS_BACKEND_FAILED,
			    "usage: rdpprint job-id user title copies options [file]%s", NULL);
	if (!uri && argv[0] && strncmp(argv[0], "rdpprint:", 9) == 0)
		uri = argv[0];
	if (!uri || strncmp(uri, "rdpprint:", 9) != 0)
		return fail(CUPS_BACKEND_STOP, "bad device URI '%s'", uri);

	/* rdpprint:/path?device=N */
	q = strchr(uri + 9, '?');
	dev = q ? strstr(q, "device=") : NULL;
	if (!q || !dev)
		return fail(CUPS_BACKEND_STOP, "no device in URI '%s'", uri);
	plen = (size_t)(q - (uri + 9));
	if (plen == 0 || plen >= sizeof(path) || plen >= sizeof(addr.sun_path))
		return fail(CUPS_BACKEND_STOP, "bad socket path in '%s'", uri);
	memcpy(path, uri + 9, plen);
	path[plen] = '\0';
	device = strtoul(dev + 7, NULL, 10);

	/* the socket must belong to the job's user */
	pw = getpwnam(argv[2]);
	if (!pw)
		return fail(CUPS_BACKEND_FAILED, "unknown user '%s'", argv[2]);
	snprintf(expected_dir, sizeof expected_dir, "/run/user/%u/", (unsigned)pw->pw_uid);
	if (strncmp(path, expected_dir, strlen(expected_dir)) != 0 ||
	    strchr(path + strlen(expected_dir), '/') ||
	    strstr(path, ".."))
		return fail(CUPS_BACKEND_FAILED,
			    "socket '%s' is not in the runtime directory of the job's user", path);
	if (lstat(path, &st) < 0)
		return fail(CUPS_BACKEND_FAILED,
			    "RDP session not connected (no socket '%s')", path);
	if (!S_ISSOCK(st.st_mode) || st.st_uid != pw->pw_uid)
		return fail(CUPS_BACKEND_FAILED,
			    "'%s' is not a socket of the job's user", path);

	if (argc == 7) {
		in = open(argv[6], O_RDONLY | O_CLOEXEC);
		if (in < 0)
			return fail(CUPS_BACKEND_FAILED, "cannot open '%s'", argv[6]);
	}

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return fail(CUPS_BACKEND_FAILED, "socket: %s", strerror(errno));
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, path, plen + 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0)
		return fail(CUPS_BACKEND_FAILED, "RDP session not reachable: %s",
			    strerror(errno));

	fprintf(stderr, "INFO: Sende Auftrag an den RDP-Client\n");
	n = snprintf(buf, sizeof buf, "RDPPRINT 1 %lu\n", device);
	if (write_all(fd, buf, (size_t)n) < 0)
		return fail(CUPS_BACKEND_FAILED, "write: %s", strerror(errno));

	while ((n = read(in, buf, sizeof buf)) != 0) {
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return fail(CUPS_BACKEND_FAILED, "read: %s", strerror(errno));
		}
		if (write_all(fd, buf, (size_t)n) < 0)
			return fail(CUPS_BACKEND_FAILED, "session closed the connection: %s",
				    strerror(errno));
	}
	shutdown(fd, SHUT_WR);

	n = read(fd, reply, sizeof reply - 1);
	if (n <= 0)
		return fail(CUPS_BACKEND_FAILED, "no answer from the RDP session%s", NULL);
	reply[n] = '\0';
	if (strncmp(reply, "OK", 2) != 0) {
		reply[strcspn(reply, "\n")] = '\0';
		return fail(CUPS_BACKEND_FAILED, "%s", reply);
	}
	fprintf(stderr, "INFO: Auftrag an den RDP-Client uebergeben\n");
	return CUPS_BACKEND_OK;
}
