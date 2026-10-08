#!/usr/bin/env bash
#
# Einfacher Build-Wrapper fuer weston-mirror (RDP-Backend + rdprail-shell).
#
# Aufruf:
#   ./build.sh                 # konfigurieren (falls noetig) und bauen
#   ./build.sh install         # bauen und nach $PREFIX installieren (sudo)
#   ./build.sh reconfigure     # meson-Optionen neu anwenden, dann bauen
#   ./build.sh clean           # Build-Verzeichnis loeschen
#   ./build.sh rebuild         # clean + bauen
#
# Umgebungsvariablen:
#   BUILD_DIR   Build-Verzeichnis            (Standard: build)
#   PREFIX      Installationspraefix         (Standard: /usr/local)
#               Aendern erfordert: PREFIX=... ./build.sh reconfigure
#   BUILDTYPE   meson buildtype              (Standard: debugoptimized)
#   JOBS        parallele ninja-Jobs         (Standard: ninja entscheidet)
#
# Nach dem ersten Lauf geht auch direkt:  ninja -C build
#
set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")"

BUILD_DIR="${BUILD_DIR:-build}"
PREFIX="${PREFIX:-/usr/local}"
BUILDTYPE="${BUILDTYPE:-debugoptimized}"

# Nur was fuer einen headless RemoteApp-Server gebraucht wird:
# RDP-Backend, rdprail-shell, pixman-Renderer, Xwayland.
MESON_OPTS=(
	--prefix="$PREFIX"
	--buildtype="$BUILDTYPE"
	-Dbackend-rdp=true
	-Dbackend-default=rdp
	-Dbackend-headless=true
	-Dbackend-drm=false
	-Dbackend-drm-screencast-vaapi=false
	-Dbackend-wayland=false
	-Dbackend-x11=false
	-Dbackend-fbdev=false
	-Drenderer-gl=false
	-Dshell-rdprail=true
	# fuer Browser-Sitzungen (weston-rail-web): eine Anwendung bildschirm-
	# fuellend (kiosk) oder mit Fensterverwaltung (desktop)
	-Dshell-kiosk=true
	-Dshell-desktop=true
	-Dshell-fullscreen=false
	-Dshell-ivi=false
	-Dlauncher-logind=false
	-Dxwayland=true
	-Dremoting=false
	-Dpipewire=false
	-Dsystemd=false
	-Dwslgd=false
	-Dcolor-management-lcms=false
	-Dcolor-management-colord=false
	-Dimage-jpeg=false
	-Dimage-webp=false
	-Dsimple-clients=
	-Ddemo-clients=false
	-Dtest-junit-xml=false
)

NINJA_OPTS=()
[[ -n "${JOBS:-}" ]] && NINJA_OPTS+=(-j "$JOBS")

need() {
	command -v "$1" >/dev/null 2>&1 || {
		echo "Fehlt: $1 -- zuerst ./scripts/install-deps.sh ausfuehren." >&2
		exit 1
	}
}

configure() {
	need meson
	need ninja
	if [[ -f "$BUILD_DIR/build.ninja" ]]; then
		meson setup --reconfigure "$BUILD_DIR" "${MESON_OPTS[@]}"
	else
		meson setup "$BUILD_DIR" "${MESON_OPTS[@]}"
	fi
}

build() {
	# immer konfigurieren: sonst greifen neue Optionen aus diesem Skript
	# nach einem "git pull" nicht, und Module wie kiosk-shell.so fehlen
	configure
	ninja -C "$BUILD_DIR" "${NINJA_OPTS[@]}"
	echo
	echo "Build fertig:"
	echo "  $BUILD_DIR/compositor/weston"
	echo "  $BUILD_DIR/libweston/backend-rdp/rdp-backend.so"
	echo "  $BUILD_DIR/rdprail-shell/rdprail-shell.so"
}

case "${1:-build}" in
	build)
		build
		;;
	install)
		build
		SUDO=""
		((EUID != 0)) && SUDO="sudo"
		$SUDO ninja -C "$BUILD_DIR" install
		$SUDO ldconfig
		INSTALLED_PREFIX_TMP="$(meson configure "$BUILD_DIR" 2>/dev/null | awk '$1=="prefix"{print $2; exit}')"
		if [[ ! -e /etc/weston-rail/apps.conf ]]; then
			$SUDO install -D -m 644 "${INSTALLED_PREFIX_TMP:-$PREFIX}/share/weston-rail/apps.conf.example" \
				/etc/weston-rail/apps.conf
			echo "Allowlist angelegt: /etc/weston-rail/apps.conf (bitte anpassen)"
		fi
		INSTALLED_PREFIX="$(meson configure "$BUILD_DIR" 2>/dev/null | awk '$1=="prefix"{print $2; exit}')"
		echo "Installiert nach ${INSTALLED_PREFIX:-$PREFIX}"
		;;
	reconfigure)
		configure
		build
		;;
	clean)
		rm -rf "$BUILD_DIR"
		echo "$BUILD_DIR entfernt."
		;;
	rebuild)
		rm -rf "$BUILD_DIR"
		build
		;;
	-h|--help|help)
		sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'
		;;
	*)
		echo "Unbekanntes Kommando: $1 (siehe ./build.sh --help)" >&2
		exit 2
		;;
esac
