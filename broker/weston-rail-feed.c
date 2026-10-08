/*
 * weston-rail-feed: workspace feed for weston-mirror RAIL RemoteApps
 *
 * Copyright © 2026 weston-mirror RAIL contributors
 * MIT licensed like the rest of weston (see COPYING).
 *
 * Serves the published programs of /etc/weston-rail/apps.conf in the
 * format of RD Web Access (RemoteApp and Desktop Connection feed,
 * http://schemas.microsoft.com/ts/2007/05/tswf), so that clients can
 * subscribe to them as a workspace:
 *
 *   - Windows: Control Panel -> RemoteApp and Desktop Connections
 *     (programs appear in the start menu; needs a trusted certificate)
 *   - Windows App on macOS, iOS/iPadOS, Android/ChromeOS: "Add workspace"
 *
 *   https://SERVER/RDWeb/Feed/webfeed.aspx      the feed
 *   https://SERVER/RDWeb/Feed/rdp/<name>.rdp    connection file per program
 *   https://SERVER/RDWeb/Feed/icon/<name>.png   icon (PNG)
 *   https://SERVER/RDWeb/Feed/icon/<name>.ico   icon (ICO)
 *
 * The feed itself needs no login: it only lists what is published. Users
 * authenticate when connecting (NLA at weston-rail-broker).
 */

#include "config.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/pkcs7.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

static struct {
	int port;
	const char *cert;
	const char *key;
	const char *apps_conf;
	const char *icon_cache;
	const char *listen_addr;	/* NULL: all addresses */
	const char *sign_cert;		/* optional: sign the .rdp files */
	const char *sign_key;
	bool plain_http;		/* behind a TLS reverse proxy (Caddy, nginx) */
	bool verbose;
} cfg = {
	.port = 443,
	.cert = "/etc/weston-rail/tls.crt",
	.key = "/etc/weston-rail/tls.key",
	.apps_conf = "/etc/weston-rail/apps.conf",
	.icon_cache = "/var/cache/weston-rail/icons",
};

static SSL_CTX *ssl_ctx;

/* one client connection: TLS, or plain HTTP behind a reverse proxy */
struct conn {
	SSL *ssl;
	int fd;
};

static int
conn_read(struct conn *c, void *buf, int len)
{
	ssize_t n;

	if (c->ssl)
		return SSL_read(c->ssl, buf, len);
	do {
		n = read(c->fd, buf, (size_t)len);
	} while (n < 0 && errno == EINTR);
	return (int)n;
}

static int
conn_write(struct conn *c, const void *buf, int len)
{
	ssize_t n;

	if (c->ssl)
		return SSL_write(c->ssl, buf, len);
	do {
		n = write(c->fd, buf, (size_t)len);
	} while (n < 0 && errno == EINTR);
	return (int)n;
}

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

/* ---- configuration (apps.conf) ------------------------------------ */

struct app {
	char name[128];
	char title[256];
	char command[2048];
	char icon[PATH_MAX];
};

struct workspace {
	char name[256];
	char address[256];	/* host[:port] the clients connect to */
	int auth_level;
	char rdp_extra[2048];	/* additional .rdp lines, "\n" separated */
	struct app *apps;
	int n_apps;
	time_t mtime;
};

static char *
trim(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && strchr(" \t\r\n", e[-1]))
		*--e = '\0';
	return s;
}

static bool
load_workspace(struct workspace *ws)
{
	FILE *f = fopen(cfg.apps_conf, "re");
	char line[4096], section[32] = "";
	struct stat st;
	struct app *cur = NULL;

	memset(ws, 0, sizeof *ws);
	gethostname(ws->name, sizeof ws->name - 1);
	ws->auth_level = 0;
	if (!f)
		return false;
	if (fstat(fileno(f), &st) == 0)
		ws->mtime = st.st_mtime;
	while (fgets(line, sizeof line, f)) {
		char *l = trim(line), *eq, *k, *v;

		if (!*l || *l == '#' || *l == ';')
			continue;
		if (*l == '[') {
			snprintf(section, sizeof section, "%s", l);
			cur = NULL;
			if (!strcmp(section, "[app]")) {
				ws->apps = realloc(ws->apps, (ws->n_apps + 1) * sizeof *ws->apps);
				if (!ws->apps) {
					fclose(f);
					return false;
				}
				cur = &ws->apps[ws->n_apps++];
				memset(cur, 0, sizeof *cur);
			}
			continue;
		}
		eq = strchr(l, '=');
		if (!eq)
			continue;
		*eq = '\0';
		k = trim(l);
		v = trim(eq + 1);
		if (!strcmp(section, "[workspace]")) {
			if (!strcmp(k, "name"))
				snprintf(ws->name, sizeof ws->name, "%s", v);
			else if (!strcmp(k, "address"))
				snprintf(ws->address, sizeof ws->address, "%s", v);
			else if (!strcmp(k, "authentication-level"))
				ws->auth_level = atoi(v);
			else if (!strcmp(k, "rdp")) {
				size_t len = strlen(ws->rdp_extra);

				snprintf(ws->rdp_extra + len, sizeof ws->rdp_extra - len, "%s\n", v);
			}
		} else if (cur) {
			if (!strcmp(k, "name"))
				snprintf(cur->name, sizeof cur->name, "%s", v);
			else if (!strcmp(k, "title"))
				snprintf(cur->title, sizeof cur->title, "%s", v);
			else if (!strcmp(k, "command"))
				snprintf(cur->command, sizeof cur->command, "%s", v);
			else if (!strcmp(k, "icon"))
				snprintf(cur->icon, sizeof cur->icon, "%s", v);
		}
	}
	fclose(f);
	/* drop incomplete entries, default titles */
	for (int i = 0; i < ws->n_apps; i++) {
		struct app *a = &ws->apps[i];

		if (!a->name[0] || !a->command[0]) {
			memmove(a, a + 1, (ws->n_apps - i - 1) * sizeof *a);
			ws->n_apps--;
			i--;
			continue;
		}
		if (!a->title[0]) {
			snprintf(a->title, sizeof a->title, "%s", a->name);
			a->title[0] = (char)toupper((unsigned char)a->title[0]);
		}
	}
	return true;
}

static struct app *
find_app(struct workspace *ws, const char *name)
{
	for (int i = 0; i < ws->n_apps; i++)
		if (!strcasecmp(ws->apps[i].name, name))
			return &ws->apps[i];
	return NULL;
}

/* ---- icons ---------------------------------------------------------- */

static void
command_basename(const char *command, char *out, size_t size)
{
	char exe[PATH_MAX];
	const char *slash;
	size_t i = 0;

	while (*command == ' ' || *command == '"' || *command == '\'')
		command++;
	while (*command && !strchr(" \t\"'", *command) && i + 1 < sizeof exe)
		exe[i++] = *command++;
	exe[i] = '\0';
	slash = strrchr(exe, '/');
	snprintf(out, size, "%s", slash ? slash + 1 : exe);
}

/* Icon= of the .desktop file whose Exec starts the same program */
static bool
desktop_icon_name(const char *command, char *out, size_t size)
{
	static const char *dirs[] = { "/usr/share/applications",
				      "/usr/local/share/applications" };
	char base[256];
	bool found = false;

	command_basename(command, base, sizeof base);
	for (size_t d = 0; d < sizeof dirs / sizeof dirs[0] && !found; d++) {
		DIR *dir = opendir(dirs[d]);
		struct dirent *de;

		if (!dir)
			continue;
		while (!found && (de = readdir(dir))) {
			char path[PATH_MAX], line[1024], icon[256] = "", execb[256] = "";
			FILE *f;
			size_t len = strlen(de->d_name);

			if (len < 9 || strcmp(de->d_name + len - 8, ".desktop"))
				continue;
			snprintf(path, sizeof path, "%s/%s", dirs[d], de->d_name);
			f = fopen(path, "re");
			if (!f)
				continue;
			while (fgets(line, sizeof line, f)) {
				if (line[0] == '[' && strncmp(line, "[Desktop Entry]", 15))
					break;	/* only the main section */
				if (!strncmp(line, "Icon=", 5))
					snprintf(icon, sizeof icon, "%s", trim(line + 5));
				else if (!strncmp(line, "Exec=", 5))
					command_basename(trim(line + 5), execb, sizeof execb);
			}
			fclose(f);
			if (icon[0] && execb[0] && !strcmp(execb, base)) {
				snprintf(out, size, "%s", icon);
				found = true;
			}
		}
		closedir(dir);
	}
	return found;
}

static bool
file_exists(const char *p)
{
	return access(p, R_OK) == 0;
}

/* find NAME.png or NAME.svg in one icon theme directory */
static bool
theme_lookup(const char *theme_dir, const char *name, char *out, size_t size)
{
	static const char *sizes[] = { "64", "48", "128", "96", "256", "72", "32", "24" };
	static const char *exts[] = { "png", "svg" };
	char path[PATH_MAX];

	for (size_t e = 0; e < 2; e++) {
		for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
			/* freedesktop layout: 64x64/apps, and Ubuntu style: apps/64 */
			snprintf(path, sizeof path, "%s/%sx%s/apps/%s.%s", theme_dir, sizes[s],
				 sizes[s], name, exts[e]);
			if (file_exists(path))
				goto found;
			snprintf(path, sizeof path, "%s/apps/%s/%s.%s", theme_dir, sizes[s], name,
				 exts[e]);
			if (file_exists(path))
				goto found;
		}
		snprintf(path, sizeof path, "%s/scalable/apps/%s.%s", theme_dir, name, exts[e]);
		if (file_exists(path))
			goto found;
	}
	return false;
found:
	snprintf(out, size, "%s", path);
	return true;
}

/* NAME from the themes: hicolor first (where applications install their
 * icons), then every other theme, then /usr/share/pixmaps */
static bool
find_icon_file(const char *name, char *out, size_t size)
{
	DIR *dir;
	struct dirent *de;
	char path[PATH_MAX];
	bool found = false;

	if (theme_lookup("/usr/share/icons/hicolor", name, out, size) ||
	    theme_lookup("/usr/local/share/icons/hicolor", name, out, size))
		return true;
	dir = opendir("/usr/share/icons");
	if (dir) {
		while (!found && (de = readdir(dir))) {
			if (de->d_name[0] == '.' || !strcmp(de->d_name, "hicolor"))
				continue;
			snprintf(path, sizeof path, "/usr/share/icons/%s", de->d_name);
			found = theme_lookup(path, name, out, size);
		}
		closedir(dir);
	}
	if (found)
		return true;
	for (size_t e = 0; e < 3; e++) {
		static const char *exts[] = { "png", "svg", "xpm" };

		if (e == 2)
			break;	/* xpm is not usable */
		snprintf(path, sizeof path, "/usr/share/pixmaps/%s.%s", name, exts[e]);
		if (file_exists(path)) {
			snprintf(out, size, "%s", path);
			return true;
		}
	}
	return false;
}

/* resolve the icon of APP to a PNG file (SVG rendered with rsvg-convert) */
static bool icon_png_path_for(const struct app *app, char *out, size_t size);

/* never leave a program without icon: clients may skip the whole entry */
static bool
icon_png_path(const struct app *app, char *out, size_t size)
{
	static const char *generic[] = { "application-x-executable", "applications-other",
					 "system-run", "utilities-terminal" };
	struct app tmp;

	if (icon_png_path_for(app, out, size))
		return true;
	for (size_t i = 0; i < sizeof generic / sizeof generic[0]; i++) {
		tmp = *app;
		snprintf(tmp.icon, sizeof tmp.icon, "%s", generic[i]);
		snprintf(tmp.name, sizeof tmp.name, "_generic-%s", generic[i]);
		if (icon_png_path_for(&tmp, out, size))
			return true;
	}
	return false;
}

static bool
icon_png_path_for(const struct app *app, char *out, size_t size)
{
	char name[256], file[PATH_MAX], cached[PATH_MAX];
	const char *ext;
	pid_t pid;
	int status;

	if (app->icon[0] == '/')
		snprintf(file, sizeof file, "%s", app->icon);
	else {
		if (app->icon[0])
			snprintf(name, sizeof name, "%s", app->icon);
		else if (!desktop_icon_name(app->command, name, sizeof name))
			return false;
		if (name[0] == '/')
			snprintf(file, sizeof file, "%s", name);	/* Icon= with a path */
		else if (!find_icon_file(name, file, sizeof file))
			return false;
	}
	if (!file_exists(file))
		return false;
	ext = strrchr(file, '.');
	if (ext && !strcasecmp(ext, ".png")) {
		snprintf(out, size, "%s", file);
		return true;
	}
	if (!ext || strcasecmp(ext, ".svg"))
		return false;

	/* render the SVG once into the cache */
	snprintf(cached, sizeof cached, "%s/%s.png", cfg.icon_cache, app->name);
	{
		struct stat cs, ss;

		if (stat(cached, &cs) == 0 && stat(file, &ss) == 0 && cs.st_mtime >= ss.st_mtime) {
			snprintf(out, size, "%s", cached);
			return true;
		}
	}
	mkdir("/var/cache/weston-rail", 0755);
	mkdir(cfg.icon_cache, 0755);
	pid = fork();
	if (pid == 0) {
		int devnull = open("/dev/null", O_WRONLY);

		if (devnull >= 0)
			dup2(devnull, STDERR_FILENO);
		execlp("rsvg-convert", "rsvg-convert", "-w", "64", "-h", "64", "-o", cached,
		       file, (char *)NULL);
		_exit(127);
	}
	if (pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
	    WEXITSTATUS(status) == 0 && file_exists(cached)) {
		snprintf(out, size, "%s", cached);
		return true;
	}
	return false;
}

/* 32x32 generic window icon, used when no icon theme provides one */
static const unsigned char builtin_icon_png[] = { 0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x20,0x00,0x00,0x00,0x20,0x08,0x06,0x00,0x00,0x00,0x73,0x7a,0x7a,0xf4,0x00,0x00,0x00,0x3e,0x49,0x44,0x41,0x54,0x78,0xda,0xed,0xd7,0x31,0x11,0x00,0x20,0x0c,0x00,0xb1,0x7a,0xaa,0x27,0xd4,0x22,0xa6,0x0e,0xc0,0x45,0xe9,0x1d,0x19,0x7e,0xcf,0xfa,0x91,0xb9,0xce,0xcb,0x62,0x0e,0x60,0xed,0xde,0x00,0x00,0xc6,0x01,0xaa,0xaa,0x35,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x79,0x80,0x6f,0xef,0xf8,0x02,0xb9,0x23,0x6a,0xbd,0xb1,0xe0,0x54,0xa0,0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82 };

static unsigned char *
read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	unsigned char *buf;
	long n;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0 || n > 8 * 1024 * 1024) {
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)n + 22);
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	*len = (size_t)n;
	return buf;
}

/* ICO containing the PNG as is (supported since Windows Vista) */
static unsigned char *
png_to_ico(const unsigned char *png, size_t len, size_t *out_len)
{
	unsigned char *ico = malloc(len + 22);
	unsigned w = 0, h = 0;

	if (!ico)
		return NULL;
	if (len > 24 && !memcmp(png, "\x89PNG", 4)) {
		w = (unsigned)png[16] << 24 | png[17] << 16 | png[18] << 8 | png[19];
		h = (unsigned)png[20] << 24 | png[21] << 16 | png[22] << 8 | png[23];
	}
	memset(ico, 0, 22);
	ico[2] = 1;			/* type: icon */
	ico[4] = 1;			/* one image */
	ico[6] = w >= 256 ? 0 : (unsigned char)w;
	ico[7] = h >= 256 ? 0 : (unsigned char)h;
	ico[10] = 1;			/* planes */
	ico[12] = 32;			/* bpp */
	ico[14] = len & 0xff;
	ico[15] = (len >> 8) & 0xff;
	ico[16] = (len >> 16) & 0xff;
	ico[17] = (len >> 24) & 0xff;
	ico[18] = 22;			/* offset */
	memcpy(ico + 22, png, len);
	*out_len = len + 22;
	return ico;
}

/* ---- HTTP ----------------------------------------------------------- */

struct buf {
	char *data;
	size_t len, cap;
};

static void
buf_add(struct buf *b, const char *fmt, ...)
{
	va_list ap;
	int n;

	for (;;) {
		size_t room = b->cap - b->len;

		va_start(ap, fmt);
		n = vsnprintf(b->data ? b->data + b->len : NULL, b->data ? room : 0, fmt, ap);
		va_end(ap);
		if (n < 0)
			return;
		if (b->data && (size_t)n < room) {
			b->len += (size_t)n;
			return;
		}
		b->cap = (b->cap + (size_t)n + 1) * 2;
		b->data = realloc(b->data, b->cap);
		if (!b->data)
			return;
	}
}

static void
xml_escape(struct buf *b, const char *s)
{
	for (; *s; s++) {
		switch (*s) {
		case '&': buf_add(b, "&amp;"); break;
		case '<': buf_add(b, "&lt;"); break;
		case '>': buf_add(b, "&gt;"); break;
		case '"': buf_add(b, "&quot;"); break;
		default: buf_add(b, "%c", *s); break;
		}
	}
}

static void
send_all(struct conn *ssl, const void *data, size_t len)
{
	const char *p = data;

	while (len > 0) {
		int n = conn_write(ssl, p, len > INT_MAX ? INT_MAX : (int)len);

		if (n <= 0)
			return;
		p += n;
		len -= (size_t)n;
	}
}

static void
respond(struct conn *ssl, bool head, int code, const char *type, const void *body, size_t len,
	const char *extra_headers)
{
	char hdr[1024];
	int n;
	const char *text = code == 200 ? "OK" : code == 404 ? "Not Found" :
			   code == 405 ? "Method Not Allowed" : "Bad Request";

	n = snprintf(hdr, sizeof hdr,
		     "HTTP/1.1 %d %s\r\n"
		     "Content-Type: %s\r\n"
		     "Content-Length: %zu\r\n"
		     "Cache-Control: no-cache\r\n"
		     "%s"
		     "Connection: close\r\n\r\n",
		     code, text, type, len, extra_headers ? extra_headers : "");
	send_all(ssl, hdr, (size_t)n);
	if (!head && body && len)
		send_all(ssl, body, len);
}

static void
iso_time(time_t t, char *out, size_t size)
{
	strftime(out, size, "%Y-%m-%dT%H:%M:%S.000Z", gmtime(&t));
}

/* the address clients connect to: [workspace] address, else the host
 * the feed was fetched from */
static void
connect_address(const struct workspace *ws, const char *host, char *out, size_t size)
{
	char h[256];

	if (ws->address[0]) {
		snprintf(out, size, "%s", ws->address);
		return;
	}
	snprintf(h, sizeof h, "%s", host);
	if (h[0] != '[')
		h[strcspn(h, ":")] = '\0';	/* drop the HTTPS port */
	snprintf(out, size, "%s", h);
}

/*
 * Links in the feed are root relative ("/RDWeb/Feed/..."), like RD Web
 * Access and RAWeb: the client resolves them against the feed URL it
 * used, so they stay correct behind a reverse proxy whatever Host header
 * reaches us.
 */
static void
serve_feed(struct conn *ssl, bool head, struct workspace *ws, const char *host)
{
	struct buf b = { 0 };
	char now[40], updated[40], server[256];

	iso_time(time(NULL), now, sizeof now);
	iso_time(ws->mtime ? ws->mtime : time(NULL), updated, sizeof updated);
	connect_address(ws, host, server, sizeof server);

	buf_add(&b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
		    "<ResourceCollection PubDate=\"%s\" SchemaVersion=\"1.1\" "
		    "xmlns=\"http://schemas.microsoft.com/ts/2007/05/tswf\">\r\n"
		    "  <Publisher LastUpdated=\"%s\" Name=\"", now, updated);
	xml_escape(&b, ws->name);
	buf_add(&b, "\" ID=\"");
	xml_escape(&b, server);
	buf_add(&b, "\" Description=\"\">\r\n    <Resources>\r\n");
	for (int i = 0; i < ws->n_apps; i++) {
		struct app *a = &ws->apps[i];

		buf_add(&b, "      <Resource ID=\"");
		xml_escape(&b, a->name);
		buf_add(&b, "\" Alias=\"");
		xml_escape(&b, a->name);
		buf_add(&b, "\" Title=\"");
		xml_escape(&b, a->title);
		buf_add(&b, "\" LastUpdated=\"%s\" Type=\"RemoteApp\">\r\n"
			    "        <Icons>\r\n"
			    "          <IconRaw FileType=\"Ico\" FileURL=\"/RDWeb/Feed/icon/",
			updated);
		xml_escape(&b, a->name);
		buf_add(&b, ".ico\" />\r\n"
			    "          <Icon32 Dimensions=\"32x32\" FileType=\"Png\" "
			    "FileURL=\"/RDWeb/Feed/icon/");
		xml_escape(&b, a->name);
		buf_add(&b, ".png\" />\r\n"
			    "        </Icons>\r\n"
			    "        <FileExtensions />\r\n"
			    "        <HostingTerminalServers>\r\n"
			    "          <HostingTerminalServer>\r\n"
			    "            <ResourceFile FileExtension=\".rdp\" "
			    "URL=\"/RDWeb/Feed/rdp/");
		xml_escape(&b, a->name);
		buf_add(&b, ".rdp\" />\r\n            <TerminalServerRef Ref=\"");
		xml_escape(&b, server);
		buf_add(&b, "\" />\r\n          </HostingTerminalServer>\r\n"
			    "        </HostingTerminalServers>\r\n"
			    "      </Resource>\r\n");
	}
	buf_add(&b, "    </Resources>\r\n    <TerminalServers>\r\n"
		    "      <TerminalServer ID=\"");
	xml_escape(&b, server);
	buf_add(&b, "\" Name=\"");
	xml_escape(&b, server);
	buf_add(&b, "\" LastUpdated=\"%s\" />\r\n"
		    "    </TerminalServers>\r\n  </Publisher>\r\n</ResourceCollection>\r\n",
		updated);
	respond(ssl, head, 200, "application/x-msts-radc+xml; charset=utf-8", b.data, b.len,
		NULL);
	free(b.data);
}


/* ---- signing of .rdp files (optional) ----------------------------- */

/*
 * Same format as Microsoft's rdpsign.exe (see also nfedera/rdpsign):
 * the "secure" settings present in the file, in this fixed order, plus
 * "signscope:s:<names>" are joined with CRLF, NUL terminated, encoded as
 * UTF-16LE and signed as detached PKCS#7 (DER, no signed attributes).
 * The signature line is base64 of a 12 byte header (0x00010001,
 * 0x00000001, length) followed by the DER blob.
 */
static const struct {
	const char *prefix;
	const char *name;
} secure_settings[] = {
	{ "full address:s:", "Full Address" },
	{ "alternate full address:s:", "Alternate Full Address" },
	{ "pcb:s:", "PCB" },
	{ "use redirection server name:i:", "Use Redirection Server Name" },
	{ "server port:i:", "Server Port" },
	{ "negotiate security layer:i:", "Negotiate Security Layer" },
	{ "enablecredsspsupport:i:", "EnableCredSspSupport" },
	{ "disableconnectionsharing:i:", "DisableConnectionSharing" },
	{ "autoreconnection enabled:i:", "AutoReconnection Enabled" },
	{ "gatewayhostname:s:", "GatewayHostname" },
	{ "gatewayusagemethod:i:", "GatewayUsageMethod" },
	{ "gatewayprofileusagemethod:i:", "GatewayProfileUsageMethod" },
	{ "gatewaycredentialssource:i:", "GatewayCredentialsSource" },
	{ "support url:s:", "Support URL" },
	{ "promptcredentialonce:i:", "PromptCredentialOnce" },
	{ "require pre-authentication:i:", "Require pre-authentication" },
	{ "pre-authentication server address:s:", "Pre-authentication server address" },
	{ "alternate shell:s:", "Alternate Shell" },
	{ "shell working directory:s:", "Shell Working Directory" },
	{ "remoteapplicationprogram:s:", "RemoteApplicationProgram" },
	{ "remoteapplicationexpandworkingdir:s:", "RemoteApplicationExpandWorkingdir" },
	{ "remoteapplicationmode:i:", "RemoteApplicationMode" },
	{ "remoteapplicationguid:s:", "RemoteApplicationGuid" },
	{ "remoteapplicationname:s:", "RemoteApplicationName" },
	{ "remoteapplicationicon:s:", "RemoteApplicationIcon" },
	{ "remoteapplicationfile:s:", "RemoteApplicationFile" },
	{ "remoteapplicationfileextensions:s:", "RemoteApplicationFileExtensions" },
	{ "remoteapplicationcmdline:s:", "RemoteApplicationCmdLine" },
	{ "remoteapplicationexpandcmdline:s:", "RemoteApplicationExpandCmdLine" },
	{ "prompt for credentials:i:", "Prompt For Credentials" },
	{ "authentication level:i:", "Authentication Level" },
	{ "audiomode:i:", "AudioMode" },
	{ "redirectdrives:i:", "RedirectDrives" },
	{ "redirectprinters:i:", "RedirectPrinters" },
	{ "redirectcomports:i:", "RedirectCOMPorts" },
	{ "redirectsmartcards:i:", "RedirectSmartCards" },
	{ "redirectposdevices:i:", "RedirectPOSDevices" },
	{ "redirectclipboard:i:", "RedirectClipboard" },
	{ "devicestoredirect:s:", "DevicesToRedirect" },
	{ "drivestoredirect:s:", "DrivesToRedirect" },
	{ "loadbalanceinfo:s:", "LoadBalanceInfo" },
	{ "redirectdirectx:i:", "RedirectDirectX" },
	{ "rdgiskdcproxy:i:", "RDGIsKDCProxy" },
	{ "kdcproxyname:s:", "KDCProxyName" },
	{ "eventloguploadaddress:s:", "EventLogUploadAddress" },
};

/* UTF-8 -> UTF-16LE (with surrogate pairs); returns bytes written */
static size_t
utf8_to_utf16le(const char *in, size_t in_len, unsigned char *out)
{
	const unsigned char *p = (const unsigned char *)in, *end = p + in_len;
	size_t o = 0;

	while (p < end) {
		unsigned cp;

		if (*p < 0x80) {
			cp = *p++;
		} else if ((*p & 0xe0) == 0xc0 && p + 1 < end) {
			cp = (p[0] & 0x1fu) << 6 | (p[1] & 0x3fu);
			p += 2;
		} else if ((*p & 0xf0) == 0xe0 && p + 2 < end) {
			cp = (p[0] & 0x0fu) << 12 | (p[1] & 0x3fu) << 6 | (p[2] & 0x3fu);
			p += 3;
		} else if ((*p & 0xf8) == 0xf0 && p + 3 < end) {
			cp = (p[0] & 0x07u) << 18 | (p[1] & 0x3fu) << 12 |
			     (p[2] & 0x3fu) << 6 | (p[3] & 0x3fu);
			p += 4;
		} else {
			cp = 0xfffd;
			p++;
		}
		if (cp >= 0x10000) {
			unsigned v = cp - 0x10000, hi = 0xd800 | (v >> 10), lo = 0xdc00 | (v & 0x3ff);

			out[o++] = hi & 0xff;
			out[o++] = hi >> 8;
			out[o++] = lo & 0xff;
			out[o++] = lo >> 8;
		} else {
			out[o++] = cp & 0xff;
			out[o++] = (cp >> 8) & 0xff;
		}
	}
	return o;
}

static const char b64chars[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void
base64_append(struct buf *b, const unsigned char *d, size_t len)
{
	for (size_t i = 0; i < len; i += 3) {
		unsigned v = (unsigned)d[i] << 16 | (i + 1 < len ? (unsigned)d[i + 1] << 8 : 0) |
			     (i + 2 < len ? d[i + 2] : 0);

		buf_add(b, "%c%c%c%c", b64chars[v >> 18 & 63], b64chars[v >> 12 & 63],
			i + 1 < len ? b64chars[v >> 6 & 63] : '=',
			i + 2 < len ? b64chars[v & 63] : '=');
	}
}

/*
 * Append "signscope" and "signature" to the CRLF separated settings in
 * RDP. Certificate and key are read on every call, so a renewed
 * certificate is used without restart. Returns false on any error (the
 * caller then serves the file unsigned and logs it).
 */
static bool
sign_rdp(struct buf *rdp)
{
	struct buf msg = { 0 }, names = { 0 };
	unsigned char *utf16 = NULL, *der = NULL, *blob = NULL;
	STACK_OF(X509) *chain = NULL;
	X509 *cert = NULL, *extra;
	EVP_PKEY *key = NULL;
	PKCS7 *p7 = NULL;
	BIO *data = NULL;
	FILE *f;
	int der_len;
	size_t utf16_len;
	bool ok = false;

	/* signed lines in the fixed order of the secure settings */
	for (size_t i = 0; i < sizeof secure_settings / sizeof secure_settings[0]; i++) {
		size_t plen = strlen(secure_settings[i].prefix);
		const char *p = rdp->data;

		while (p && *p) {
			const char *eol = strstr(p, "\r\n");
			size_t llen = eol ? (size_t)(eol - p) : strlen(p);

			if (llen >= plen && !strncmp(p, secure_settings[i].prefix, plen)) {
				buf_add(&msg, "%.*s\r\n", (int)llen, p);
				buf_add(&names, "%s%s", names.len ? "," : "", secure_settings[i].name);
				break;
			}
			p = eol ? eol + 2 : NULL;
		}
	}
	if (!names.len)
		goto out;
	buf_add(&msg, "signscope:s:%s\r\n", names.data);

	/* UTF-16LE including the terminating NUL character */
	utf16 = malloc((msg.len + 1) * 4);
	if (!utf16)
		goto out;
	utf16_len = utf8_to_utf16le(msg.data, msg.len, utf16);
	utf16[utf16_len++] = 0;
	utf16[utf16_len++] = 0;

	f = fopen(cfg.sign_cert, "re");
	if (!f)
		goto out;
	cert = PEM_read_X509(f, NULL, NULL, NULL);
	chain = sk_X509_new_null();
	/* the rest of a fullchain file: intermediates, sent along */
	while (chain && (extra = PEM_read_X509(f, NULL, NULL, NULL)))
		sk_X509_push(chain, extra);
	fclose(f);
	ERR_clear_error();	/* end of file after the last certificate */
	f = fopen(cfg.sign_key, "re");
	if (!f)
		goto out;
	key = PEM_read_PrivateKey(f, NULL, NULL, NULL);
	fclose(f);
	if (!cert || !key || !X509_check_private_key(cert, key))
		goto out;

	data = BIO_new_mem_buf(utf16, (int)utf16_len);
	p7 = PKCS7_sign(cert, key, chain, data,
			PKCS7_BINARY | PKCS7_DETACHED | PKCS7_NOATTR | PKCS7_NOSMIMECAP);
	if (!p7)
		goto out;
	der_len = i2d_PKCS7(p7, &der);
	if (der_len <= 0)
		goto out;

	blob = malloc((size_t)der_len + 12);
	if (!blob)
		goto out;
	blob[0] = 0x01; blob[1] = 0x00; blob[2] = 0x01; blob[3] = 0x00;
	blob[4] = 0x01; blob[5] = 0x00; blob[6] = 0x00; blob[7] = 0x00;
	blob[8] = der_len & 0xff;
	blob[9] = (der_len >> 8) & 0xff;
	blob[10] = (der_len >> 16) & 0xff;
	blob[11] = (der_len >> 24) & 0xff;
	memcpy(blob + 12, der, (size_t)der_len);

	buf_add(rdp, "signscope:s:%s\r\nsignature:s:", names.data);
	base64_append(rdp, blob, (size_t)der_len + 12);
	buf_add(rdp, "\r\n");
	ok = true;
out:
	if (!ok)
		logmsg("cannot sign .rdp files with %s / %s (serving them unsigned): %s",
		       cfg.sign_cert, cfg.sign_key,
		       ERR_peek_last_error() ? ERR_error_string(ERR_get_error(), NULL)
					     : "certificate, key or signscope missing");
	free(msg.data);
	free(names.data);
	free(utf16);
	free(blob);
	OPENSSL_free(der);
	PKCS7_free(p7);
	BIO_free(data);
	X509_free(cert);
	sk_X509_pop_free(chain, X509_free);
	EVP_PKEY_free(key);
	return ok;
}

static void
serve_rdp(struct conn *ssl, bool head, struct workspace *ws, const char *host, struct app *a)
{
	struct buf b = { 0 };
	char server[256], disp[512];

	connect_address(ws, host, server, sizeof server);
	buf_add(&b,
		"full address:s:%s\r\n"
		"alternate full address:s:%s\r\n"
		"remoteapplicationmode:i:1\r\n"
		"remoteapplicationprogram:s:||%s\r\n"
		"alternate shell:s:||%s\r\n"
		"remoteapplicationname:s:%s\r\n"
		"workspace id:s:%s\r\n"
		"enablecredsspsupport:i:1\r\n"
		"authentication level:i:%d\r\n"
		"disableconnectionsharing:i:1\r\n"
		"drivestoredirect:s:*\r\n"
		"redirectprinters:i:1\r\n"
		"redirectclipboard:i:1\r\n",
		server, server, a->name, a->name, a->title, server, ws->auth_level);
	if (ws->rdp_extra[0]) {
		char extra[2048], *line, *save = NULL;

		snprintf(extra, sizeof extra, "%s", ws->rdp_extra);
		for (line = strtok_r(extra, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
			buf_add(&b, "%s\r\n", line);
	}
	/* optional: only with --sign-cert/--sign-key */
	if (cfg.sign_cert && cfg.sign_key)
		sign_rdp(&b);
	snprintf(disp, sizeof disp, "Content-Disposition: attachment; filename=\"%s.rdp\"\r\n",
		 a->name);
	respond(ssl, head, 200, "application/x-rdp", b.data, b.len, disp);
	free(b.data);
}

static void
serve_icon(struct conn *ssl, bool head, struct app *a, bool ico)
{
	char png[PATH_MAX];
	unsigned char *data = NULL, *out;
	size_t len = 0, out_len = 0;

	if (icon_png_path(a, png, sizeof png))
		data = read_file(png, &len);
	if (!data) {
		len = sizeof builtin_icon_png;
		data = malloc(len);
		if (!data) {
			respond(ssl, head, 404, "text/plain", "no icon\n", 8, NULL);
			return;
		}
		memcpy(data, builtin_icon_png, len);
	}
	if (ico) {
		out = png_to_ico(data, len, &out_len);
		free(data);
		if (!out) {
			respond(ssl, head, 404, "text/plain", "no icon\n", 8, NULL);
			return;
		}
		respond(ssl, head, 200, "image/x-icon", out, out_len, NULL);
		free(out);
	} else {
		respond(ssl, head, 200, "image/png", data, len, NULL);
		free(data);
	}
}

static bool
ends_with_ci(const char *s, const char *suffix)
{
	size_t a = strlen(s), b = strlen(suffix);

	return a >= b && !strcasecmp(s + a - b, suffix);
}

static void
handle(struct conn *ssl, const char *rhost)
{
	char req[8192], method[16], path[2048], host[256] = "";
	int n, total = 0;
	char *line, *save = NULL, *q;
	struct workspace ws;
	bool head;

	/* read the request header */
	while (total < (int)sizeof req - 1) {
		n = conn_read(ssl, req + total, (int)sizeof req - 1 - total);
		if (n <= 0)
			return;
		total += n;
		req[total] = '\0';
		if (strstr(req, "\r\n\r\n"))
			break;
	}
	if (sscanf(req, "%15s %2047s", method, path) != 2) {
		respond(ssl, false, 400, "text/plain", "bad request\n", 12, NULL);
		return;
	}
	{
		char fwd_host[256] = "";

		for (line = strtok_r(req, "\r\n", &save); line;
		     line = strtok_r(NULL, "\r\n", &save)) {
			if (!strncasecmp(line, "Host:", 5))
				snprintf(host, sizeof host, "%s", trim(line + 5));
			else if (!strncasecmp(line, "X-Forwarded-Host:", 17))
				snprintf(fwd_host, sizeof fwd_host, "%s", trim(line + 17));
		}
		/* behind a reverse proxy the public name is what the links need */
		if (cfg.plain_http && fwd_host[0]) {
			fwd_host[strcspn(fwd_host, ",")] = '\0';
			snprintf(host, sizeof host, "%s", trim(fwd_host));
		}
	}
	if (!host[0] || strpbrk(host, "\"<>/ ")) {
		respond(ssl, false, 400, "text/plain", "bad host\n", 9, NULL);
		return;
	}
	head = !strcmp(method, "HEAD");
	if (!head && strcmp(method, "GET")) {
		respond(ssl, false, 405, "text/plain", "GET only\n", 9, NULL);
		return;
	}
	q = strchr(path, '?');
	if (q)
		*q = '\0';
	if (cfg.verbose)
		logmsg("%s %s %s", rhost, method, path);

	if (!load_workspace(&ws)) {
		respond(ssl, head, 404, "text/plain", "no apps.conf\n", 13, NULL);
		return;
	}

	/* feed: .../webfeed.aspx, .../webfeed, /RDWeb/Feed(/) */
	if (ends_with_ci(path, "/webfeed.aspx") || ends_with_ci(path, "/webfeed") ||
	    !strcasecmp(path, "/RDWeb/Feed") || !strcasecmp(path, "/RDWeb/Feed/")) {
		serve_feed(ssl, head, &ws, host);
	} else if (!strncasecmp(path, "/RDWeb/Feed/rdp/", 16) && ends_with_ci(path, ".rdp")) {
		char name[128];
		struct app *a;

		snprintf(name, sizeof name, "%.*s", (int)(strlen(path) - 16 - 4), path + 16);
		a = find_app(&ws, name);
		if (a)
			serve_rdp(ssl, head, &ws, host, a);
		else
			respond(ssl, head, 404, "text/plain", "not published\n", 14, NULL);
	} else if (!strncasecmp(path, "/RDWeb/Feed/icon/", 17) &&
		   (ends_with_ci(path, ".png") || ends_with_ci(path, ".ico"))) {
		char name[128];
		struct app *a;

		snprintf(name, sizeof name, "%.*s", (int)(strlen(path) - 17 - 4), path + 17);
		a = find_app(&ws, name);
		if (a)
			serve_icon(ssl, head, a, ends_with_ci(path, ".ico"));
		else
			respond(ssl, head, 404, "text/plain", "not published\n", 14, NULL);
	} else {
		respond(ssl, head, 404, "text/plain", "not found\n", 10, NULL);
	}
	free(ws.apps);
}

static void *
connection_thread(void *data)
{
	int fd = (int)(intptr_t)data;
	struct sockaddr_storage ss;
	socklen_t sl = sizeof ss;
	char rhost[INET6_ADDRSTRLEN] = "?";
	struct timeval tv = { .tv_sec = 15 };
	SSL *ssl;

	if (getpeername(fd, (struct sockaddr *)&ss, &sl) == 0) {
		if (ss.ss_family == AF_INET)
			inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, rhost, sizeof rhost);
		else
			inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ss)->sin6_addr, rhost, sizeof rhost);
	}
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	if (cfg.plain_http) {
		struct conn c = { .ssl = NULL, .fd = fd };

		handle(&c, rhost);
	} else {
		ssl = SSL_new(ssl_ctx);
		if (ssl) {
			struct conn c = { .ssl = ssl, .fd = fd };

			SSL_set_fd(ssl, fd);
			if (SSL_accept(ssl) == 1)
				handle(&c, rhost);
			SSL_shutdown(ssl);
			SSL_free(ssl);
		}
	}
	close(fd);
	return NULL;
}

int
main(int argc, char *argv[])
{
	static const struct option opts[] = {
		{ "port", required_argument, NULL, 'p' },
		{ "cert", required_argument, NULL, 'c' },
		{ "key", required_argument, NULL, 'k' },
		{ "apps", required_argument, NULL, 'a' },
		{ "http", no_argument, NULL, 'H' },
		{ "sign-cert", required_argument, NULL, 'S' },
		{ "sign-key", required_argument, NULL, 'K' },
		{ "listen", required_argument, NULL, 'l' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	struct sockaddr_in6 a6 = { .sin6_family = AF_INET6 };
	struct sockaddr_in a4 = { .sin_family = AF_INET };
	int lfd, on = 1, off = 0, opt;
	bool bound = false;

	while ((opt = getopt_long(argc, argv, "p:c:k:a:vh", opts, NULL)) != -1) {
		switch (opt) {
		case 'p': cfg.port = atoi(optarg); break;
		case 'c': cfg.cert = optarg; break;
		case 'k': cfg.key = optarg; break;
		case 'a': cfg.apps_conf = optarg; break;
		case 'H': cfg.plain_http = true; break;
		case 'S': cfg.sign_cert = optarg; break;
		case 'K': cfg.sign_key = optarg; break;
		case 'l': cfg.listen_addr = optarg; break;
		case 'v': cfg.verbose = true; break;
		default:
			fprintf(stderr,
				"usage: weston-rail-feed [--port=443] [--cert=FILE] [--key=FILE]\n"
				"                        [--apps=/etc/weston-rail/apps.conf] [-v]\n"
				"       behind a TLS reverse proxy (Caddy, nginx):\n"
				"       weston-rail-feed --http --listen=127.0.0.1 --port=8080\n"
				"       optional signing of the .rdp files (like rdpsign.exe):\n"
				"       --sign-cert=fullchain.pem --sign-key=key.pem\n");
			return opt == 'h' ? 0 : 2;
		}
	}
	signal(SIGPIPE, SIG_IGN);
	setvbuf(stderr, NULL, _IOLBF, 0);

	if ((cfg.sign_cert != NULL) != (cfg.sign_key != NULL)) {
		logmsg("--sign-cert and --sign-key belong together");
		return 1;
	}
	if (cfg.sign_cert)
		logmsg(".rdp files are signed with %s", cfg.sign_cert);

	if (!cfg.plain_http && (!(ssl_ctx = SSL_CTX_new(TLS_server_method())) ||
	    SSL_CTX_use_certificate_chain_file(ssl_ctx, cfg.cert) != 1 ||
	    SSL_CTX_use_PrivateKey_file(ssl_ctx, cfg.key, SSL_FILETYPE_PEM) != 1)) {
		logmsg("cannot load %s / %s (weston-rail-broker creates them on its first "
		       "start)", cfg.cert, cfg.key);
		return 1;
	}
	if (ssl_ctx)
		SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_2_VERSION);

	if (cfg.listen_addr && inet_pton(AF_INET, cfg.listen_addr, &a4.sin_addr) == 1) {
		lfd = -1;	/* IPv4 address given: straight to the IPv4 socket */
	} else if (cfg.listen_addr && inet_pton(AF_INET6, cfg.listen_addr, &a6.sin6_addr) != 1) {
		logmsg("invalid --listen address %s", cfg.listen_addr);
		return 1;
	} else {
		lfd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
	}
	if (lfd >= 0) {
		setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
		setsockopt(lfd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
		if (!cfg.listen_addr)
			a6.sin6_addr = in6addr_any;
		a6.sin6_port = htons(cfg.port);
		bound = bind(lfd, (struct sockaddr *)&a6, sizeof a6) == 0;
		if (!bound) {
			close(lfd);
			lfd = -1;
		}
	}
	if (lfd < 0) {
		lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (lfd < 0)
			return 1;
		setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
		if (!cfg.listen_addr)
			a4.sin_addr.s_addr = htonl(INADDR_ANY);
		a4.sin_port = htons(cfg.port);
		bound = bind(lfd, (struct sockaddr *)&a4, sizeof a4) == 0;
	}
	if (!bound || listen(lfd, 32) < 0) {
		logmsg("cannot listen on port %d: %s", cfg.port, strerror(errno));
		return 1;
	}
	if (cfg.plain_http)
		logmsg("weston-rail-feed listening on %s:%d (plain HTTP for a TLS reverse proxy), "
		       "feed path /RDWeb/Feed/webfeed.aspx",
		       cfg.listen_addr ? cfg.listen_addr : "*", cfg.port);
	else
		logmsg("weston-rail-feed listening on port %d, feed: https://<server>%s/RDWeb/Feed/webfeed.aspx",
		       cfg.port, cfg.port == 443 ? "" : ":<port>");

	for (;;) {
		pthread_t tid;
		int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);

		if (fd < 0)
			continue;
		if (pthread_create(&tid, NULL, connection_thread, (void *)(intptr_t)fd) != 0) {
			close(fd);
			continue;
		}
		pthread_detach(tid);
	}
}
