#!/bin/sh
# SPDX-FileCopyrightText: 2026 dvdavd
# SPDX-License-Identifier: MIT
#
# Builds a portable, statically linked diskvu-agent inside an Alpine (musl)
# container, so it runs on any x86-64/arm64 Linux server regardless of the
# CPU baseline or glibc version of the machine you build on (some distros,
# e.g. CachyOS, build their toolchain for x86-64-v3 and the result fails with
# "CPU ISA level is lower than required" on older servers).
#
# Usage: scripts/build-agent-static.sh [output-path]
#        ENGINE=podman scripts/build-agent-static.sh
#        ARCH=aarch64 scripts/build-agent-static.sh   # cross-build (needs qemu binfmt)
# ARCH is x86_64 or aarch64 (default: this machine's).
# Default output: build/agents/diskvu-agent-linux-<arch>, the name the
# "Install agent on host" feature looks for. Bundle it into diskvu with
#   cmake -DDISKVU_AGENT_BINARIES_DIR=build/agents ...
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
engine=${ENGINE:-}

arch=${ARCH:-$(uname -m)}
case "$arch" in
    x86_64|amd64)  arch=x86_64;  platform=linux/amd64 ;;
    aarch64|arm64) arch=aarch64; platform=linux/arm64 ;;
    *) echo "error: unsupported ARCH '$arch' (use x86_64 or aarch64)" >&2; exit 1 ;;
esac
out=${1:-"$root/build/agents/diskvu-agent-linux-$arch"}

if [ -z "$engine" ]; then
    for candidate in docker podman; do
        if command -v "$candidate" >/dev/null 2>&1; then engine=$candidate; break; fi
    done
fi
if [ -z "$engine" ]; then
    echo "error: neither docker nor podman found (set ENGINE=...)" >&2
    exit 1
fi

# Same version as the rest of the project (project(diskvu VERSION x.y.z ...)).
version=$(sed -n 's/^project(diskvu VERSION \([0-9][0-9.]*\).*/\1/p' "$root/CMakeLists.txt")
version=${version:-dev}

mkdir -p "$(dirname "$out")"
outdir=$(cd "$(dirname "$out")" && pwd)
outname=$(basename "$out")

"$engine" run --rm --platform "$platform" \
    -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" -e DVA_VERSION="$version" \
    -v "$root/agent":/src:ro \
    -v "$outdir":/out \
    alpine:3 sh -c "
        set -e
        apk add --no-cache g++ zstd-dev zstd-static >/dev/null
        g++ -std=c++17 -O2 -static -pthread -DDVA_HAVE_ZSTD -DDVA_VERSION=\\\"\$DVA_VERSION\\\" /src/diskvu-agent.cpp -o /out/$outname -lzstd
        strip /out/$outname
        chmod 755 /out/$outname
        chown "\$HOST_UID:\$HOST_GID" /out/$outname
    "

echo "Built $out"
file "$out" 2>/dev/null || true
echo "Bundle it:   cmake -DDISKVU_AGENT_BINARIES_DIR='$outdir' ... (then use Install... in the remote host dialog)"
echo "Or by hand:  scp '$out' user@server:~/.local/bin/diskvu-agent"
