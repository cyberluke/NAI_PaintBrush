#!/usr/bin/env python3
"""TLS test server for the pb_vision_service harness (real OpenSSL TLS).

Usage: python pb_harness_tls_server.py <cert.pem> <key.pem> <port|0>
Prints "READY <port>" on stdout once listening (port 0 -> ephemeral).
Responds to any HTTP request with the OpenAI-compatible JSON body the
vision service expects. Tolerates clients that abort mid-handshake.
"""
import json
import socket
import ssl
import sys

cert_path, key_path, port = sys.argv[1], sys.argv[2], int(sys.argv[3])

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(cert_path, key_path)

body = json.dumps({
    "choices": [{
        "message": {
            "content": json.dumps({
                "answer": "ok from harness",
                "evidence": "",
                "uncertainty": "low",
            })
        }
    }]
}).encode("utf-8")

server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(5)
print("READY %d" % server.getsockname()[1], flush=True)

while True:
    conn, _ = server.accept()
    try:
        with ctx.wrap_socket(conn, server_side=True) as tls:
            data = b""
            while b"\r\n\r\n" not in data:
                chunk = tls.recv(4096)
                if not chunk:
                    break
                data += chunk
            response = (
                b"HTTP/1.1 200 OK\r\n"
                b"Content-Type: application/json\r\n"
                b"Content-Length: " + str(len(body)).encode() + b"\r\n"
                b"Connection: close\r\n\r\n" + body)
            tls.sendall(response)
    except Exception:
        pass