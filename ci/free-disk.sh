#!/bin/bash
# GitHub runners start with ~14 GB free; the kernel and Wine trees need more.
sudo rm -rf /usr/share/dotnet /usr/local/lib/android /opt/ghc /usr/local/.ghcup \
    /opt/hostedtoolcache/CodeQL /usr/local/share/boost /usr/local/share/powershell
sudo docker image prune --all --force >/dev/null 2>&1 || true
df -h /
