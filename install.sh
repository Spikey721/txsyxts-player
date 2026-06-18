#!/usr/bin/env bash
# txsyxts-player — full dependency install, build & install
set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
DIM='\033[2m'
BOLD='\033[1m'
NC='\033[0m'

# ── banner ──────────────────────────────────────────────────────────────────
echo ""
echo -e "${DIM}┌─────────────────────────────────────────────┐${NC}"
echo -e "${DIM}│${NC}  ${BOLD}txsyxts${NC} — dependency installer & builder    ${DIM}│${NC}"
echo -e "${DIM}└─────────────────────────────────────────────┘${NC}"
echo ""

# ── detect distro ────────────────────────────────────────────────────────────
detect_distro() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        echo "${ID}"
    elif command -v apt-get &>/dev/null; then
        echo "debian"
    elif command -v pacman &>/dev/null; then
        echo "arch"
    elif command -v dnf &>/dev/null; then
        echo "fedora"
    elif command -v zypper &>/dev/null; then
        echo "opensuse"
    else
        echo "unknown"
    fi
}

DISTRO=$(detect_distro)
echo -e "  ${DIM}detected distro:${NC} ${BLUE}${DISTRO}${NC}"
echo ""

# ── install system build + lib dependencies ──────────────────────────────────
install_deps() {
    echo -e "${BOLD}[1/3] Installing build & library dependencies...${NC}"
    echo ""

    case "${DISTRO}" in
        ubuntu|debian|linuxmint|pop|elementary|kali|raspbian)
            echo -e "  ${DIM}→ using apt${NC}"
            sudo apt-get update -qq
            sudo apt-get install -y \
                build-essential \
                cmake \
                git \
                pkg-config \
                libmpv-dev \
                libcurl4-openssl-dev \
                mpv \
                python3-pip \
                libwebkit2gtk-4.1-dev \
                libgtk-3-dev 2>/dev/null || \
            sudo apt-get install -y \
                build-essential \
                cmake \
                git \
                pkg-config \
                libmpv-dev \
                libcurl4-openssl-dev \
                mpv \
                python3-pip
            ;;
        arch|manjaro|endeavouros|garuda|artix)
            echo -e "  ${DIM}→ using pacman${NC}"
            sudo pacman -Sy --needed --noconfirm \
                base-devel \
                cmake \
                git \
                pkgconf \
                mpv \
                curl \
                python-pip \
                webkit2gtk-4.1 \
                gtk3 2>/dev/null || \
            sudo pacman -Sy --needed --noconfirm \
                base-devel cmake git pkgconf mpv curl python-pip
            ;;
        fedora|rhel|centos|almalinux|rocky)
            echo -e "  ${DIM}→ using dnf${NC}"
            sudo dnf install -y \
                @development-tools \
                cmake \
                git \
                pkgconfig \
                mpv-libs-devel \
                libcurl-devel \
                mpv \
                python3-pip \
                webkit2gtk4.1-devel \
                gtk3-devel 2>/dev/null || \
            sudo dnf install -y \
                @development-tools cmake git pkgconfig \
                mpv-libs-devel libcurl-devel mpv python3-pip
            ;;
        opensuse*|suse*)
            echo -e "  ${DIM}→ using zypper${NC}"
            sudo zypper install -y -t pattern devel_basis
            sudo zypper install -y \
                cmake \
                git \
                pkg-config \
                mpv-devel \
                libcurl-devel \
                mpv \
                python3-pip \
                webkit2gtk3-devel 2>/dev/null || \
            sudo zypper install -y \
                cmake git pkg-config mpv-devel libcurl-devel mpv python3-pip
            ;;
        *)
            echo -e "  ${YELLOW}⚠ unknown distro '${DISTRO}' — skipping auto-install${NC}"
            echo -e "  ${DIM}you may need to manually install:${NC}"
            echo -e "    build-essential / base-devel, cmake, git, pkg-config"
            echo -e "    libmpv-dev, libcurl-dev, mpv, python3-pip"
            echo ""
            ;;
    esac

    echo ""
}

# ── install yt-dlp ────────────────────────────────────────────────────────────
install_ytdlp() {
    echo -e "${BOLD}[2/3] Installing yt-dlp (runtime)...${NC}"
    echo ""

    if command -v yt-dlp &>/dev/null; then
        echo -e "  ${GREEN}✓${NC} yt-dlp already installed ($(yt-dlp --version 2>/dev/null || echo 'unknown version'))"
        echo -e "  ${DIM}→ upgrading to latest...${NC}"
        pip3 install --upgrade yt-dlp --break-system-packages 2>/dev/null \
            || pip3 install --upgrade yt-dlp 2>/dev/null \
            || true
    else
        echo -e "  ${DIM}→ installing via pip...${NC}"
        pip3 install yt-dlp --break-system-packages 2>/dev/null \
            || pip3 install yt-dlp 2>/dev/null \
            || {
                # fallback: install binary directly
                echo -e "  ${DIM}→ pip failed, trying binary download...${NC}"
                sudo curl -L https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp \
                    -o /usr/local/bin/yt-dlp
                sudo chmod a+rx /usr/local/bin/yt-dlp
            }
    fi

    if command -v yt-dlp &>/dev/null; then
        echo -e "  ${GREEN}✓${NC} yt-dlp $(yt-dlp --version 2>/dev/null)"
    else
        echo -e "  ${YELLOW}⚠ yt-dlp install may have failed — youtube streaming may not work${NC}"
    fi
    echo ""
}

# ── verify all deps ──────────────────────────────────────────────────────────
verify_deps() {
    echo -e "${BOLD}Verifying dependencies...${NC}"
    echo ""
    ok=true

    # build tools
    for dep in cmake g++ git pkg-config; do
        if command -v "$dep" &>/dev/null; then
            echo -e "  ${GREEN}✓${NC} $dep"
        else
            echo -e "  ${RED}✗${NC} $dep not found"
            ok=false
        fi
    done

    # pkg-config libs
    for lib in mpv libcurl; do
        if pkg-config --exists "$lib" 2>/dev/null; then
            echo -e "  ${GREEN}✓${NC} $lib ($(pkg-config --modversion "$lib" 2>/dev/null))"
        else
            echo -e "  ${RED}✗${NC} $lib dev headers not found"
            ok=false
        fi
    done

    # optional: webkit2gtk
    if pkg-config --exists webkit2gtk-4.1 2>/dev/null; then
        echo -e "  ${GREEN}✓${NC} webkit2gtk-4.1 (browser login enabled)"
    else
        echo -e "  ${DIM}·${NC} webkit2gtk-4.1 not found (manual sp_dc login only)"
    fi

    # runtime tools
    for opt in mpv yt-dlp; do
        if command -v "$opt" &>/dev/null; then
            echo -e "  ${GREEN}✓${NC} $opt (runtime)"
        else
            echo -e "  ${YELLOW}⚠${NC} $opt not found (some features will not work)"
        fi
    done

    echo ""
    if [ "$ok" = false ]; then
        echo -e "${RED}✗ some required build dependencies are still missing.${NC}"
        echo -e "  Please install them manually and re-run this script."
        exit 1
    fi
}

# ── build ─────────────────────────────────────────────────────────────────────
build_project() {
    echo -e "${BOLD}[3/3] Building txsyxts...${NC}"
    echo ""
    echo -e "  ${DIM}→ configuring with cmake (Release)...${NC}"
    mkdir -p build
    cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 | grep -E "(STATUS|error|warning|Building)" || true
    echo -e "  ${DIM}→ compiling (using $(nproc) cores)...${NC}"
    make -j"$(nproc)" 2>&1
    cd ..
}

# ── install binary ─────────────────────────────────────────────────────────────
install_binary() {
    echo ""
    if [ -f build/txsyxts ]; then
        echo -e "${GREEN}✓ build successful!${NC}"
        [ -f build/txsyxts-login ] && echo -e "  ${GREEN}✓${NC} login helper built (browser login enabled)"
        echo ""
        read -rp "  Install to /usr/local/bin? (needs sudo) [y/N] " yn
        if [[ "$yn" =~ ^[Yy]$ ]]; then
            sudo cp build/txsyxts /usr/local/bin/txsyxts
            [ -f build/txsyxts-login ] && sudo cp build/txsyxts-login /usr/local/bin/txsyxts-login
            echo -e "${GREEN}installed!${NC} run: ${BOLD}txsyxts${NC}"
        else
            echo -e "  binary at: ${BOLD}./build/txsyxts${NC}"
            echo -e "  install manually: ${DIM}sudo cp build/txsyxts /usr/local/bin/${NC}"
        fi
    else
        echo -e "${RED}✗ build failed — check errors above${NC}"
        exit 1
    fi
}

# ── main ──────────────────────────────────────────────────────────────────────
install_deps
install_ytdlp
verify_deps
build_project
install_binary
