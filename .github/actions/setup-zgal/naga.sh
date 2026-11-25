#!/bin/sh
# naga (naga-cli, a Rust crate), which translates the SPIR-V glslc compiles from GLSL shaders into
# WGSL for ZGAL's web backend (make test-web also validates all generated WGSL with it, as Firefox
# validates WGSL with its own naga, whose version may differ). Its version is pinned here only: zgal.mk
# runs this script to build it locally, and setup-zgal (beside it) to build it in CI. Building it
# needs cargo (Rust's build tool, from rustup); the crate's own lockfile pins its dependencies.
#
#   naga.sh version      prints the pinned version
#   naga.sh install DIR  builds naga-cli with cargo into DIR (DIR/bin/naga)
set -eu

VERSION=30.0.1

usage() {
  echo "usage: $0 version | install DIR" >&2
  exit 2
}

[ $# -ge 1 ] || usage
case "$1" in
  version)
    [ $# -eq 1 ] || usage
    echo "$VERSION"
    exit 0
    ;;
  install)
    [ $# -eq 2 ] || usage
    dest=$2
    ;;
  *) usage ;;
esac

if ! command -v cargo > /dev/null 2>&1; then
  echo "naga.sh: building naga $VERSION needs cargo (Rust's build tool: install rustup, https://rustup.rs)" >&2
  exit 1
fi

# cargo installs the binary once it's built, so an interrupted install leaves no partial one
echo "naga.sh: building naga-cli $VERSION with cargo"
cargo install naga-cli --version "$VERSION" --locked --root "$dest"
echo "naga.sh: installed naga $VERSION in $dest/bin/naga"
