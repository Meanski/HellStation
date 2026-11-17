#!/usr/bin/env python3
"""Read the PS2 build's in-memory log ring (hs_log_ring) out of a running or
paused PCSX2 over PINE, PCSX2's IPC socket. Works after a crash: PCSX2 pauses
the VM on an EE exception and keeps memory readable.

    tools/port/pine-log.py <ring addr hex> <pos addr hex> [socket]

Addresses come from `nm build-ps2/d2.elf` (tools/port/run-pcsx2.sh passes them).
PCSX2 needs [EmuCore] EnablePINE = true (default slot 28011).
"""
import os, socket, struct, sys

def connect(path):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(path)
    return s

def read64(s, addr):
    s.sendall(struct.pack("<IBI", 9, 0x03, addr))  # MsgRead64
    hdr = b""
    while len(hdr) < 4: hdr += s.recv(4 - len(hdr))
    size = struct.unpack("<I", hdr)[0]
    body = b""
    while len(body) < size - 4: body += s.recv(size - 4 - len(body))
    if body[0] != 0: raise RuntimeError("PINE read failed at %08x" % addr)
    return body[1:9]

def main():
    ring, pos = int(sys.argv[1], 16), int(sys.argv[2], 16)
    path = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.environ.get("TMPDIR", "/tmp"), "pcsx2.sock")
    s = connect(path)
    n = struct.unpack("<I", read64(s, pos)[:4])[0]
    size = 65536
    start = max(0, n - size)
    data = bytearray()
    for off in range(start - start % 8, n, 8):
        data += read64(s, ring + off % size)
    text = bytes(data[start % 8: start % 8 + (n - start)]).decode("latin-1", "replace")
    sys.stdout.write(text)

if __name__ == "__main__":
    main()
