#!/bin/sh
# The wasi-sdk release (clang and wasm-ld for wasm32) that builds the web runtime's image decoder
# (web/decode). Its version and the tarballs' SHA-256 are pinned here only: web/decode/decode.mk
# runs this script to install it locally, and setup-zgal (beside it) to install it in CI.
#
#   wasi-sdk.sh version      prints the pinned version
#   wasi-sdk.sh install DIR  downloads the host's tarball from GitHub, checks its SHA-256 and unpacks
#                            it as DIR (DIR/bin/clang, ...), replacing nothing until it's complete
set -eu

VERSION=34.0
TAG=wasi-sdk-34

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

case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) host=arm64-macos sum=9c59398106b417f8f14913380fdf0097a8cc0ff4af9eb3ce0065a859e88d49e9 ;;
  Darwin-x86_64) host=x86_64-macos sum=87d27fa8adc68dee59bfbf2e22a6d34ef717c34d6bf1d8af2a56fc929d9ce0eb ;;
  Linux-x86_64) host=x86_64-linux sum=b761e3a0721dbae9c09a0059e5fdb2bf917d1b4a8a7b430fb3b5aafb0984b2c4 ;;
  Linux-aarch64 | Linux-arm64) host=arm64-linux sum=f7e243dff54d60bcc576e94d6166b69f410f2500ae4a9ceef34315be10e77971 ;;
  *)
    echo "wasi-sdk.sh: no wasi-sdk $VERSION for $(uname -s) $(uname -m)" >&2
    exit 1
    ;;
esac

name="wasi-sdk-$VERSION-$host"
url="https://github.com/WebAssembly/wasi-sdk/releases/download/$TAG/$name.tar.gz"

# Downloaded and unpacked beside DIR, then renamed, so an interrupted install leaves no partial SDK
mkdir -p "$(dirname "$dest")"
work=$(mktemp -d "$dest.XXXXXX")
trap 'rm -rf "$work"' EXIT

echo "wasi-sdk.sh: downloading $url"
# A stalled transfer (below 100 kB/s for 60 s) is abandoned and retried, as are other failures
curl -sSfL --retry 3 --retry-all-errors --connect-timeout 30 --speed-limit 100000 --speed-time 60 \
  -o "$work/$name.tar.gz" "$url"

if command -v sha256sum > /dev/null 2>&1; then
  got=$(sha256sum "$work/$name.tar.gz" | cut -d' ' -f1)
else
  got=$(shasum -a 256 "$work/$name.tar.gz" | cut -d' ' -f1)
fi
if [ "$got" != "$sum" ]; then
  echo "wasi-sdk.sh: $name.tar.gz's SHA-256 is $got, not $sum" >&2
  exit 1
fi

mkdir "$work/sdk"
tar -xzf "$work/$name.tar.gz" -C "$work/sdk" --strip-components=1
rm -rf "$dest"
mv "$work/sdk" "$dest"
echo "wasi-sdk.sh: installed wasi-sdk $VERSION in $dest"
