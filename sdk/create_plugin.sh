#!/bin/bash
#
# create_plugin.sh — scaffold a new xInsp2 plugin in $PWD/<name>/.
#
# Usage:
#   cd /path/to/your/plugins/folder
#   sh /path/to/xinsp2/sdk/create_plugin.sh [name] [extra scaffold args]
#
# Examples:
#   sh /opt/xinsp2/sdk/create_plugin.sh foo
#   sh ./xInsp2/sdk/create_plugin.sh my_detector --force
#
# If <name> is omitted you'll be prompted.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCAFFOLD="$SCRIPT_DIR/scaffold.mjs"

if [ ! -f "$SCAFFOLD" ]; then
    echo "ERROR: scaffold.mjs not found at $SCAFFOLD" >&2
    exit 2
fi

NAME="$1"
shift || true

if [ -z "$NAME" ]; then
    read -p "plugin name (snake_case): " NAME
fi
if [ -z "$NAME" ]; then
    echo "ERROR: name is required" >&2
    exit 2
fi
if ! echo "$NAME" | grep -qE '^[a-z][a-z0-9_]*$'; then
    echo "ERROR: name must match [a-z][a-z0-9_]* (got '$NAME')" >&2
    exit 2
fi

OUT="$PWD/$NAME"

if [ -e "$OUT" ] && [ "$#" -eq 0 ]; then
    echo "ERROR: $OUT already exists. Pass --force to overwrite." >&2
    exit 2
fi

node "$SCAFFOLD" "$OUT" --name "$NAME" "$@"
