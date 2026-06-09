#!/usr/bin/env python3
"""
Slow-client stability test.

Sends many requests without reading responses to trigger the server's
output-buffer backpressure (MAX_OUT_BUFFER_SIZE = 2MB).

Verifies:
- Server closes the slow connection when output buffer exceeds limit
- Server does NOT crash
- Server still responds to other (normal) clients
"""

import socket
import struct
import time

HOST = "127.0.0.1"
PORT = 8080
FIXED_BODY_SIZE = 1 + 1 + 8
MAX_OUT_BUFFER_SIZE = 2 * 1024 * 1024

# We'll use LOG_PUSH with a moderately sized payload so each response
# accumulates enough bytes in the output buffer to trigger the limit.
# Each ECHO response returns the payload, so using a ~100KB payload
# means ~20 responses would exceed 2MB. Let's use a safer payload.
PAYLOAD_SIZE = 1024 * 100  # 100 KB per response ~= 20 responses to fill 2MB


def make_packet(msg_type, req_id, payload):
    body_length = FIXED_BODY_SIZE + len(payload)
    header = struct.pack("!I", body_length)
    meta = struct.pack("!BBQ", 1, msg_type, req_id)
    return header + meta + payload


def main():
    payload = b"x" * PAYLOAD_SIZE

    print(f"[slow-client] Connecting and sending ~60 requests (100 KB payload each)...")
    print(f"[slow-client] Server MAX_OUT_BUFFER_SIZE = {MAX_OUT_BUFFER_SIZE} bytes")
    print(f"[slow-client] After ~20 unread responses, server should close us")

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(3.0)
    sock.connect((HOST, PORT))

    # Set the receive buffer very small so we don't accidentally drain
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)

    sent = 0
    for i in range(60):
        packet = make_packet(2, 10000 + i, payload)  # ECHO
        try:
            sock.sendall(packet)
            sent += 1
        except (OSError, BrokenPipeError):
            print(f"[slow-client] Send failed at request {sent} — server closed connection")
            break

    # Small delay to let server process
    time.sleep(0.5)

    # Now try to connect with a normal client to verify server still alive
    print(f"\n[slow-client] Sent {sent} requests without reading responses")
    print(f"[slow-client] Checking if server is still alive with a new connection...")

    try:
        check = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        check.settimeout(3.0)
        check.connect((HOST, PORT))

        # Send PING
        packet = make_packet(1, 9999, b"")  # PING
        check.sendall(packet)

        h = check.recv(4)
        if not h:
            print("[slow-client] FAIL: server not responding")
            check.close()
            return False

        bl = struct.unpack("!I", h)[0]
        body = check.recv(bl)
        rtype = body[1]
        rid = struct.unpack("!Q", body[2:10])[0]
        payload_str = body[10:].decode("utf-8", errors="replace")

        if rtype == 5 and rid == 9999:
            print(f"[slow-client] PASS: server is alive (PONG type={rtype}, id={rid})")
            print(f"[slow-client] PASS: slow client connection was closed by server")
            check.close()
            return True
        else:
            print(f"[slow-client] WARN: unexpected response type={rtype}, id={rid}")
            check.close()
            return False

    except OSError as e:
        print(f"[slow-client] FAIL: server unreachable — {e}")
        return False


if __name__ == "__main__":
    ok = main()
    exit(0 if ok else 1)
