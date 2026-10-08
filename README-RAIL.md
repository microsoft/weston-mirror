# weston-mirror als RemoteApp-Server (RAIL) unter Debian 13

Dieser Fork von [microsoft/weston-mirror](https://github.com/microsoft/weston-mirror)
soll einzelne Linux-Anwendungen per RDP als **RemoteApp** bereitstellen – ohne
Desktop dahinter, vergleichbar mit RemoteApp auf Windows Server.

Grundlage ist Weston 9 mit dem RDP-Backend und der `rdprail-shell`
(RAIL = *Remote Applications Integrated Locally*, MS-RDPERP). Jedes Fenster
einer Anwendung erscheint beim Client als eigenes lokales Fenster.

## Stand

Getestet mit Windows `mstsc` gegen einen Debian-13-LXC-Container auf Proxmox.

| Bereich | Status |
|---|---|
| RemoteApp mit `mstsc`: Fenster, Maus, Tastatur inkl. Umlaute | ja (Firefox, gedit, weston-terminal) |
| X11-Programme (Java/Swing, ältere Toolkits) | ja, die Sessions starten mit Xwayland |
| Mehrere Instanzen einer Anwendung | ja |
| Session pro User, parallel mehrere User | ja, `weston-rail-broker` |
| Anmeldung mit Passwortabfrage in mstsc (NLA) | ja, NTLM über NT-Hash-Datei |
| Active Directory (Kerberos) | ja, getestet mit Samba-AD (Funktionsebene 2003) und xfreerdp3 |
| Allowlist veröffentlichter Programme | ja, `/etc/weston-rail/apps.conf` |
| Laufwerke des Clients | ja: lesen, speichern, umbenennen, löschen (FUSE) |
| Drucken auf Client-Druckern | ja, über den Windows-Treiber des Clients (XPS) |
| Session endet nach der letzten App / Wiederverbinden | ja |
| Ton-Ausgabe | **fehlt** (die WSLg-PulseAudio-Senke gibt es unter Debian nicht) |
| Mikrofon (audin) | fehlt (unter FreeRDP 3 noch nicht portiert) |
| Passwortwechsel → NT-Hash automatisch nachziehen | ja, PAM-Modul `pam_weston_rail.so` |
| Linux-Client `xfreerdp3` | startet, Fensterinhalt bleibt schwarz (Client-Problem) |
| Arbeitsbereich (Feed) für Windows-Startmenü und Windows App | ja, `weston-rail-feed` (siehe unten) |
| App-Liste an den Client publizieren (`rdpapplist`) | nicht verfügbar (Microsoft-eigener Kanal) |

### Voraussetzungen im Proxmox-LXC

Unter Optionen → Features **nesting** (für systemd-logind: Login-Sessions,
`/run/user/<uid>`) und **FUSE** (Laufwerksumleitung) aktivieren, danach den
Container neu starten.

## Schnellstart

```bash
git clone https://github.com/zwiebelchen/weston-mirror.git
cd weston-mirror

./scripts/install-deps.sh      # Abhängigkeiten installieren
./build.sh                     # konfigurieren + bauen
./build.sh install             # nach /usr/local installieren (sudo)

sudo nano /etc/weston-rail/apps.conf          # veröffentlichte Programme
sudo weston-rail-passwd $USER                 # NLA-Anmeldung einrichten
sudo systemctl daemon-reload
sudo systemctl enable --now weston-rail-broker
```

Danach mit der `.rdp`-Datei aus dem Abschnitt Session-Broker verbinden.

### install-deps.sh

| Aufruf | Wirkung |
|---|---|
| `./scripts/install-deps.sh` | installiert alle fehlenden Pakete |
| `./scripts/install-deps.sh --list` | zeigt die komplette Paketliste |
| `./scripts/install-deps.sh --check` | zeigt, welche Pakete noch fehlen |
| `./scripts/install-deps.sh --force` | auch auf anderen Distributionen als Debian 13 |

Benötigte Pakete (Stand Debian 13):

- **Werkzeuge:** `build-essential git meson ninja-build pkgconf`
- **Wayland:** `libwayland-dev libwayland-bin wayland-protocols`
- **Grafik/Text:** `libpixman-1-dev libcairo2-dev libpango1.0-dev libpng-dev librsvg2-dev`
- **Eingabe:** `libxkbcommon-dev libinput-dev libevdev-dev libudev-dev`
- **Sonstiges:** `libdrm-dev libpam0g-dev libssl-dev libglib2.0-dev`
- **RDP:** `freerdp3-dev libwinpr3-dev`
- **Laufwerke:** `libfuse3-dev fuse3`
- **Drucken:** `cups cups-client cups-filters ghostscript`
- **Xwayland (X11-Apps):** `libx11-dev libxcb1-dev libxcb-composite0-dev libxcb-shape0-dev libxcb-xfixes0-dev libxcursor-dev`
- **Laufzeit:** `xwayland openssl`

### build.sh

| Aufruf | Wirkung |
|---|---|
| `./build.sh` | konfiguriert (falls nötig) und baut |
| `./build.sh install` | baut und installiert (sudo, danach `ldconfig`) |
| `./build.sh reconfigure` | meson-Optionen neu anwenden, dann bauen |
| `./build.sh clean` | Build-Verzeichnis löschen |
| `./build.sh rebuild` | clean + bauen |

Umgebungsvariablen: `PREFIX` (Standard `/usr/local`), `BUILD_DIR` (`build`),
`BUILDTYPE` (`debugoptimized`), `JOBS`. Ein geänderter `PREFIX` braucht
`PREFIX=... ./build.sh reconfigure`.

Nach dem ersten Lauf geht auch direkt `ninja -C build`.

Gebaut wird nur, was ein headless RemoteApp-Server braucht: RDP-Backend,
rdprail-shell, pixman-Renderer (kein GL, keine GPU nötig) und Xwayland.

## Session-Broker: eine Session pro User

Für den Mehrbenutzerbetrieb startet man Weston nicht mehr selbst, sondern
den Broker. Er läuft als root auf Port 3389:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now weston-rail-broker
journalctl -u weston-rail-broker -f          # Log des Brokers
```

Ablauf:

1. mstsc verbindet sich, der Broker prüft Benutzername und Passwort per PAM
   (Dienst `weston-rail`, `/etc/pam.d/weston-rail`) gegen die Linux-Konten.
2. Der Broker leitet den Client mit einem Einmal-Token (60 s gültig) auf sich
   selbst um. mstsc verbindet sich sofort neu, das sieht man nicht.
3. Die neue Verbindung geht an die Weston-Instanz des Users. Hat er noch
   keine, startet der Broker sie unter seinem Konto: eigene PAM-/logind-
   Session, `/run/user/<uid>`, eigener D-Bus, Locale aus `/etc/default/locale`.
4. Weitere Verbindungen desselben Users (zweite App, Wiederverbinden)
   landen in derselben Instanz. Andere User bekommen ihre eigene.
5. Ist kein Client mehr verbunden und keine App mehr offen, endet die
   Session nach 60 s (`--idle-exit=SEK`).

`.rdp`-Datei für den Broker:

```
full address:s:SERVER:3389
remoteapplicationmode:i:1
remoteapplicationprogram:s:||firefox
remoteapplicationname:s:Firefox
enablecredsspsupport:i:1
prompt for credentials:i:1
authentication level:i:0
disableconnectionsharing:i:1
drivestoredirect:s:*
redirectprinters:i:1
```

### Fehlersuche bei Anwendungen

Alles, was ein Programm auf stdout/stderr schreibt, steht im Log seiner
Session, zusammen mit den Meldungen von Weston:

```bash
sudo tail -f /run/user/$(id -u BENUTZER)/weston-rail.log
```

Dort steht auch, was gestartet wurde:
`RDP RAIL: 'firefox' allowed, starting: /usr/bin/firefox`.

Startet ein Programm gar nicht, hilft ein Wrapper-Skript als `command`, das
Umgebung setzt und die Ausgabe zusätzlich wegschreibt:

```sh
#!/bin/sh
exec >>"$HOME/meine-app.log" 2>&1
echo "--- Start $(date), DISPLAY=$DISPLAY WAYLAND_DISPLAY=$WAYLAND_DISPLAY"
exec /pfad/zur/anwendung "$@"
```

X11-Programme (Java/Swing, Motif, ältere Toolkits) brauchen Xwayland; der
Broker startet die Sessions damit (abschaltbar mit `--no-xwayland`). Ohne
Xwayland fehlt `DISPLAY`, und solche Programme beenden sich sofort und
kommentarlos. Paket `xwayland` muss installiert sein.

### Sessions verwalten

```bash
sudo weston-rail-sessions                 # Tabelle: User, Beginn, verbunden/getrennt, Programme
sudo weston-rail-sessions --json          # dasselbe als JSON (für Verwaltungsprogramme)
sudo weston-rail-sessions --logoff lars   # Session beenden (Weston und seine Programme)
```

Übergabe ohne Umleitung für Clients, die der RDP-Umleitung nicht folgen
(z. B. `guacd`): `CONNECT <user> [<uid>]` startet die Session und öffnet
einen einmaligen Port auf 127.0.0.1, der in der Antwort steht (`PORT <n>`).
Die erste Verbindung dorthin wird an die Session übergeben, danach ist der
Port zu. Mit `<uid>` darf nur dieser lokale Benutzer verbinden. Der Aufrufer
muss den Benutzer vorher selbst authentifiziert haben – die
Verwaltungsschnittstelle ist root vorbehalten.

Eine *getrennte* Session läuft mit ihren Programmen weiter, bis der User sich
wieder verbindet; sie endet von selbst, sobald keine Programme mehr laufen.
Schnittstelle: `/run/weston-rail-broker.sock` (nur root), Befehle `LIST` und
`LOGOFF <user>`.

### Anmeldung: NLA oder ohne NLA

Der Broker kann beides. Mit **NLA** fragt mstsc wie bei Windows-RDS selbst
nach Benutzer und Passwort (`enablecredsspsupport:i:1`,
`prompt for credentials:i:1`, kein `username`). Ohne NLA schickt mstsc nur
gespeicherte Zugangsdaten mit (`cmdkey`).

NLA prüft die Anmeldung per NTLM oder Kerberos, bevor eine Session entsteht.
Danach übergibt mstsc das Passwort, das der Broker zusätzlich per PAM prüft
(gesperrte oder abgelaufene Konten kommen nicht durch). Woher der Server
weiß, ob das Passwort stimmt:

**Ohne Domäne – NT-Hash-Datei.** NTLM braucht den NT-Hash des Passworts, den
Linux nicht hat. `weston-rail-passwd` legt ihn in `/etc/weston-rail/ntlm.sam`
ab (nur root lesbar; ein NT-Hash ist so schützenswert wie ein Passwort):

```bash
sudo weston-rail-passwd lars     # prüft das Passwort gegen das Linux-Passwort
sudo weston-rail-passwd -l       # eingetragene User
sudo weston-rail-passwd -d lars  # entfernen
```

Automatisch aktuell halten mit dem PAM-Modul `pam_weston_rail.so`: Es trägt
den NT-Hash bei jeder Linux-Anmeldung mit Passwort ein (SSH, Konsole, `su`,
Broker-Anmeldung ohne NLA) und zieht ihn bei jedem Passwortwechsel nach. Es
beeinflusst das Ergebnis der Anmeldung nie. Jeweils ans **Ende** der Dateien
anhängen:

```bash
M=/usr/local/lib/x86_64-linux-gnu/security/pam_weston_rail.so
echo "auth	optional	$M" | sudo tee -a /etc/pam.d/common-auth
echo "password	optional	$M" | sudo tee -a /etc/pam.d/common-password
```

(Nicht direkt hinter die `pam_unix`-Zeile setzen: deren `success=1` würde
sonst die falsche Zeile überspringen.)

**Mit Active Directory – Kerberos.** Der Container tritt der Domäne bei
(sssd), ein Dienstkonto trägt den Dienstnamen `TERMSRV/<Name, mit dem die
Clients verbinden>`, dessen Keytab liegt in `/etc/weston-rail/krb5.keytab`:

```bash
# Container: DNS auf den DC, dann Beitritt
sudo apt install realmd sssd sssd-tools adcli krb5-user libnss-sss libpam-sss oddjob oddjob-mkhomedir
sudo realm join -U Administrator ad.example.org
#   /etc/sssd/sssd.conf: use_fully_qualified_names = False, default_shell = /bin/bash
sudo pam-auth-update --enable mkhomedir

# DC (Samba): Dienstkonto, Dienstname, Keytab
samba-tool user create svc-rdp --random-password
samba-tool user setexpiry svc-rdp --noexpiry
samba-tool spn add TERMSRV/rdp.example.org svc-rdp
samba-tool domain exportkeytab /root/rdp.keytab --principal=TERMSRV/rdp.example.org
#   -> nach /etc/weston-rail/krb5.keytab im Container (root, 600)
```

Ein eigenes Dienstkonto statt des Computerkontos, weil sssd dessen Passwort
regelmäßig ändert und die Keytab dann veraltet. Heißen die Linux-Konten
`lars@realm` (vollqualifizierte Namen), dem Broker `--user-map=upn` mitgeben.

Hinweise aus dem Test:
- Bei Funktionsebene 2000/2003 erzeugt Samba für Konten nur RC4-Schlüssel
  (kein AES). Windows 10 akzeptiert das; bei neueren Windows-Versionen kann
  RC4 abgeschaltet sein, dann weicht die Anmeldung auf NTLM aus.
- Die Verbindung nach der Umleitung läuft immer per NTLM mit dem Hash des
  gerade angemeldeten Users; die Domäne darf NTLM nicht komplett verbieten.
- Die Meldung `krb5_get_init_creds_keytab (Client 'TERMSRV/…' not found)` im
  Broker-Log ist harmlos.

Beides lässt sich kombinieren: Kerberos für Domänen-Clients, die NT-Hash-Datei
für alle anderen. Broker-Optionen: `--sam`, `--keytab`, `--user-map`,
`--no-nla` (nur ohne NLA) und `--nla-only` (Anmeldung ohne NLA verbieten).
Beim Start meldet der Broker im Log, welche Verfahren aktiv sind.

### Zertifikat

Beim ersten Start erzeugt der Broker `/etc/weston-rail/tls.crt/.key`
(selbstsigniert), gültig für Rechnername, vollqualifizierten Namen, alle
IP-Adressen und die mit `--name` angegebenen Namen. Jede Session bekommt eine
Kopie, damit der Client nur ein Zertifikat sieht.

Externen DNS-Namen aufnehmen:

```bash
sudo systemctl edit weston-rail-broker
#   [Service]
#   Environment=BROKER_ARGS=--name=rdp.example.org
sudo rm /etc/weston-rail/tls.crt /etc/weston-rail/tls.key
sudo systemctl restart weston-rail-broker
```

Damit Windows dem selbstsignierten Zertifikat vertraut, `tls.crt` auf den
Client kopieren und als Administrator importieren (oder per GPO verteilen):

```
certutil -addstore Root tls.crt
```

Alternativ ein Zertifikat einer eigenen oder öffentlichen CA (z. B. Let's
Encrypt) als `tls.crt`/`tls.key` ablegen.

Log einer Session: `/run/user/<uid>/weston-rail.log`.

Drucken im Mehrbenutzerbetrieb: Jeder User braucht die Gruppe `lpadmin`,
damit seine Session Warteschlangen anlegen darf.

Optionen: `weston-rail-broker --help` (Port, Zertifikat, Weston-Pfad,
Leerlaufzeit). Für Tests ohne Broker funktioniert der direkte Start von
Weston wie oben weiterhin.

## Arbeitsbereich: Programme im Startmenü und in der Windows App

`weston-rail-feed` stellt die veröffentlichten Programme aus
`/etc/weston-rail/apps.conf` als Arbeitsbereich im Format von RD Web Access
bereit – mit Namen, Symbolen und fertigen Verbindungsdateien:

```bash
sudo systemctl enable --now weston-rail-feed      # HTTPS auf Port 443
```

Feed-URL: `https://SERVER/RDWeb/Feed/webfeed.aspx`

| Client | Einrichten |
|---|---|
| Windows 10/11 | Systemsteuerung → **RemoteApp- und Desktopverbindungen** → „Auf RemoteApp und Desktops zugreifen“ → Feed-URL. Die Programme erscheinen im Startmenü unter „Arbeitsressourcen“ und werden automatisch aktualisiert. Per GPO verteilbar: Benutzerkonfiguration → Administrative Vorlagen → Windows-Komponenten → Remotedesktopdienste → RemoteApp- und Desktopverbindungen → „Standardverbindungs-URL angeben“. |
| Windows App auf macOS, iOS/iPadOS, Android/ChromeOS | **+** → **Arbeitsbereich hinzufügen** → Feed-URL oder E-Mail-Adresse (siehe unten) |

Die **Windows App unter Windows** kann derzeit keine RDS-Arbeitsbereiche
abonnieren (Microsoft unterstützt das dort nur für Azure Virtual Desktop und
Windows 365). Unter Windows übernimmt das die eingebaute Funktion
„RemoteApp- und Desktopverbindungen“.

Wichtig:

- **Windows verlangt für den Feed ein vertrauenswürdiges HTTPS-Zertifikat**
  (die mobilen Apps können ein unbekanntes Zertifikat akzeptieren). Ein
  eigenes Zertifikat, z. B. Let's Encrypt, per
  `sudo systemctl edit weston-rail-feed` →
  `Environment="FEED_ARGS=--cert=/pfad/fullchain.pem --key=/pfad/privkey.pem"`.
- **Hinter einem Reverse Proxy** (Caddy, nginx) mit Let's-Encrypt-Zertifikat
  spricht der Feed nur HTTP auf localhost, TLS macht der Proxy (siehe
  „Feed hinter Caddy“).
- Der Feed braucht keine Anmeldung, er listet nur die veröffentlichten
  Programme. Angemeldet wird beim Start eines Programms (NLA am Broker).
- Einstellungen im Abschnitt `[workspace]` der `apps.conf`: `name`
  (angezeigter Name), `address` (Server für die Verbindung, Standard: der Name,
  unter dem der Feed abgerufen wurde), `authentication-level`, zusätzliche
  `.rdp`-Zeilen mit `rdp=…`. Pro Programm `title` und `icon` (Name aus dem
  Icon-Theme oder Pfad; ohne Angabe aus der `.desktop`-Datei des Programms).
- Anmeldung per E-Mail-Adresse in der Windows App: DNS-TXT-Eintrag
  `_msradc.example.org` mit dem Wert `https://SERVER/RDWeb/Feed`.

### Feed hinter Caddy (Let's Encrypt)

Der Feed lauscht dann nur auf localhost und ohne eigenes TLS; Caddy belegt
Port 443 und holt das Zertifikat:

```bash
sudo systemctl edit weston-rail-feed
#   [Service]
#   Environment="FEED_ARGS=--http --listen=127.0.0.1 --port=8080"
sudo systemctl restart weston-rail-feed
```

`/etc/caddy/Caddyfile`:

```
desktop.example.org {
	reverse_proxy /RDWeb/* 127.0.0.1:8080
}
```

Caddy reicht den Hostnamen durch, die Links im Feed zeigen damit auf
`https://desktop.example.org/…`. Hinter anderen Proxys wird auch
`X-Forwarded-Host` ausgewertet. Die RDP-Verbindung selbst geht weiterhin
direkt an Port 3389 des Brokers, nicht über Caddy.

Das Let's-Encrypt-Zertifikat kann auch der Broker verwenden; dann vertraut
mstsc auch der RDP-Verbindung (`authentication-level=2` möglich). Der Broker
liest das Zertifikat bei jeder Anmeldung neu, Verlängerungen durch Caddy
greifen also von selbst:

```bash
D=/var/lib/caddy/.local/share/caddy/certificates/acme-v02.api.letsencrypt.org-directory/desktop.example.org
sudo systemctl edit weston-rail-broker
#   [Service]
#   Environment="BROKER_ARGS=--cert=$D/desktop.example.org.crt --key=$D/desktop.example.org.key"
```

(`$D` in der Unit ausgeschrieben eintragen. Enthält die Zeile Leerzeichen,
die ganze Zuweisung in Anführungszeichen setzen: `Environment="…"`, sonst
ignoriert systemd alles nach dem ersten Leerzeichen.)

### .rdp-Dateien signieren (optional)

Ohne Signatur zeigt Windows beim Start „Unbekannter Herausgeber“ und schaltet
Laufwerke, Zwischenablage und Drucker standardmäßig ab. Mit Signatur steht
dort der Name aus dem Zertifikat, die Auswahl lässt sich merken, und per GPO
kann die Warnung ganz entfallen.

Signiert wird wie mit Microsofts `rdpsign.exe`, mit einem Zertifikat samt
Schlüssel (PEM), z. B. demselben Let's-Encrypt-Zertifikat wie für den
Broker. Ohne diese Optionen bleiben die Dateien unsigniert:

```bash
sudo systemctl edit weston-rail-feed
#   [Service]
#   Environment="FEED_ARGS=--http --listen=10.10.10.244 --port=8080 --sign-cert=/pfad/fullchain.pem --sign-key=/pfad/key.pem"
sudo systemctl restart weston-rail-feed
```

Zertifikat und Schlüssel werden bei jedem Abruf neu gelesen; ein
verlängertes Zertifikat gilt also ohne Neustart. Danach unter „RemoteApp- und
Desktopverbindungen“ einmal „Jetzt aktualisieren“.

Warnung per GPO abschalten: Computer- oder Benutzerkonfiguration →
Administrative Vorlagen → Windows-Komponenten → Remotedesktopdienste →
Remotedesktopverbindungs-Client → „SHA1-Fingerabdrücke von Zertifikaten
angeben, die vertrauenswürdige RDP-Herausgeber darstellen“. Fingerabdruck:

```bash
openssl x509 -in /pfad/fullchain.pem -noout -fingerprint -sha1 | tr -d ':' 
```

Ohne diesen Eintrag fragt Windows bei jedem Start nach, auch wenn
„Meine Auswahl … merken“ angehakt ist (getestet mit Windows 11). Zum Testen
auf einem einzelnen Rechner genügt der Registry-Wert, den die GPO setzt:

```
reg add "HKCU\Software\Policies\Microsoft\Windows NT\Terminal Services" /v TrustedCertThumbprints /t REG_SZ /d FINGERABDRUCK /f
```

Bei Let's Encrypt ändert sich der Fingerabdruck mit jeder Verlängerung (etwa
alle 60–90 Tage) und müsste jedes Mal nachgezogen werden. Für den Betrieb
daher besser ein eigenes, lange gültiges Zertifikat nur zum Signieren
verwenden (`--sign-cert`/`--sign-key` sind unabhängig vom TLS-Zertifikat)
und dessen Fingerabdruck einmal per GPO verteilen.

## Allowlist: veröffentlichte Programme

Ein Client kann nur Programme starten, die in `/etc/weston-rail/apps.conf`
stehen (`./build.sh install` legt beim ersten Mal eine Beispieldatei an).
Die Datei wird bei jedem Start neu gelesen, Änderungen gelten sofort.

```ini
[app]
name=firefox
command=/usr/bin/firefox
client-arguments=false

[app]
name=writer
command=/usr/bin/libreoffice --writer
client-arguments=true
```

In der `.rdp`-Datei wird das Programm über seinen Namen angefordert, wie bei
Windows-RemoteApps: `remoteapplicationprogram:s:||firefox`. Der Pfad aus
`command` (`/usr/bin/firefox`) wird ebenfalls akzeptiert. `command` wird ohne
Shell ausgeführt; Argumente mit Leerzeichen in Anführungszeichen setzen.
Argumente aus der `.rdp`-Datei (`remoteapplicationcmdline:s:…`) werden nur
mit `client-arguments=true` angehängt, sonst ignoriert.

Alles andere lehnt der Server ab (mstsc zeigt dann, dass das Programm nicht
in der Liste der zulässigen Programme steht). Nur zum Testen lässt sich die
Prüfung mit `WESTON_RAIL_ALLOW_ANY_PROGRAM=1` abschalten – dann kann jeder
Client z. B. eine Shell starten. Andere Datei: `WESTON_RAIL_APPS_CONF=/pfad`.

## Laufwerksumleitung

Die freigegebenen Laufwerke des Clients erscheinen in der Session unter
`~/RDP-Laufwerke/<Laufwerk>` (z. B. `~/RDP-Laufwerke/C`), anderer Ort über
`WESTON_RDP_DRIVES_DIR`, abschalten mit `WESTON_RDP_DISABLE_DRIVES=1`.
Anwendungen öffnen und speichern dort ganz normal.

Voraussetzungen: Paket `fuse3` und Zugriff auf `/dev/fuse`. Im Proxmox-LXC:
Optionen → Features → **FUSE** aktivieren.

Einschränkungen: Dateien über 4 GB, Kürzen auf eine andere Größe als 0,
Zeitstempel und Rechte setzen werden nicht unterstützt (letzteres wird
stillschweigend akzeptiert, damit `cp -p` & Co. funktionieren).

Der rdpdr-Server stammt aus FreeRDP 3.15 und liegt korrigiert unter
`libweston/backend-rdp/rdpdr/` (fünf Fehler behoben, die Laufwerke in
FreeRDP unbenutzbar machen; Details im Dateikopf).

## Druckerumleitung

Für jeden Drucker, den der Client meldet (`redirectprinters:i:1`), legt die
Session eine CUPS-Warteschlange an: `rdp-<user>-<Druckername>`, Beschreibung
„<Druckername> (<Client>)“. Sie erscheint in jedem Druckdialog (GTK, Firefox,
LibreOffice, `lp`). Der Standarddrucker des Clients wird Standard des Users.
Beim Trennen werden die Warteschlangen wieder entfernt.

Druckweg: Anwendung → CUPS (PDF) → Filter `rdpxps` (Ghostscript `xpswrite`)
→ Backend `rdpprint` → Session → rdpdr → Client. Drucker, die mstsc mit
`XPSFORMAT` meldet, bekommen XPS und werden vom Windows-Treiber des Clients
gedruckt – unabhängig vom Druckermodell. Andere Drucker bekommen generisches
PostScript. Erzwingen mit `WESTON_RDP_PRINT_FORMAT=xps` bzw. `ps`,
abschalten mit `WESTON_RDP_DISABLE_PRINTERS=1`.

Einmalig einrichten:

```bash
sudo usermod -aG lpadmin $USER     # darf Warteschlangen anlegen; danach neu anmelden
sudo systemctl restart cups        # nach ./build.sh install (neuer MIME-Typ)
```

`./build.sh install` legt das Backend nach `/usr/lib/cups/backend/rdpprint`
(root, 0700 – es muss in das private Laufzeitverzeichnis des Users) und den
Filter nach `/usr/lib/cups/filter/rdpxps`. Das Backend liefert nur an einen
Socket in `/run/user/<uid>` des Auftrags-Users, der diesem User gehört.

## Sicherheit – bitte lesen

Der Stand ist jung; vor einem Einsatz im offenen Netz noch einmal prüfen.

- Betrieb nur über den **Session-Broker**. Beim direkten Start von Weston
  gibt es keine Anmeldung: Wer den Port erreicht, bekommt eine Session des
  startenden Users.
- Mit NLA (`--nla-only`) prüft der Server die Anmeldung, bevor eine Session
  entsteht. Ohne NLA gehen die Zugangsdaten TLS-verschlüsselt im
  Client-Info-PDU über die Leitung.
- Mit dem selbstsignierten Zertifikat und `authentication level:i:0` prüft
  mstsc die Identität des Servers nicht. Für den Betrieb ein
  vertrauenswürdiges Zertifikat und `authentication level:i:2` verwenden.
- `/etc/weston-rail/ntlm.sam` enthält NT-Hashes (so schützenswert wie
  Passwörter, nur root lesbar).
- Startbar sind nur Programme aus der Allowlist. Ein Terminal gehört nicht
  hinein, sonst hat jeder Client eine Shell.

## Ohne Broker: Weston direkt starten (Test)

```bash
export XDG_RUNTIME_DIR=/run/user/$(id -u)
weston --backend=rdp-backend.so --shell=rdprail-shell.so --port=3389 \
       --logger-scopes=log,rdp-backend,rdprail-shell
```

`XDG_RUNTIME_DIR` muss auf ein existierendes Verzeichnis zeigen. Fehlt
`/run/user/<uid>` (z. B. im LXC-Container ohne Login-Session):
`sudo loginctl enable-linger $USER` oder das Verzeichnis von Hand anlegen
(Besitzer = User, Rechte 700).

Eine UTF-8-Locale setzen (`export LANG=de_DE.UTF-8` bzw. `C.UTF-8`), sonst
zeigen Terminal-Anwendungen keine Umlaute an. Grafische Anwendungen wie
Firefox sind davon nicht betroffen.

Ohne `--rdp-tls-cert`/`--rdp-tls-key` erzeugt Weston beim Start ein
selbstsigniertes Zertifikat. Mehr Debug-Ausgaben: `WESTON_RDP_DEBUG_LEVEL=4`.

Client (Linux):

```bash
xfreerdp3 /v:SERVER:3389 /app:program:'||terminal' /cert:ignore
```

Client (Windows, empfohlen): `.rdp`-Datei mit

```
full address:s:SERVER:3389
remoteapplicationmode:i:1
remoteapplicationprogram:s:||firefox
remoteapplicationname:s:Firefox
enablecredsspsupport:i:0
authentication level:i:0
prompt for credentials:i:0
```

`enablecredsspsupport:i:0` ist nötig, weil der Server (noch) kein NLA kann.

Für Laufwerke und Drucker zusätzlich:

```
drivestoredirect:s:*
redirectprinters:i:1
```

## Vorgemerkt

- **Ton-Ausgabe**: eigene Audio-Anbindung (PipeWire/PulseAudio), da die
  WSLg-Senke unter Debian fehlt; danach Mikrofon (audin für FreeRDP 3)
- NTLM-Rückfall für AD-Benutzer: NT-Hashes direkt vom Samba-DC beziehen
  statt aus `ntlm.sam`
- **Verwaltungs-Snap-in für ice2k** (veröffentlichte Programme, Sessions,
  Broker-Dienst) auf Basis von `weston-rail-sessions --json` und `apps.conf`
- Let's-Encrypt-Zertifikat automatisch von der OPNsense in den Container
  übertragen (ACME-Automatisierung per SFTP)

## Änderungen gegenüber microsoft/weston-mirror

- Port auf die FreeRDP-3-API (Zertifikate, RFX, Clipboard, Tastatur, WinPR),
  Laufzeitfehler mit FreeRDP 3 behoben (drdynvc, Aktivierung, Fenster-Orders)
- RAIL: mehrere Instanzen, Abmelden nach der letzten App, Übernahme der
  Session durch eine neue Verbindung, Allowlist, Programmstart mit Argumenten
- Geräteumleitung (rdpdr): Laufwerke per FUSE, Drucker per CUPS (XPS);
  korrigierte Kopie des FreeRDP-rdpdr-Servers unter `libweston/backend-rdp/rdpdr/`
- Session-Broker `weston-rail-broker` (PAM, NLA, Umleitung, eine Weston-Instanz
  pro User), `weston-rail-passwd`, `pam_weston_rail.so`, `weston-rail-sessions`
- Arbeitsbereich-Feed `weston-rail-feed` (RemoteApp- und Desktopverbindungen,
  Windows App)
- audin unter FreeRDP 3 als Stub, Xwayland mit `-listenfd`
- Build- und Installationsskripte
