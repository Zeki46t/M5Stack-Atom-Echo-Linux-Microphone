#!/usr/bin/env python3
"""Bridge the Atom Echo's USB serial audio stream to a PipeWire/PulseAudio mic.

Reads framed PCM from the serial port (frame = A5 5A | seq | 512 bytes | xor),
creates a virtual source called "atom_echo" with module-pipe-source, and feeds
it. No third-party Python packages are needed.
"""
import argparse
import fcntl
import glob
import os
import select
import signal
import struct
import subprocess
import sys
import termios
import time

HDR = b"\xa5\x5a"
PAYLOAD = 512
FRAME = 2 + 1 + PAYLOAD + 1
SOURCE_NAME = "atom_echo"
F_SETPIPE_SZ = getattr(fcntl, "F_SETPIPE_SZ", 1031)

running = True


def log(msg):
    print(msg, file=sys.stderr, flush=True)


class Parser:
    def __init__(self):
        self.buf = bytearray()
        self.good = 0
        self.bad = 0
        self.lost = 0
        self.last_seq = None
        self.junk = bytearray()

    def feed(self, data):
        self.buf += data
        out = []
        while True:
            i = self.buf.find(HDR)
            if i < 0:
                keep = 1 if self.buf[-1:] == HDR[:1] else 0
                cut = len(self.buf) - keep
                self.junk += self.buf[:cut]
                del self.buf[:cut]
                break
            if i > 0:
                self.junk += self.buf[:i]
                del self.buf[:i]
            if len(self.buf) < FRAME:
                break
            seq = self.buf[2]
            payload = bytes(self.buf[3:3 + PAYLOAD])
            x = seq
            for b in payload:
                x ^= b
            if x == self.buf[3 + PAYLOAD]:
                if self.last_seq is not None:
                    self.lost += (seq - self.last_seq - 1) & 0xFF
                self.last_seq = seq
                self.good += 1
                out.append(payload)
                del self.buf[:FRAME]
            else:
                self.bad += 1
                del self.buf[:2]
        return out


def pactl(*args):
    return subprocess.run(["pactl", *args], capture_output=True, text=True)


def unload_stale():
    r = pactl("list", "short", "modules")
    for line in r.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) >= 3 and parts[1] == "module-pipe-source" and f"source_name={SOURCE_NAME}" in parts[2]:
            pactl("unload-module", parts[0])


def load_module(pipe_path):
    base = ["load-module", "module-pipe-source", f"source_name={SOURCE_NAME}",
            f"file={pipe_path}", "format=s16le", "rate=16000", "channels=1"]
    pretty = 'source_properties="device.description=\'Atom Echo Mic\'"'
    for extra in ([pretty], []):
        r = pactl(*base, *extra)
        if r.returncode == 0 and r.stdout.strip().isdigit():
            return int(r.stdout.strip())
        err = r.stderr.strip()
    log(f"could not load module-pipe-source: {err}")
    return None


def find_port(spec):
    if spec != "auto":
        return spec if os.path.exists(spec) else None
    cands = sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"))
    if len(cands) == 1:
        return cands[0]
    if len(cands) > 1:
        log(f"several serial devices found {cands}; pass --port /dev/serial/by-id/...")
    return None


def open_serial(path, baud):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = 0
    a[1] = 0
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    a[3] = 0
    a[4] = a[5] = getattr(termios, f"B{baud}")
    a[6][termios.VMIN] = 0
    a[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    try:  # keep DTR/RTS low so the ESP32 is not held in reset or download mode
        fcntl.ioctl(fd, termios.TIOCMBIC, struct.pack("I", termios.TIOCM_DTR | termios.TIOCM_RTS))
    except OSError:
        pass
    termios.tcflush(fd, termios.TCIFLUSH)
    return fd


def open_pipe(path):
    fd = os.open(path, os.O_WRONLY | os.O_NONBLOCK | os.O_CREAT, 0o600)
    try:  # small pipe = low latency; 512-byte writes stay atomic
        fcntl.fcntl(fd, F_SETPIPE_SZ, 4096)
    except OSError:
        pass
    return fd


def stream(port_fd, port_path, pipe_fd, verbose):
    p = Parser()
    last_report = time.time()
    dropped = 0
    empty = 0
    while running:
        r, _, _ = select.select([port_fd], [], [], 1.0)
        if r:
            try:
                data = os.read(port_fd, 4096)
            except BlockingIOError:
                continue
            if not data:  # spurious wake-up, or the device went away
                empty += 1
                time.sleep(0.01)
                if empty % 100 == 0 and not os.path.exists(port_path):
                    raise OSError("serial port disappeared")
                continue
            empty = 0
            for payload in p.feed(data):
                try:
                    os.write(pipe_fd, payload)
                except BlockingIOError:
                    dropped += 1  # nobody is recording; drop instead of lagging
                except BrokenPipeError:
                    raise
        if verbose and p.junk:
            text = p.junk.decode("utf-8", "replace").strip()
            if text:
                log(f"[esp32] {text}")
            p.junk.clear()
        elif p.junk:
            p.junk.clear()
        if verbose and time.time() - last_report > 5:
            log(f"frames ok={p.good} bad={p.bad} lost={p.lost} dropped={dropped}")
            last_report = time.time()


def main():
    global running
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", default="auto", help="serial device or 'auto'")
    ap.add_argument("--baud", type=int, default=500000, help="must match the firmware")
    ap.add_argument("--pipe", default=os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "atom_echo.pipe"))
    ap.add_argument("--no-module", action="store_true", help="do not call pactl; just write PCM to --pipe")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    def stop(*_):
        global running
        running = False
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    module_id = None
    if not args.no_module:
        unload_stale()
        try:
            os.unlink(args.pipe)
        except FileNotFoundError:
            pass
        while running and module_id is None:
            module_id = load_module(args.pipe)
            if module_id is None:
                time.sleep(3)
    try:
        while running:
            port = find_port(args.port)
            if not port:
                time.sleep(2)
                continue
            try:
                port_fd = open_serial(port, args.baud)
                pipe_fd = open_pipe(args.pipe)
            except OSError as e:
                log(f"open failed: {e}")
                time.sleep(2)
                continue
            log(f"streaming from {port}")
            try:
                stream(port_fd, port, pipe_fd, args.verbose)
            except OSError as e:
                log(f"stream stopped: {e}")
            finally:
                os.close(port_fd)
                os.close(pipe_fd)
            time.sleep(1)
    finally:
        if module_id is not None:
            pactl("unload-module", str(module_id))


if __name__ == "__main__":
    main()
