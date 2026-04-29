#!/usr/bin/env bash
# run.sh — launcher for xr-workspace
# Checks dependencies, installs if possible, then starts with 3 monitors.

set -euo pipefail

BINARY="$(dirname "$(realpath "$0")")/build/xr-workspace"
ARGS=(--monitors 3)

# ── Pretty printing ───────────────────────────────────────────────────────────
RED='\033[0;31m'; YELLOW='\033[1;33m'; GREEN='\033[0;32m'; BOLD='\033[1m'; NC='\033[0m'
info()  { echo -e "${BOLD}[xr-workspace]${NC} $*"; }
warn()  { echo -e "${YELLOW}[xr-workspace] WARNING:${NC} $*"; }
error() { echo -e "${RED}[xr-workspace] ERROR:${NC} $*" >&2; }
ok()    { echo -e "${GREEN}[xr-workspace] OK:${NC} $*"; }

# ── Shortcut / usage banner ───────────────────────────────────────────────────
print_shortcuts() {
    echo ""
    echo -e "${BOLD}╔══════════════════════════════════════════════════╗${NC}"
    echo -e "${BOLD}║           xr-workspace — 3 virtual monitors     ║${NC}"
    echo -e "${BOLD}╠══════════════════════════════════════════════════╣${NC}"
    echo -e "${BOLD}║  Ctrl+Shift+C${NC}  Recenter / snap to head position  ${BOLD}║${NC}"
    echo -e "${BOLD}║  SIGUSR1      ${NC}  Recenter (signal, for scripting)  ${BOLD}║${NC}"
    echo -e "${BOLD}║  Ctrl+C       ${NC}  Quit                              ${BOLD}║${NC}"
    echo -e "${BOLD}╠══════════════════════════════════════════════════╣${NC}"
    echo -e "${BOLD}║  IMU drift?${NC} Press Ctrl+Shift+C to re-snap.        ${BOLD}║${NC}"
    echo -e "${BOLD}╚══════════════════════════════════════════════════╝${NC}"
    echo ""
}

# ── Detect package manager ────────────────────────────────────────────────────
detect_pm() {
    if   command -v pacman  &>/dev/null; then echo "pacman"
    elif command -v apt-get &>/dev/null; then echo "apt"
    elif command -v dnf     &>/dev/null; then echo "dnf"
    elif command -v zypper  &>/dev/null; then echo "zypper"
    else echo "none"
    fi
}

try_install() {
    local pm="$1"; shift
    local packages=("$@")
    case "$pm" in
        pacman) sudo pacman -S --noconfirm "${packages[@]}" ;;
        apt)    sudo apt-get install -y    "${packages[@]}" ;;
        dnf)    sudo dnf install -y        "${packages[@]}" ;;
        zypper) sudo zypper install -y     "${packages[@]}" ;;
    esac
}

# Map shared library → package name per distro
pkg_for_lib() {
    local lib="$1" pm="$2"
    case "$lib" in
        libwayland-client*|libwayland-egl*)
            case "$pm" in
                pacman) echo "wayland" ;;
                apt)    echo "libwayland-dev" ;;
                dnf)    echo "wayland-devel" ;;
                zypper) echo "wayland-devel" ;;
            esac ;;
        libEGL*|libGLESv2*|libGLdispatch*)
            case "$pm" in
                pacman) echo "libglvnd mesa" ;;
                apt)    echo "libgles2 libegl1" ;;
                dnf)    echo "mesa-libEGL mesa-libGLES" ;;
                zypper) echo "Mesa-libEGL1 Mesa-libGLESv2" ;;
            esac ;;
        libstdc*|libgcc_s*)
            case "$pm" in
                pacman) echo "gcc-libs" ;;
                apt)    echo "libstdc++6" ;;
                dnf)    echo "libstdc++" ;;
                zypper) echo "libstdc++6" ;;
            esac ;;
    esac
}

# ── Check required shared libraries ──────────────────────────────────────────
REQUIRED_LIBS=(
    libwayland-client.so.0
    libwayland-egl.so.1
    libEGL.so.1
    libGLESv2.so.2
)

PM=$(detect_pm)
MISSING=()

info "Checking runtime dependencies..."
for lib in "${REQUIRED_LIBS[@]}"; do
    if ldconfig -p 2>/dev/null | grep -q "^[[:space:]]*${lib}"; then
        ok "$lib found"
    else
        warn "$lib NOT found"
        MISSING+=("$lib")
    fi
done

if [ ${#MISSING[@]} -gt 0 ]; then
    warn "Missing libraries: ${MISSING[*]}"
    if [ "$PM" = "none" ]; then
        error "No supported package manager found (pacman/apt/dnf/zypper)."
        error "Please install the following manually: ${MISSING[*]}"
        exit 1
    fi

    info "Attempting to install missing packages via $PM..."
    PKGS=()
    for lib in "${MISSING[@]}"; do
        read -ra p <<< "$(pkg_for_lib "$lib" "$PM")"
        PKGS+=("${p[@]}")
    done
    # Deduplicate
    mapfile -t PKGS < <(printf '%s\n' "${PKGS[@]}" | sort -u)

    if [ ${#PKGS[@]} -eq 0 ]; then
        error "Could not determine package names for missing libraries."
        exit 1
    fi

    info "Installing: ${PKGS[*]}"
    if ! try_install "$PM" "${PKGS[@]}"; then
        error "Package installation failed. Please install manually: ${PKGS[*]}"
        exit 1
    fi
    ok "Packages installed."
fi

# ── Check IMU shared memory ───────────────────────────────────────────────────
if [ ! -e /dev/shm/breezy_desktop_imu ]; then
    warn "/dev/shm/breezy_desktop_imu not found — is XRLinuxDriver running?"
    warn "Head tracking will be unavailable until the driver is started."
    warn "Start it with: sudo <path-to-xrDriver>"
fi

# ── Check Wayland session ─────────────────────────────────────────────────────
if [ -z "${WAYLAND_DISPLAY:-}" ]; then
    error "WAYLAND_DISPLAY is not set. Must be run in a Wayland session."
    exit 1
fi

# ── Check binary ─────────────────────────────────────────────────────────────
if [ ! -x "$BINARY" ]; then
    error "Binary not found: $BINARY"
    error "Build it first with: cmake -B build && cmake --build build"
    exit 1
fi

# ── Launch ────────────────────────────────────────────────────────────────────
print_shortcuts
info "Launching: $BINARY ${ARGS[*]}"
exec "$BINARY" "${ARGS[@]}" "$@"
