#!/usr/bin/env bash
#
# Installiert alle Build- und Laufzeitabhaengigkeiten fuer weston-mirror
# (RDP-Backend + rdprail-shell) unter Debian 13 "trixie".
#
# Aufruf:
#   ./scripts/install-deps.sh           # installiert (fragt sudo, falls noetig)
#   ./scripts/install-deps.sh --list    # zeigt nur die Paketliste
#   ./scripts/install-deps.sh --check   # zeigt, welche Pakete noch fehlen
#   ./scripts/install-deps.sh --force   # auch auf Nicht-Debian-13 installieren
#
set -euo pipefail

# Werkzeuge
BUILD_TOOLS=(
	build-essential
	git
	meson
	ninja-build
	pkgconf
)

# Bibliotheken (in Klammern: pkg-config-Name, den meson sucht)
BUILD_LIBS=(
	libwayland-dev          # wayland-server, wayland-client, wayland-cursor
	libwayland-bin          # wayland-scanner
	wayland-protocols       # wayland-protocols
	libpixman-1-dev         # pixman-1
	libcairo2-dev           # cairo, cairo-xcb
	libpango1.0-dev         # pango, pangocairo
	libpng-dev              # libpng
	libxkbcommon-dev        # xkbcommon
	libinput-dev            # libinput
	libevdev-dev            # libevdev
	libudev-dev             # libudev
	libdrm-dev              # libdrm (von libweston immer verlangt)
	libpam0g-dev            # pam
	libssl-dev              # openssl (Session-TLS-Zertifikat automatisch erzeugen)
	librsvg2-dev            # librsvg-2.0 (Icons in rdprail-shell)
	libglib2.0-dev          # glib-2.0 (rdprail-shell)
	freerdp3-dev            # freerdp3, freerdp-server3
	libwinpr3-dev           # winpr3
	libfuse3-dev            # fuse3 (Laufwerksumleitung)
	libx11-dev              # x11       (Xwayland-Support)
	libxcb1-dev             # xcb
	libxcb-composite0-dev   # xcb-composite
	libxcb-shape0-dev       # xcb-shape
	libxcb-xfixes0-dev      # xcb-xfixes
	libxcursor-dev          # xcursor
)

# Laufzeit
RUNTIME=(
	xwayland                # X11-Anwendungen als RemoteApp
	fuse3                   # fusermount3 fuer die Laufwerksumleitung
	cups                    # Druckerumleitung: Warteschlangen je Client-Drucker
	cups-client             # lpadmin, lpstat, lpoptions
	cups-filters            # Umwandlung der Druckdaten nach PDF/PostScript
	ghostscript             # PDF -> XPS (xpswrite) fuer mstsc-Drucker
	openssl                 # TLS-Zertifikat des Session-Brokers
	dbus-user-session       # dbus-run-session fuer die Sessions (Broker)
	adwaita-icon-theme      # Maus-Cursor und Symbole fuer die Anwendungen
	librsvg2-bin            # rsvg-convert: SVG-Symbole fuer den Arbeitsbereich-Feed
)

strip_comments() {
	for p in "$@"; do printf '%s\n' "${p%%#*}"; done | awk 'NF'
}

mapfile -t PKGS < <(strip_comments "${BUILD_TOOLS[@]}" "${BUILD_LIBS[@]}" "${RUNTIME[@]}")

missing_pkgs() {
	local p
	for p in "${PKGS[@]}"; do
		dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q "install ok installed" \
			|| printf '%s\n' "$p"
	done
}

FORCE=0
case "${1:-}" in
	--list)
		printf '%s\n' "${PKGS[@]}"
		exit 0
		;;
	--check)
		mapfile -t MISSING < <(missing_pkgs)
		if ((${#MISSING[@]} == 0)); then
			echo "Alle ${#PKGS[@]} Pakete sind installiert."
		else
			echo "Fehlende Pakete (${#MISSING[@]}):"
			printf '  %s\n' "${MISSING[@]}"
			exit 1
		fi
		exit 0
		;;
	--force)
		FORCE=1
		;;
	"")
		;;
	-h|--help)
		sed -n '3,12p' "$0" | sed 's/^# \{0,1\}//'
		exit 0
		;;
	*)
		echo "Unbekannte Option: $1 (siehe --help)" >&2
		exit 2
		;;
esac

# Distribution pruefen
if [[ -r /etc/os-release ]]; then
	# shellcheck disable=SC1091
	. /etc/os-release
fi
if [[ "${ID:-}" != "debian" || "${VERSION_ID:-}" != "13" ]]; then
	echo "Warnung: gebaut und getestet fuer Debian 13 (trixie), gefunden: ${PRETTY_NAME:-unbekannt}." >&2
	if ((FORCE == 0)); then
		echo "Mit --force trotzdem installieren." >&2
		exit 1
	fi
fi

SUDO=""
if ((EUID != 0)); then
	command -v sudo >/dev/null || { echo "Bitte als root ausfuehren oder sudo installieren." >&2; exit 1; }
	SUDO="sudo"
fi

mapfile -t MISSING < <(missing_pkgs)
if ((${#MISSING[@]} == 0)); then
	echo "Alle Abhaengigkeiten sind bereits installiert."
	exit 0
fi

echo "Installiere ${#MISSING[@]} Paket(e):"
printf '  %s\n' "${MISSING[@]}"

$SUDO apt-get update
$SUDO apt-get install -y --no-install-recommends "${MISSING[@]}"

echo
echo "Fertig. Weiter mit: ./build.sh"
