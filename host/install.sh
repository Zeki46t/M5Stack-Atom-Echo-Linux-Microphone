#!/usr/bin/env bash
# Installs the serial-to-microphone bridge as a systemd user service.
# Works on Pop!_OS/Ubuntu/Debian and Fedora (PipeWire based desktops).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"

if ! command -v pactl >/dev/null; then
  echo "pactl not found. Install it first:"
  echo "  Fedora:         sudo dnf install pulseaudio-utils"
  echo "  Pop!_OS/Ubuntu: sudo apt install pulseaudio-utils"
  exit 1
fi

if ! id -nG | tr ' ' '\n' | grep -qx dialout; then
  echo "You are not in the 'dialout' group, so you cannot open /dev/ttyUSB*."
  echo "Run:  sudo usermod -aG dialout \$USER   then log out and back in."
fi

if systemctl is-active --quiet ModemManager 2>/dev/null; then
  echo "Note: ModemManager is running and can grab USB serial ports (random resets,"
  echo "'port busy'). If that happens, see the README section 'ModemManager'."
fi

install -Dm755 "$here/atom_echo_bridge.py" "$HOME/.local/bin/atom-echo-bridge"
install -Dm644 "$here/atom-echo-mic.service" "$HOME/.config/systemd/user/atom-echo-mic.service"
systemctl --user daemon-reload
systemctl --user enable --now atom-echo-mic.service
echo "Installed. Check with: systemctl --user status atom-echo-mic"
echo "Before flashing the Atom Echo, run: systemctl --user stop atom-echo-mic"
