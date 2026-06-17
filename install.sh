#!/usr/bin/env bash
# txsyxts-player — build & install
set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
DIM='\033[2m'
NC='\033[0m'

echo ""
echo -e "${DIM}┌─────────────────────────────────┐${NC}"
echo -e "${DIM}│${NC}  txsyxts — build & install       ${DIM}│${NC}"
echo -e "${DIM}└─────────────────────────────────┘${NC}"
echo ""

# check deps
ok=true
for dep in cmake g++ pkg-config; do
    if command -v "$dep" &>/dev/null; then
        echo -e "  ${GREEN}✓${NC} $dep"
    else
        echo -e "  ${RED}✗${NC} $dep not found"
        ok=false
    fi
done

# check libraries
for lib in mpv libcurl; do
    if pkg-config --exists "$lib" 2>/dev/null; then
        echo -e "  ${GREEN}✓${NC} $lib"
    else
        echo -e "  ${RED}✗${NC} $lib not found"
        ok=false
    fi
done

# optional libs (webkit2gtk for browser login)
if pkg-config --exists webkit2gtk-4.1 2>/dev/null; then
    echo -e "  ${GREEN}✓${NC} webkit2gtk-4.1 (browser login)"
else
    echo -e "  ${DIM}·${NC} webkit2gtk-4.1 not found (manual cookie login only)"
fi

# optional runtime
for opt in yt-dlp ytfzf mpv; do
    if command -v "$opt" &>/dev/null; then
        echo -e "  ${GREEN}✓${NC} $opt (runtime)"
    else
        echo -e "  ${DIM}·${NC} $opt not found (optional runtime dep)"
    fi
done

if [ "$ok" = false ]; then
    echo ""
    echo -e "${RED}missing build dependencies. install them first:${NC}"
    echo "  sudo apt install build-essential cmake libmpv-dev libcurl4-openssl-dev"
    exit 1
fi

echo ""
echo "building..."
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -5
make -j$(nproc) 2>&1

echo ""
if [ -f txsyxts ]; then
    echo -e "${GREEN}build successful!${NC}"
    [ -f txsyxts-login ] && echo -e "  ${GREEN}✓${NC} login helper built (browser login enabled)"
    echo ""
    echo "install to /usr/local/bin? (needs sudo)"
    read -rp "  [y/N] " yn
    if [[ "$yn" =~ ^[Yy]$ ]]; then
        sudo cp txsyxts /usr/local/bin/txsyxts
        [ -f txsyxts-login ] && sudo cp txsyxts-login /usr/local/bin/txsyxts-login
        echo -e "${GREEN}installed!${NC} run: txsyxts"
    else
        echo "  binary at: ./build/txsyxts"
        echo "  install manually: sudo cp build/txsyxts build/txsyxts-login /usr/local/bin/"
    fi
else
    echo -e "${RED}build failed${NC}"
    exit 1
fi
