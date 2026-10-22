#!/bin/bash
set -eu

if [ "$(/usr/sbin/sysctl -n hw.optional.arm64)" != 1 ]; then
    echo 'ScenePad requires an Apple Silicon Mac.' >&2
    exit 1
fi
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source="$base/obs-scenepad.plugin"
destination="$HOME/Library/Application Support/obs-studio/plugins"
if [ "$#" -eq 2 ] && [ "$1" = "--copy-to" ]; then
    destination=$2
elif [ "$#" -ne 0 ]; then
    echo 'Usage: install_OBS-ScenePad.sh [--copy-to directory]' >&2
    exit 1
elif pgrep -x OBS >/dev/null; then
    echo 'Close OBS before installing ScenePad.' >&2
    exit 1
fi
if [ ! -f "$source/Contents/MacOS/obs-scenepad" ]; then
    echo 'Extract the complete ZIP before running this installer.' >&2
    exit 1
fi
mkdir -p "$destination"
target="$destination/obs-scenepad.plugin"
/usr/bin/ditto "$source" "$target"
# Remove quarantine from the installed plugin only.
/usr/bin/xattr -dr com.apple.quarantine "$target"
echo "ScenePad installed: $target"
