#!/usr/bin/env python3
"""
Half-packet (split-send) stability test.

Sends a valid ECHO packet in two halves separated by a delay.
Verifies the server waits for the full packet before responding.
"""

import socket
import struct
import time

HOST = "127.0.0.1"
PORT = 8080
FIXED_BODY_SIZE = 1 + 1 + 8          # version + type + request_id


def main():
    payload = b"hello-half-packet"
    body_length = FIXED_BODY_SIZE + len(payload)
    req_id = 9001

    # Full packet: header(4) + meta(10) + payload
    header = struct.pack("!I", body_length)
    meta = struct.pack("!BBQ", 1, 2, req_id)   # ECHO
    full_packet = header + meta + payload

    # Split point: send the 4-byte header first, then the rest after a delay
    split_pos = 4

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(3.0)
    sock.connect((HOST, PORT))
    print(f"[half-packet] Connected, sending first {split_pos} bytes...")

    sock.sendall(full_packet[:split_pos])
    time.sleep(1.5)  # wait — server should not respond yet
    print("[half-packet] Waiting 1.5s… server should hold, not close")

    # Try to read — should timeout because server is waiting for more data
    sock.settimeout(0.8)
    try:
        partial = sock.recv(1)
        if partial:
            print("[half-packet] FAIL: server sent data before full packet arrived!")
            sock.close()
            return False
    except socket.timeout:
        print("[half-packet] OK: no response yet (server waiting for more data)")

    # Now send the rest
    print("[half-packet] Sending remaining data...")
    sock.sendall(full_packet[split_pos:])

    # Read the response
    sock.settimeout(3.0)
    header_resp = sock.recv(4)
    if not header_resp or len(header_resp) < 4:
        print("[half-packet] FAIL: no response after full packet")
        sock.close()
        return False

    resp_len = struct.unpack("!I", header_resp)[0]
    body = b""
    while len(body) < resp_len:
        chunk = sock.recv(resp_len - len(body))
        if not chunk:
            break
        body += chunk
    if len(body) < resp_len:
        print("[half-packet] FAIL: incomplete response body")
        sock.close()
        return False

    rtype = body[1]
    rid = struct.unpack("!Q", body[2:10])[0]
    resp_payload = body[10:].decode("utf-8", errors="replace")

    if rtype == 6 and rid == req_id:
        print(f"[half-packet] PASS: got ECHO_RESP (type=6), request_id={rid}, payload='{resp_payload}'")
        sock.close()
        return True
    else:
        print(f"[half-packet] FAIL: unexpected resp type={rtype}, id={rid} (expected type=6, id={req_id})")
        sock.close()
        return False


if __name__ == "__main__":
    ok = main()
    exit(0 if ok else 1)
