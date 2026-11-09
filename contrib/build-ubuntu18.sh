#!/usr/bin/env bash
# Build the FlapX daemon (flapxd) on Ubuntu 18.04 (bionic), e.g. for an older
# seed-node VM that can't be upgraded yet.
#
# Ubuntu 18.04 only ships Boost 1.65 and FlapX needs Boost 1.66 or newer, so
# this script builds Boost 1.74 (the same version as Ubuntu 22.04) from source
# and links it in statically. Everything else comes from Ubuntu 18.04:
# OpenSSL 1.1.1, Berkeley DB 5.3 and miniupnpc.
#
# The resulting flapxd links Boost, Berkeley DB, OpenSSL and miniupnpc
# statically, so on the VM it only needs the C/C++ runtime (glibc 2.27 or
# newer). It also runs on newer Ubuntu releases.
#
# Usage (as root, or a user with sudo, on Ubuntu 18.04):
#   contrib/build-ubuntu18.sh
#   SKIP_DEPS=1 contrib/build-ubuntu18.sh    # don't run apt-get
#   BOOST_PREFIX=/opt/boost-1.74 contrib/build-ubuntu18.sh   # where Boost goes
#   USE_UPNP=0 contrib/build-ubuntu18.sh     # UPnP built in but off by default
#                                            # (default here: 1, on)
#
# Without Ubuntu 18.04 at hand, the same build runs in Docker:
#   docker run --rm -v "$PWD":/src -w /src ubuntu:18.04 contrib/build-ubuntu18.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BOOST_VERSION=1.74.0
BOOST_DIRNAME="boost_${BOOST_VERSION//./_}"
BOOST_PREFIX="${BOOST_PREFIX:-$REPO_ROOT/depends-ubuntu18/boost-$BOOST_VERSION}"
UPNP="${USE_UPNP:-1}"
JOBS="$(nproc)"

SUDO=""
if [ "$(id -u)" != "0" ]; then SUDO="sudo"; fi

if [ -z "${SKIP_DEPS:-}" ]; then
    echo "==> Installing build dependencies"
    export DEBIAN_FRONTEND=noninteractive
    $SUDO apt-get update
    # Note: no libboost-*-dev here; Boost is built below
    $SUDO apt-get install -y --no-install-recommends \
        build-essential ca-certificates wget bzip2 file \
        libssl-dev libdb++-dev libminiupnpc-dev zlib1g-dev
fi

if [ ! -f "$BOOST_PREFIX/include/boost/version.hpp" ]; then
    echo "==> Building Boost $BOOST_VERSION (static) into $BOOST_PREFIX"
    WORK="$(mktemp -d)"
    wget -q -O "$WORK/boost.tar.bz2" \
        "https://archives.boost.io/release/$BOOST_VERSION/source/$BOOST_DIRNAME.tar.bz2"
    tar -xjf "$WORK/boost.tar.bz2" -C "$WORK"
    (
        cd "$WORK/$BOOST_DIRNAME"
        ./bootstrap.sh --with-libraries=system,filesystem,program_options,thread,chrono \
            --prefix="$BOOST_PREFIX"
        ./b2 -j"$JOBS" -d0 link=static runtime-link=shared threading=multi \
            variant=release cxxflags=-fPIC cflags=-fPIC install
    )
    rm -rf "$WORK"
fi

echo "==> Building flapxd (USE_UPNP=$UPNP)"
cd "$REPO_ROOT/src"
make -f makefile.unix -j"$JOBS" USE_UPNP="$UPNP" STATIC=1 \
    BOOST_INCLUDE_PATH="$BOOST_PREFIX/include" BOOST_LIB_PATH="$BOOST_PREFIX/lib"
strip flapxd

echo
file flapxd
ldd flapxd || true
echo
echo "==> Done: $REPO_ROOT/src/flapxd"
