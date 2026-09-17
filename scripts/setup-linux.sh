#!/usr/bin/env bash
set -euo pipefail
[[ $(uname -s) == Linux ]] || { echo 'Requires Linux' >&2; exit 1; }
sudo apt-get update
sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y \
  build-essential cmake ninja-build clang llvm libbpf-dev libelf-dev zlib1g-dev \
  linux-tools-common "linux-tools-$(uname -r)" pkg-config curl git python3
test -r /sys/kernel/btf/vmlinux
mkdir -p .local
sudo bpftool feature probe kernel > .local/kernel-features.txt
