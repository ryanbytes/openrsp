#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

prefix="/Library/OpenRSP/0.1"
loader_path="/opt/homebrew/lib/libsdrplay_api.dylib"

if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo: sudo $0" >&2
    exit 1
fi
launchctl bootout system /Library/LaunchDaemons/com.openrsp.openrspd.plist >/dev/null 2>&1 || true
rm -f /Library/LaunchDaemons/com.openrsp.openrspd.plist /var/run/openrspd.sock
if [ -L "$loader_path" ]; then
    target=$(readlink "$loader_path")
    case "$target" in
        /Library/OpenRSP/*) rm "$loader_path" ;;
        *) echo "Leaving unrelated symlink untouched: $loader_path -> $target" >&2 ;;
    esac
fi
rm -rf "$prefix"
echo "OpenRSP 0.1 removed. USB hardware state was not changed."
