"""funk-midi --dirt's handshake against a stand-in for SuperDirt 1.7.3
(docs/dev/dirt.md): it listens only after 2.5 s, so funk-midi waits, then
replies to /dirt/handshake as SuperDirt does, then sends /dirt/hello, which
must bring another handshake. Bundles must arrive, with no send errors.
Usage: python3 dirt_handshake.py FUNK_MIDI"""
import socket
import struct
import subprocess
import sys
import tempfile
import time


def osc_string(s):
    b = s.encode() + b"\0"
    return b + b"\0" * (-len(b) % 4)


def message(addr, *strings):
    return osc_string(addr) + osc_string("," + "s" * len(strings)) + b"".join(osc_string(s) for s in strings)


def address(d):
    if d.startswith(b"#bundle\0"):
        (size,) = struct.unpack(">I", d[16:20])
        d = d[20:20 + size]
    return d[:d.index(b"\0")].decode()


def main():
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write('pat = s "x*4"\n:wait 7000\n')
        script = f.name
    funk = subprocess.Popen([sys.argv[1], "--dirt", "127.0.0.1:%d" % port, "--script", script],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    time.sleep(2.5)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", port))
    s.settimeout(0.2)
    shakes, bundles, hello_at, shakes_after_hello = 0, 0, None, 0
    end = time.time() + 4
    while time.time() < end:
        try:
            d, peer = s.recvfrom(65536)
        except socket.timeout:
            continue
        a = address(d)
        if a == "/dirt/handshake":
            shakes += 1
            if hello_at:
                shakes_after_hello += 1
            s.sendto(message("/dirt/handshake/reply", "&serverHostname", "127.0.0.1"), peer)
        elif a == "/dirt/play":
            bundles += 1
            if shakes and not hello_at:
                hello_at = time.time()
                s.sendto(message("/dirt/hello"), peer)
    out, err = funk.communicate(timeout=30)
    waiting = err.count("Waiting for SuperDirt")
    connected = err.count("Connected to SuperDirt.")
    ok = (waiting == 1 and connected == 1 and err.index("Waiting") < err.index("Connected")
          and shakes >= 1 and shakes_after_hello >= 1 and bundles > 0 and "sending to SuperDirt" not in err
          and funk.returncode == 0)
    print("waiting %d connected %d shakes %d after hello %d bundles %d exit %s" %
          (waiting, connected, shakes, shakes_after_hello, bundles, funk.returncode))
    if not ok:
        print(err)
        sys.exit(1)


main()
