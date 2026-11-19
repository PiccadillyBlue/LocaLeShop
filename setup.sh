#!/usr/bin/env bash
set -e

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if command -v python3 >/dev/null 2>&1; then
    exec python3 "${ROOT_DIR}/setup.py" "$@"
elif command -v python >/dev/null 2>&1; then
    exec python "${ROOT_DIR}/setup.py" "$@"
else
    echo "Error: Python 3 is required to run the LocaLeShop setup wizard."
    echo "Install Python 3.9+ from https://www.python.org/ or your system package manager."
    exit 1
fi
