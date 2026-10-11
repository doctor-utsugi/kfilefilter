#!/bin/sh
set -eu

unit=plasma-xdg-desktop-portal-kde.service
rm -f "$HOME/.config/systemd/user/$unit.d/kfilefilter.conf"
rmdir "$HOME/.config/systemd/user/$unit.d" 2>/dev/null || true
rm -rf "$HOME/.local/lib/kfilefilter"

systemctl --user daemon-reload
systemctl --user try-restart "$unit"
echo "kfilefilter removed."
