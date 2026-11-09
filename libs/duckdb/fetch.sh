#!/usr/bin/env bash
# Copyright (c) 2026 The Sea Authors
# SPDX-License-Identifier: BSD-3-Clause

# Download the DuckDB C library (libduckdb.so + duckdb.h) into vendor/.
# The version and the SHA-256 of each release archive are pinned here.
set -euo pipefail

VERSION=1.5.6
cd "$(dirname "$0")"

case "$(uname -m)" in
    x86_64|amd64)  ARCH=amd64; SHA=b845005f5132a7d8180057c35e14a7626632258782f871a90861b19c1c03841b ;;
    aarch64|arm64) ARCH=arm64; SHA=b72ed9f05003f5e9d2015f7ceada6416b377d9dd33169cdde3c9e33897856eee ;;
    *) echo "fetch.sh: no prebuilt DuckDB for $(uname -m); build it and use make DUCKDB_INC=... DUCKDB_LIB=..." >&2; exit 1 ;;
esac

ZIP="libduckdb-linux-$ARCH.zip"
URL="https://github.com/duckdb/duckdb/releases/download/v$VERSION/$ZIP"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "fetching DuckDB $VERSION ($ARCH)..."
if command -v curl >/dev/null; then curl -fsSL -o "$TMP/$ZIP" "$URL"
else wget -q -O "$TMP/$ZIP" "$URL"; fi

echo "$SHA  $TMP/$ZIP" | sha256sum -c --quiet - || { echo "fetch.sh: checksum mismatch" >&2; exit 1; }

mkdir -p vendor
if command -v unzip >/dev/null; then unzip -o -q "$TMP/$ZIP" libduckdb.so duckdb.h -d vendor
elif command -v python3 >/dev/null; then
    python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2], ["libduckdb.so", "duckdb.h"])' "$TMP/$ZIP" vendor
else echo "fetch.sh: needs unzip or python3 to extract $ZIP" >&2; exit 1; fi
echo "$VERSION" > vendor/VERSION
echo "DuckDB $VERSION in $(pwd)/vendor"
