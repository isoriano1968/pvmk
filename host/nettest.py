#!/usr/bin/env python3
"""nettest.py: the PC's side of pvmk's `net` command (svc/net.c).

  nettest.py IP in   [seconds] [size] [rate]   UDP to port 9 as fast as the
                                               PC can (or rate datagrams/s),
                                               then pvmk's count: what got in
  nettest.py IP out  [count] [size]            pvmk sends count datagrams of
                                               size bytes; what arrives here
  nettest.py IP echo [count] [size]            UDP 7 round trips

Each datagram starts with a 32-bit sequence number, so a loss is a gap.
Ping it with the system's ping.
"""
import socket
import struct
import sys
import time


def stat(ip):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(2)
    s.sendto(b"STAT", (ip, 5001))
    try:
        return s.recvfrom(256)[0].decode()
    except socket.timeout:
        return "no answer to STAT"


def test_in(ip, secs=5, size=1024, rate=0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    pad = bytes(size - 4)
    before = stat(ip)
    t0 = time.time()
    n = 0
    while time.time() - t0 < secs:
        s.sendto(struct.pack(">I", n) + pad, (ip, 9))
        n += 1
        if rate:
            while n > (time.time() - t0) * rate:
                time.sleep(0.0002)
    dt = time.time() - t0
    time.sleep(0.5)
    after = stat(ip)
    print("sent %d datagrams of %d bytes in %.1f s: %.0f KB/s offered"
          % (n, size, dt, n * size / dt / 1024))
    print("pvmk before: %s" % before)
    print("pvmk after:  %s" % after)


def test_out(ip, count=5000, size=1024):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("", 0))
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
    port = s.getsockname()[1]
    s.settimeout(5)
    s.sendto(b"SEND %d %d %d" % (count, size, port), (ip, 5001))
    got, last, gaps, t0, t1, done = 0, -1, 0, None, None, b""
    while True:
        try:
            d, _ = s.recvfrom(4096)
        except socket.timeout:
            break
        if d.startswith(b"DONE"):
            done = d
            break
        if t0 is None:
            t0 = time.time()
        t1 = time.time()
        seq = struct.unpack(">I", d[:4])[0]
        if seq != last + 1:
            gaps += seq - last - 1
        last = seq
        got += 1
    dt = (t1 - t0) if t0 and t1 and t1 > t0 else 0
    print("asked %d of %d bytes: got %d, lost %d%s" % (count, size, got, count - got,
          ", %.0f KB/s" % (got * size / dt / 1024) if dt else ""))
    print("pvmk: %s" % (done.decode() if done else "no DONE"))


def test_echo(ip, count=200, size=64):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(1)
    rtts, lost = [], 0
    for i in range(count):
        t0 = time.time()
        s.sendto(struct.pack(">I", i) + bytes(size - 4), (ip, 7))
        try:
            while True:
                d, _ = s.recvfrom(4096)
                if struct.unpack(">I", d[:4])[0] == i:
                    break
            rtts.append((time.time() - t0) * 1000)
        except socket.timeout:
            lost += 1
    if rtts:
        rtts.sort()
        print("%d echoes of %d bytes: lost %d, min %.2f ms, median %.2f ms, max %.2f ms"
              % (count, size, lost, rtts[0], rtts[len(rtts) // 2], rtts[-1]))
    else:
        print("no echo at all")


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    ip, mode, rest = argv[1], argv[2], [int(x) for x in argv[3:]]
    {"in": test_in, "out": test_out, "echo": test_echo}[mode](ip, *rest)


main(sys.argv)
