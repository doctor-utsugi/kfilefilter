#!/bin/sh
# Builds kfilefilter and hooks it into the KDE file chooser portal.
# Everything lands under $HOME, so no package update can overwrite it.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
lib_dir="$HOME/.local/lib/kfilefilter"
unit=plasma-xdg-desktop-portal-kde.service
dropin_dir="$HOME/.config/systemd/user/$unit.d"

cmake -S "$here" -B "$here/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$here/build" --target kfilefilter -j

mkdir -p "$lib_dir" "$dropin_dir"
install -m 755 "$here/build/kfilefilter.so" "$lib_dir/kfilefilter.so"

cat > "$dropin_dir/kfilefilter.conf" <<CONF
# Installed by kfilefilter. Delete this file to turn the filter off.
[Service]
Environment=LD_PRELOAD=$lib_dir/kfilefilter.so
CONF

systemctl --user daemon-reload
systemctl --user try-restart "$unit"
echo "kfilefilter installed. The next file dialog opened through the portal has the filter."
