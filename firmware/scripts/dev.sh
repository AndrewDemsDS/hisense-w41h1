#!/usr/bin/env bash
# dev.sh -- thin shim kept for the name users already type (CLAUDE.md, #119). The one entry point
# is now the portable dev.py; this just forwards to it so `dev.sh ...` and `dev.py ...` are equal.
exec python3 "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/dev.py" "$@"
