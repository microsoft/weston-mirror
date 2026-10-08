/*
 * weston-rail-sessions: list and end sessions of weston-rail-broker
 *
 * Copyright © 2026 weston-mirror RAIL contributors
 * MIT licensed like the rest of weston (see COPYING).
 *
 *   weston-rail-sessions               table of the sessions
 *   weston-rail-sessions --json        the same as JSON (for tools)
 *   weston-rail-sessions --logoff USER end USER's session
 *
 * Talks to the broker over /run/weston-rail-broker.sock (root only).
 */

#include "config.h"

#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define ADMIN_SOCKET "/run/weston-rail-broker.sock"

static FILE *
broker_request(const char *request)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	snprintf(addr.sun_path, sizeof addr.sun_path, "%s", ADMIN_SOCKET);
	if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		fprintf(stderr, "Broker nicht erreichbar (%s): %s\n", ADMIN_SOCKET,
			strerror(errno));
		if (errno == EACCES)
			fprintf(stderr, "Nur root darf die Sessions abfragen (sudo).\n");
		return NULL;
	}
	if (write(fd, request, strlen(request)) != (ssize_t)strlen(request)) {
		close(fd);
		return NULL;
	}
	shutdown(fd, SHUT_WR);
	return fdopen(fd, "r");
}

static const char *
state(int connected)
{
	return connected == 1 ? "verbunden" : connected == 0 ? "getrennt" : "?";
}

static int
list(bool json)
{
	FILE *f = broker_request("LIST\n");
	char line[512];
	bool first = true;
	int n = 0;

	if (!f)
		return 1;
	if (json)
		printf("[");
	else
		printf("%-20s %6s %8s  %-19s  %-10s %s\n", "BENUTZER", "UID", "PID",
		       "ANGEMELDET SEIT", "STATUS", "PROGRAMME");
	while (fgets(line, sizeof line, f)) {
		char user[64], since[32];
		unsigned uid;
		int pid, connected, apps;
		long started;
		time_t t;

		if (!strncmp(line, "END", 3))
			break;
		if (sscanf(line, "%63s %u %d %ld %d %d", user, &uid, &pid, &started,
			   &connected, &apps) != 6)
			continue;
		t = (time_t)started;
		strftime(since, sizeof since, "%Y-%m-%d %H:%M:%S", localtime(&t));
		if (json) {
			printf("%s\n  {\"user\": \"%s\", \"uid\": %u, \"pid\": %d, "
			       "\"started\": %ld, \"connected\": %s, \"apps\": %d}",
			       first ? "" : ",", user, uid, pid, started,
			       connected == 1 ? "true" : connected == 0 ? "false" : "null",
			       apps);
		} else {
			char apps_s[16];

			if (apps >= 0)
				snprintf(apps_s, sizeof apps_s, "%d", apps);
			else
				snprintf(apps_s, sizeof apps_s, "?");
			printf("%-20s %6u %8d  %-19s  %-10s %s\n", user, uid, pid, since,
			       state(connected), apps_s);
		}
		first = false;
		n++;
	}
	fclose(f);
	if (json)
		printf("%s]\n", n ? "\n" : "");
	else if (n == 0)
		printf("(keine Sessions)\n");
	return 0;
}

static int
logoff(const char *user)
{
	char request[128], line[256] = "";
	FILE *f;

	snprintf(request, sizeof request, "LOGOFF %s\n", user);
	f = broker_request(request);
	if (!f)
		return 1;
	if (!fgets(line, sizeof line, f))
		line[0] = '\0';
	fclose(f);
	if (!strncmp(line, "OK", 2)) {
		printf("Session von %s wird beendet.\n", user);
		return 0;
	}
	fprintf(stderr, "%s", line[0] ? line : "keine Antwort vom Broker\n");
	return 1;
}

int
main(int argc, char *argv[])
{
	static const struct option opts[] = {
		{ "json", no_argument, NULL, 'j' },
		{ "logoff", required_argument, NULL, 'l' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	bool json = false;
	const char *off = NULL;
	int opt;

	while ((opt = getopt_long(argc, argv, "jl:h", opts, NULL)) != -1) {
		switch (opt) {
		case 'j': json = true; break;
		case 'l': off = optarg; break;
		default:
			fprintf(stderr, "usage: weston-rail-sessions [--json] [--logoff BENUTZER]\n");
			return opt == 'h' ? 0 : 2;
		}
	}
	return off ? logoff(off) : list(json);
}
