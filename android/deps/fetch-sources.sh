#!/bin/zsh
set -eu
cd "${0:A:h}"
mkdir -p sources
fetch() {
 local archive=$1 url=$2 hash=$3 dir=$4
 if [[ ! -f sources/$archive ]]; then
  curl --fail --location --proto '=https' --retry 3 "$url" -o "sources/$archive.part"
  mv "sources/$archive.part" "sources/$archive"
 fi
 echo "$hash  sources/$archive" | shasum -a 256 -c -
 if [[ ! -d sources/$dir ]]; then tar xf "sources/$archive" -C sources; fi
}
fetch curl.tar.xz https://curl.se/download/curl-8.12.1.tar.xz 0341f1ed97a26c811abaebd37d62b833956792b7607ea3f15d001613c76de202 curl-8.12.1
fetch slirp.tar.gz https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v4.9.0/libslirp-v4.9.0.tar.gz e744a32767668fe80e3cb3bd75d10d501f981e98c26a1f318154a97e99cdac22 libslirp-v4.9.0
fetch lzfse.tar.gz https://github.com/lzfse/lzfse/archive/refs/tags/lzfse-1.0.tar.gz cf85f373f09e9177c0b21dbfbb427efaedc02d035d2aade65eb58a3cbf9ad267 lzfse-lzfse-1.0
fetch lz4.tar.gz https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.tar.gz 537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b lz4-1.10.0
