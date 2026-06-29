#!/usr/bin/env bash
# install.sh — install xr-workspace + VITURE glasses auto-launch integration.
#
# Usage:
#   sudo ./packaging/install.sh                 # system-wide install
#   ./packaging/install.sh --user               # per-user install (no root)
#   sudo ./packaging/install.sh --prefix /usr   # custom prefix
#
# Installs:
#   - xr-workspace binary, xrctl, xr-game, xr-glasses-hotplugd  → PREFIX/bin
#   - udev rule (IMU uaccess)                                   → /etc/udev/rules.d
#   - systemd user unit (auto-launch watcher)                  → user unit dir
#   - Hyprland config snippet                                  → example dir
#
# Idempotent: safe to re-run.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
log()  { echo -e "${GREEN}[install]${NC} $*"; }
warn() { echo -e "${YELLOW}[install]${NC} $*"; }
die()  { echo -e "${RED}[install]${NC} $*" >&2; exit 1; }

USER_MODE=false
PREFIX="/usr"
for arg in "$@"; do
    case "$arg" in
        --user) USER_MODE=true ;;
        --prefix) shift; PREFIX="${1:?--prefix needs a path}" ;;
        --prefix=*) PREFIX="${arg#*=}" ;;
    esac
done

BIN="$REPO_ROOT/build/xr-workspace"
[ -x "$BIN" ] || die "renderer not built: $BIN (run: cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build)"

install_bin() { install -Dm755 "$1" "$2/$(basename "$1")"; log "installed $(basename "$1") → $2"; }

if $USER_MODE; then
    BINDIR="${HOME}/.local/bin"
    UDEVDIR=""   # cannot install udev rules without root
    UNITDIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
    HYPRDIR="${XDG_CONFIG_HOME:-$HOME/.config}/hypr"
else
    [ "$(id -u)" -eq 0 ] || die "system install needs root (use sudo, or pass --user)"
    BINDIR="$PREFIX/bin"
    UDEVDIR="/etc/udev/rules.d"
    UNITDIR="/usr/lib/systemd/user"
    HYPRDIR="$PREFIX/share/xr-workspace"
fi

mkdir -p "$BINDIR" "$UNITDIR" "$HYPRDIR"

# ── Binaries / scripts ────────────────────────────────────────────────────────
install_bin "$BIN" "$BINDIR"
install_bin "$REPO_ROOT/xrctl" "$BINDIR"
install_bin "$REPO_ROOT/xr-game" "$BINDIR"
install_bin "$SCRIPT_DIR/hotplug/xr-glasses-hotplugd" "$BINDIR"

# ── udev rule (IMU uaccess) ───────────────────────────────────────────────────
if [ -n "$UDEVDIR" ]; then
    install -Dm644 "$SCRIPT_DIR/udev/99-viture-xr.rules" "$UDEVDIR/99-viture-xr.rules"
    log "installed udev rule → $UDEVDIR"
    udevadm control --reload-rules 2>/dev/null || warn "could not reload udev rules"
    udevadm trigger --subsystem-match=usb 2>/dev/null || true
else
    warn "skipping udev rule (no root) — IMU may need manual permissions"
fi

# ── systemd user service ──────────────────────────────────────────────────────
install -Dm644 "$SCRIPT_DIR/systemd/xr-glasses.service" "$UNITDIR/xr-glasses.service"
log "installed systemd user unit → $UNITDIR"

# ── Hyprland snippet ──────────────────────────────────────────────────────────
install -Dm644 "$SCRIPT_DIR/hypr/xr-workspace.conf" "$HYPRDIR/xr-workspace.conf"
log "installed Hyprland snippet → $HYPRDIR/xr-workspace.conf"

# ── Default config ────────────────────────────────────────────────────────────
CFGDIR="${XDG_CONFIG_HOME:-$HOME/.config}/xr-workspace"
if $USER_MODE && [ ! -f "$CFGDIR/config.json" ] && [ -f "$REPO_ROOT/config.example.json" ]; then
    install -Dm644 "$REPO_ROOT/config.example.json" "$CFGDIR/config.json"
    log "installed default config → $CFGDIR/config.json"
fi

log "done."
echo
echo "Next steps:"
if $USER_MODE; then
    echo "  systemctl --user daemon-reload"
    echo "  systemctl --user enable --now xr-glasses.service"
else
    echo "  (each user) systemctl --user enable --now xr-glasses.service"
fi
echo "  Add to ~/.config/hypr/hyprland.conf:  source = $HYPRDIR/xr-workspace.conf"
echo "  Plug in VITURE glasses — the renderer starts automatically."
