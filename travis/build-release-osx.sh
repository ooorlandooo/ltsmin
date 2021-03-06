#!/bin/bash
set -e
#set -o xtrace

mkdir "$HOME/static-libs"
cp "$HOME/ltsmin-deps/lib/libzmq.a" "$HOME/static-libs"
cp "$HOME/ltsmin-deps/lib/libczmq.a" "$HOME/static-libs"
cp /usr/local/lib/libgmp.a "$HOME/static-libs"
cp /usr/local/lib/libpopt.a "$HOME/static-libs"

libxml2_version=$(brew list --versions libxml2 | cut -d' ' -f2)
cp "/usr/local/Cellar/libxml2/$libxml2_version/lib/libxml2.a" \
    "$HOME/static-libs"

export LTSMIN_LDFLAGS="-Wl,-search_paths_first"
# libiconv is necessary for libpopt
export LTSMIN_LDFLAGS="$LTSMIN_LDFLAGS -L$HOME/static-libs -weak-liconv"
export LTSMIN_CFLAGS=""
export LTSMIN_CXXFLAGS=""
export STRIP_FLAGS=""
export MCRL2_LIB_DIR="/mCRL2.app/Contents"

. travis/build-release-generic.sh --disable-mcrl2-jittyc

set +e

