#!/usr/bin/env bash
#
# Developer convenience wrapper: rebuild srthub, the container image and the web
# app after a code change, without redoing the system package phase.
#
# All of the real work lives in setup.sh. Third-party libraries (libsrt,
# libcurl, FFmpeg) are left alone unless their build output is missing; delete
# the cbsrt/ cblibcurl/ cbffmpeg/ directories to force a full rebuild.
#
set -Eeuo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

echo "srthub: rebuilding (skipping system packages)"
make clean
exec ./setup.sh --skip-deps "$@"
