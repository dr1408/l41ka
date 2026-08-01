#!/usr/bin/env bash
set -euo pipefail

# CLion HACK so i can run this as a CTRL+R target
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
elf="${ELF:-$script_dir/../cmake-build-release/l41ka.elf}"
baud="${BAUD:-115200}"

require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        printf 'error: %s not found in PATH\n' "$1" >&2
        exit 127
    fi
}

require_command picotool

if [[ ! -f "$elf" ]]; then
    printf 'error: ELF not found: %s\n' "$elf" >&2
    exit 1
fi

killall -9 l41ka || true # claims exclusivity over the usb currently.
python3 $script_dir/laikadbg.py bootsel || picotool reboot -f -u
sleep 1

picotool load -t elf "$elf" -x -f
