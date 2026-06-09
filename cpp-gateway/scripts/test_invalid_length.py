#!/usr/bin/env python3
"""
Invalid body_length stability test.

Sends packets with various invalid body_length values and verifies:
- Server closes the connection (does not crash)
- Server still accepts new connections afterwards
"""

import socket
import struct
import time

HOST = "127.0.0.1"
PORT = 8080
FIXED_BODY_SIZE = 1 + 1 + 8
MAX_BODY_SIZE = 4 * 1024 * 1024 + FIXED_BODY_SIZE

# Test cases: (name, body_length, payload)
CASES = [
    ("body_length = 0",        0,                           b"ignored"),
    ("body_length = 5",        5,                           b"short"),     # < FIXED_BODY_SIZE
    ("body_length = 9",        9,                           b"x" * 9),     # still < FIXED_BODY_SIZE
    ("body_length = 999999999", 999999999,                   b"fake"),      # > MAX_BODY_SIZE
    ("body_length = 4MB+1",    MAX_BODY_SIZE + 1,            b"x"),
    ("body_length = MAX_PAYLOAD_SIZE only (no meta)",
                                4 * 1024 * 1024,             b"x" * 1024), # payload alone exceeds max? actually body_length itself = 4MB < MAX_BODY_SIZE? Let me think...

    # Actually body_length = 4MB means total body = 4MB = 4194304. FIXED_BODY_SIZE=10, MAX_BODY_SIZE=4194314+10=4194314? No: MAX_BODY_SIZE = MAX_PAYLOAD_SIZE + FIXED_BODY_SIZE = 4194304 + 10 = 4194314
    # So body_length=4194304 => 4194304 < 4194314 => passes length check, then payload_length = 4194304-10 = 4194294, which = MAX_PAYLOAD_SIZE. OK borderline.
    # Let me just test clearly invalid ones.
]

# More focused test cases
CASES = [
    ("body_length < FIXED_BODY_SIZE (val=5)",     5),
    ("body_length < FIXED_BODY_SIZE (val=9)",     9),
    ("body_length = 0",                          0),
    ("body_length = MAX_BODY_SIZE + 100",         MAX_BODY_SIZE + 100),
    ("body_length = 999999999",                  999999999),
]


def try_invalid_packet(name, body_length):
    """Send an invalid-length packet and verify the server closes the connection."""
    print(f"\n[invalid-length] Test: {name}")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(3.0)
    try:
        sock.connect((HOST, PORT))
    except OSError as e:
        print(f"[invalid-length] FAIL: could not connect — {e}")
        return False

    # Send header with invalid body_length + some garbage body
    header = struct.pack("!I", body_length)
    garbage = b"x" * min(body_length, 64) if body_length > 0 else b""
    packet = header + garbage

    try:
        sock.sendall(packet)
    except OSError:
        # Server already closed us — that's fine
        print(f"[invalid-length] OK: server closed connection immediately (send failed)")
        sock.close()
        return True

    # Try to read — server should close the connection
    time.sleep(0.3)
    try:
        data = sock.recv(16)
        if data:
            print(f"[invalid-length] WARN: server sent {len(data)} bytes instead of closing")
            # Could be an ERROR_RESP, but should still close
        else:
            print(f"[invalid-length] OK: server closed connection (recv returned empty)")
            sock.close()
            return True
    except socket.timeout:
        # Server didn't close? That's a problem
        print(f"[invalid-length] FAIL: server did NOT close connection (timeout)")
        sock.close()
        return False
    except OSError:
        print(f"[invalid-length] OK: connection closed by server")
        return True

    sock.close()
    return True


def check_server_alive():
    """Verify the server is still alive after all tests."""
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(3.0)
        sock.connect((HOST, PORT))
        # Send PING
        body_length = FIXED_BODY_SIZE + 0
        header = struct.pack("!I", body_length)
        meta = struct.pack("!BBQ", 1, 1, 9999)
        sock.sendall(header + meta)
        # Read response
        h = sock.recv(4)
        if not h:
            print("[invalid-length] FAIL: server not responding after tests")
            sock.close()
            return False
        bl = struct.unpack("!I", h)[0]
        b = sock.recv(bl)
        rtype = b[1]
        print(f"[invalid-length] Server alive check: got type={rtype} (expected 5=PONG)")
        sock.close()
        return rtype == 5
    except OSError as e:
        print(f"[invalid-length] FAIL: server unreachable after tests — {e}")
        return False


def main():
    all_pass = True
    for name, bl in CASES:
        if not try_invalid_packet(name, bl):
            all_pass = False

    print()
    if check_server_alive():
        print("[invalid-length] PASS: server still alive after all invalid-length tests")
    else:
        print("[invalid-length] FAIL: server crashed or unreachable")
        all_pass = False

    if all_pass:
        print("[invalid-length] RESULT: PASS")
    else:
        print("[invalid-length] RESULT: FAIL")
    return all_pass


if __name__ == "__main__":
    ok = main()
    exit(0 if ok else 1)
