#!/usr/bin/env python3
"""
Sticky-packet test: send multiple complete packets in a single sendall().

Verifies the server decodes all requests and sends back all responses.
"""

import socket
import struct

HOST = "127.0.0.1"
PORT = 8080
FIXED_BODY_SIZE = 1 + 1 + 8

PACKET_COUNT = 5


def make_echo_packet(req_id, payload):
    body_length = FIXED_BODY_SIZE + len(payload)
    header = struct.pack("!I", body_length)
    meta = struct.pack("!BBQ", 1, 2, req_id)
    return header + meta + payload


def recv_one_response(sock):
    header = b""
    while len(header) < 4:
        chunk = sock.recv(4 - len(header))
        if not chunk:
            return None
        header += chunk
    body_len = struct.unpack("!I", header)[0]
    body = b""
    while len(body) < body_len:
        chunk = sock.recv(body_len - len(body))
        if not chunk:
            return None
        body += chunk
    rtype = body[1]
    rid = struct.unpack("!Q", body[2:10])[0]
    payload = body[10:].decode("utf-8", errors="replace")
    return {"type": rtype, "request_id": rid, "payload": payload}


def main():
    # Build N packets
    packets = []
    for i in range(PACKET_COUNT):
        payload = f"sticky-msg-{i}".encode()
        packets.append(make_echo_packet(2000 + i, payload))

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5.0)
    sock.connect((HOST, PORT))
    print(f"[sticky-packet] Sending {PACKET_COUNT} packets in one sendall()...")

    # Send all at once
    sock.sendall(b"".join(packets))
    print("[sticky-packet] All packets sent, now reading responses...")

    ok = True
    for i in range(PACKET_COUNT):
        resp = recv_one_response(sock)
        if resp is None:
            print(f"[sticky-packet] FAIL: missing response for packet {i}")
            ok = False
            break
        expected_id = 2000 + i
        if resp["type"] != 6:
            print(f"[sticky-packet] FAIL: packet {i} got type={resp['type']} (expected 6)")
            ok = False
        elif resp["request_id"] != expected_id:
            print(f"[sticky-packet] FAIL: packet {i} got request_id={resp['request_id']} (expected {expected_id})")
            ok = False
        else:
            print(f"[sticky-packet] OK packet {i}: type={resp['type']}, id={resp['request_id']}, payload='{resp['payload']}'")

    sock.close()

    if ok:
        print(f"[sticky-packet] PASS: all {PACKET_COUNT} responses received and matched")
    else:
        print(f"[sticky-packet] FAIL: some responses mismatched or missing")
    return ok


if __name__ == "__main__":
    ok = main()
    exit(0 if ok else 1)
