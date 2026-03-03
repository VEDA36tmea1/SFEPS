#!/usr/bin/env python3
"""
Simple TCP mock authentication server for tests.

Listens on HOST:PORT and expects payloads in the form "user:pass".
Responds with b"PASS" when credentials match the `USERS` map, otherwise b"FAIL".
"""
import socketserver
import logging

logging.basicConfig(level=logging.INFO, format="[mock-auth] %(message)s")

# Configuration: change these values to add accounts or a different port.
HOST = "127.0.0.1"
PORT = 5555
# Simple user -> password mapping used to determine PASS/FAIL
USERS = {
    "admin": "1111",
}


class AuthHandler(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            data = self.request.recv(1024).decode("utf-8").strip()
        except Exception:
            return

        logging.info("conn from %s - received: %r", self.client_address, data)

        # expected format: user:password
        if ":" in data:
            user, pwd = data.split(":", 1)
            if USERS.get(user) == pwd:
                self.request.sendall(b"PASS")
                logging.info("sent: PASS for user=%s", user)
                return

        self.request.sendall(b"FAIL")
        logging.info("sent: FAIL")


def main():
    # bind only to localhost to avoid affecting other machines on the network
    with socketserver.TCPServer((HOST, PORT), AuthHandler) as srv:
        logging.info("mock auth server listening on %s:%d", HOST, PORT)
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            logging.info("shutting down")


if __name__ == "__main__":
    main()
