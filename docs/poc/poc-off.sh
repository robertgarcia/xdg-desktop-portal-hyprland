#!/usr/bin/env bash
# Revierte poc-on.sh: vuelve al portal de Hyprland del sistema.
set -euo pipefail

UNITS="$HOME/.config/systemd/user"
rm -f "$UNITS/xdg-desktop-portal.service.d/poc-synergy.conf" \
      "$UNITS/xdg-desktop-portal-hyprland.service.d/poc-synergy.conf"
rmdir "$UNITS/xdg-desktop-portal.service.d" "$UNITS/xdg-desktop-portal-hyprland.service.d" 2>/dev/null || true

systemctl --user daemon-reload
systemctl --user restart xdg-desktop-portal-hyprland.service
systemctl --user restart xdg-desktop-portal.service
echo "POC desactivada: portal del sistema restaurado"
