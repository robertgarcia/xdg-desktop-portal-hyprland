#!/usr/bin/env bash
# Activa el portal de Hyprland compilado desde este repo (rama
# poc-synergy-remotedesktop) en lugar del del sistema, solo para la sesión de
# usuario y sin tocar nada bajo /usr. Revertir con poc-off.sh.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$REPO/build/xdg-desktop-portal-hyprland"
STATE="${XDG_STATE_HOME:-$HOME/.local/state}/xdph-poc"
UNITS="$HOME/.config/systemd/user"

[[ -x $BIN ]] || { echo "No existe $BIN; compila primero (ver docs/synergy-omarchy.md)" >&2; exit 1; }

# Copia de los .portal del sistema, con el hyprland.portal del repo que declara
# RemoteDesktop y Clipboard
rm -rf "$STATE/portals"
mkdir -p "$STATE/portals"
cp /usr/share/xdg-desktop-portal/portals/*.portal "$STATE/portals/"
cp "$REPO/hyprland.portal" "$STATE/portals/hyprland.portal"

mkdir -p "$UNITS/xdg-desktop-portal.service.d" "$UNITS/xdg-desktop-portal-hyprland.service.d"
cat > "$UNITS/xdg-desktop-portal.service.d/poc-synergy.conf" <<CONF
# Creado por docs/poc/poc-on.sh. Borrar con poc-off.sh.
[Service]
Environment=XDG_DESKTOP_PORTAL_DIR=$STATE/portals
CONF
cat > "$UNITS/xdg-desktop-portal-hyprland.service.d/poc-synergy.conf" <<CONF
# Creado por docs/poc/poc-on.sh. Borrar con poc-off.sh.
[Service]
ExecStart=
# stdbuf: el logger no vacía std::cout y las líneas llegan tarde al journal
ExecStart=/usr/bin/stdbuf -oL $BIN --verbose
CONF

systemctl --user daemon-reload
systemctl --user restart xdg-desktop-portal-hyprland.service
systemctl --user restart xdg-desktop-portal.service
# Synergy no reabre su sesión de RemoteDesktop/EIS si el portal se reinicia
systemctl --user try-restart synergy-3.service
sleep 2

if busctl --user introspect org.freedesktop.portal.Desktop /org/freedesktop/portal/desktop 2>/dev/null | grep -q 'org.freedesktop.portal.RemoteDesktop'; then
    echo "POC activa: org.freedesktop.portal.RemoteDesktop disponible"
else
    echo "POC instalada pero RemoteDesktop NO aparece en el frontend; revisa:" >&2
    echo "  journalctl --user -u xdg-desktop-portal-hyprland -u xdg-desktop-portal -n 50" >&2
    exit 1
fi
