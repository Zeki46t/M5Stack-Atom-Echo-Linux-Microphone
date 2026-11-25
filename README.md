# Atom Echo as a Linux microphone (USB serial + Bluetooth)

One firmware, two microphones that work at the same time:

- **USB serial**: 16 kHz 16-bit mono, framed over the USB-C port. A small bridge on Linux exposes it as an input called "Atom Echo Mic" that any app can use.
- **Bluetooth**: Classic HFP headset. Pair it and it appears as a Bluetooth headset mic (call quality: 8 kHz, or 16 kHz with mSBC).

This replaces the ESPHome firmware. To go back, flash your ESPHome config again.

## 1. Flash

```
cd atom-echo-mic
rm -f sdkconfig.atom-echo        # only if you built this project before
pio run -t upload
```

Stop the bridge first if it is running: `systemctl --user stop atom-echo-mic`.

## 2. USB serial mic

```
sudo usermod -aG dialout $USER   # once, then log out and back in
./host/install.sh
```

"Atom Echo Mic" now shows in Sound settings > Input. If several USB serial devices are plugged in, edit `ExecStart` in `~/.config/systemd/user/atom-echo-mic.service` to use `--port /dev/serial/by-id/<your device>`.

Debug: `systemctl --user stop atom-echo-mic; ~/.local/bin/atom-echo-bridge -v` prints frame counts and ESP32 log lines.

## 3. Bluetooth mic

1. Open Bluetooth settings (or `bluetoothctl`), scan, and pair "Atom Echo Mic". Trust it so it reconnects. After a reboot the Atom Echo also reconnects to the PC it was paired with.
2. A Bluetooth mic only works in the headset profile. Select it in Sound settings, or:
   ```
   pactl list cards short | grep bluez
   pactl set-card-profile <card name> headset-head-unit-msbc   # or headset-head-unit
   ```
   Many apps switch to it automatically when they start recording.

## Tuning

- Too quiet or clipping: change `MIC_GAIN` in `src/main.c` and reflash.
- Bluetooth sounds narrowband: it negotiated CVSD (8 kHz); that is normal for some setups.
- To pair with a different PC, remove the bond on the Atom Echo by erasing flash (`pio run -t erase`) and reflash.

## Fedora (Workstation 45, GNOME)

The firmware is the same; only the host setup differs.

```
sudo dnf install pulseaudio-utils      # provides pactl (PipeWire's pulse layer is already there)
sudo usermod -aG dialout $USER         # then log out and back in
pipx install platformio                # or: sudo dnf install pipx first
./host/install.sh
```

- Flashing: PlatformIO needs its udev rules once:
  `sudo curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules -o /etc/udev/rules.d/99-platformio-udev.rules && sudo udevadm control --reload-rules && sudo udevadm trigger`
- USB mic: GNOME Settings > Sound > Input shows "Atom Echo Mic".
- Bluetooth: GNOME Settings > Bluetooth, pair "Atom Echo Mic". Then in Settings > Sound pick the headset profile for it, or use `pactl set-card-profile` as above. Run `pactl list cards short` to get the card name; on newer PipeWire the profile may be just `headset-head-unit` and the codec (mSBC/CVSD) is negotiated automatically.
- SELinux needs no changes; the bridge runs as your user.

## ModemManager

On both distros ModemManager may probe USB serial ports and interfere. Find the device IDs with `lsusb`, then:

```
echo 'ATTRS{idVendor}=="XXXX", ATTRS{idProduct}=="YYYY", ENV{ID_MM_DEVICE_IGNORE}="1"' | sudo tee /etc/udev/rules.d/99-atom-echo.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```
