#!/usr/bin/env bash
# Run the HTTP/3 0-RTT replay acceptance suite from an httpd build tree.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# The pyhttpd suite starts the in-tree httpd, generates certificates, and
# exercises three independent resumed clients plus time-window/capacity cases.
# curl is included automatically when the curl on PATH has HTTP/3 and
# SSLS-EXPORT support.
exec make check-pytest \
    PYTEST_DIRS=test/modules/http3/test_210_early_replay.py \
    "$@"
