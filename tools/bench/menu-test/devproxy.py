#!/usr/bin/env python3
# devproxy.py IFACE HOST PORT: a TCP connection bound to one network device (SO_BINDTODEVICE),
# relayed on stdin/stdout - an ssh ProxyCommand for two units with the same address.
import sys, socket, os, select
iface, host, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, iface.encode() + b'\0')
s.settimeout(15); s.connect((host, port)); s.settimeout(None)
fi, fo = sys.stdin.fileno(), sys.stdout.fileno()
while True:
    r, _, _ = select.select([s, fi], [], [])
    if s in r:
        d = s.recv(65536)
        if not d: break
        while d: d = d[os.write(fo, d):]
    if fi in r:
        d = os.read(fi, 65536)
        if not d: break
        s.sendall(d)
