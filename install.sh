#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Zov & Sklad Installer for Linux and macOS (x86_64 / aarch64)
set -eu

REPO="abit-foggy/zov"
VERSION="${ZOV_VERSION:-latest}"
PREFIX="${PREFIX:-/usr/local}"
INSTALL_ZOVC=1
INSTALL_SKLAD=1
FROM_DIR=""

while [ $# -gt 0 ]; do
    case "$1" in
        --zovc-only|--only-zovc)
            INSTALL_ZOVC=1
            INSTALL_SKLAD=0
            ;;
        --sklad-only|--only-sklad)
            INSTALL_ZOVC=0
            INSTALL_SKLAD=1
            ;;
        --prefix)
            shift
            PREFIX="$1"
            ;;
        --version)
            shift
            VERSION="$1"
            ;;
        --from-dir)
            shift
            FROM_DIR="$1"
            ;;
        -h|--help)
            echo "usage: install.sh [options]"
            echo ""
            echo "options:"
            echo "  --zovc-only              install only zovc (compiler, stdlib, runtime)"
            echo "  --sklad-only             install only sklad (package & toolchain manager)"
            echo "  --prefix <dir>           installation prefix (default: /usr/local)"
            echo "  --version <tag>          release version to install (default: latest)"
            echo "  --from-dir <dir>         install from local directory containing tarballs"
            echo "  -h, --help               display this help message"
            exit 0
            ;;
        *)
            echo "error: unknown option '$1'" >&2
            exit 1
            ;;
    esac
    shift
done

OS_RAW="$(uname -s)"
ARCH_RAW="$(uname -m)"

case "$OS_RAW" in
    Linux)  OS="linux" ;;
    Darwin) OS="macos" ;;
    *)
        echo "error: unsupported operating system '$OS_RAW'" >&2
        exit 1
        ;;
esac

case "$ARCH_RAW" in
    x86_64|amd64)   ARCH="x86_64" ;;
    aarch64|arm64)  ARCH="aarch64" ;;
    *)
        echo "error: unsupported architecture '$ARCH_RAW'" >&2
        exit 1
        ;;
esac

TARGET="${OS}-${ARCH}"

if [ -n "$FROM_DIR" ]; then
    case "$FROM_DIR" in
        /*) ;;
        *)  FROM_DIR="$(cd "$FROM_DIR" && pwd)" ;;
    esac
elif [ "$VERSION" = "latest" ]; then
    BASE_URL="https://github.com/${REPO}/releases/latest/download"
else
    BASE_URL="https://github.com/${REPO}/releases/download/${VERSION}"
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT INT TERM

can_write() {
    target="$1"
    while [ ! -d "$target" ] && [ "$target" != "/" ] && [ "$target" != "." ]; do
        target="$(dirname "$target")"
    done
    [ -w "$target" ]
}

need_sudo=""
if ! can_write "$PREFIX"; then
    if command -v sudo >/dev/null 2>&1; then
        need_sudo="sudo"
    fi
fi

install_archive() {
    pkg="$1"
    archive="${pkg}-${TARGET}.tar.gz"

    if [ -n "$FROM_DIR" ]; then
        src_path="${FROM_DIR}/${archive}"
        if [ ! -f "$src_path" ]; then
            echo "error: archive not found: ${src_path}" >&2
            exit 1
        fi
        echo "==> installing ${pkg} from ${src_path}..."
        cp "$src_path" "${TMPDIR}/${archive}"
    else
        url="${BASE_URL}/${archive}"
        echo "==> downloading ${archive} for ${TARGET}..."
        curl -fsSL "$url" -o "${TMPDIR}/${archive}"
    fi

    mkdir -p "${TMPDIR}/${pkg}"
    tar -xzf "${TMPDIR}/${archive}" -C "${TMPDIR}/${pkg}"
    $need_sudo mkdir -p "${PREFIX}/bin"
    $need_sudo cp -R "${TMPDIR}/${pkg}/"* "${PREFIX}/"
    echo "==> installed ${pkg} to ${PREFIX}"
}

if [ "$INSTALL_ZOVC" -eq 1 ]; then
    install_archive "zovc"
fi

if [ "$INSTALL_SKLAD" -eq 1 ]; then
    install_archive "sklad"
fi

echo "==> done! ensure ${PREFIX}/bin is in your PATH."
