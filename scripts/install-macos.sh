#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

version="0.1"
source_library="${1:-build/libsdrplay_api.so.3.15}"
source_daemon="build/openrspd"
source_plist="packaging/macos/com.openrsp.openrspd.plist"
prefix="/Library/OpenRSP/$version"
loader_path="/opt/homebrew/lib/libsdrplay_api.dylib"

if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo: sudo $0 [path-to-libsdrplay_api.so.3.15]" >&2
    exit 1
fi
if [ ! -f "$source_library" ]; then
    echo "Built compatibility library not found: $source_library" >&2
    exit 1
fi
if [ ! -x "$source_daemon" ] || [ ! -f "$source_plist" ]; then
    echo "Built openrspd or LaunchDaemon plist is missing" >&2
    exit 1
fi
if launchctl print system/com.sdrplay.service >/dev/null 2>&1; then
    echo "Refusing to install while SDRplay's proprietary daemon is loaded." >&2
    echo "Uninstall the SDRplay API package first." >&2
    exit 1
fi
if [ -e "$loader_path" ] && [ ! -L "$loader_path" ]; then
    echo "Refusing to replace non-symlink library: $loader_path" >&2
    exit 1
fi

mkdir -p "$prefix/lib" "$prefix/bin" /opt/homebrew/lib
install -m 0755 "$source_library" "$prefix/lib/libsdrplay_api.so.3.15"
install -m 0755 "$source_daemon" "$prefix/bin/openrspd"
launchctl bootout system /Library/LaunchDaemons/com.openrsp.openrspd.plist >/dev/null 2>&1 || true
attempt=0
while launchctl print system/com.openrsp.openrspd >/dev/null 2>&1; do
    attempt=$((attempt + 1))
    [ "$attempt" -lt 40 ] || { echo "Timed out unloading existing openrspd" >&2; exit 1; }
    sleep 0.25
done
install -o root -g wheel -m 0644 "$source_plist" /Library/LaunchDaemons/com.openrsp.openrspd.plist
if [ -x "build/openrsp-reset" ]; then
    install -m 0755 "build/openrsp-reset" "$prefix/bin/openrsp-reset"
fi
ln -sfn "$prefix/lib/libsdrplay_api.so.3.15" "$loader_path"
attempt=0
until launchctl bootstrap system /Library/LaunchDaemons/com.openrsp.openrspd.plist 2>/dev/null; do
    attempt=$((attempt + 1))
    [ "$attempt" -lt 40 ] || { echo "Timed out loading openrspd" >&2; exit 1; }
    sleep 0.25
done
echo "Installed OpenRSP compatibility library: $prefix/lib/libsdrplay_api.so.3.15"
echo "Installed standalone driver service: $prefix/bin/openrspd"
echo "Loader path: $loader_path"
