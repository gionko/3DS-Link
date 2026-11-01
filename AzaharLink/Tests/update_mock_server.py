# The servers Tests/update_http_test.cpp talks to: one HTTPS server for each kind of certificate, on consecutive ports from the first one. Used by Tests/update_http_test.sh.
#   usage: update_mock_server.py <first port> <folder with the certificates>
import http.server
import os
import socket
import ssl
import sys
import threading
import time

FILE_BYTES = bytes((i * 7) & 0xFF for i in range(1024 * 1024))
FIRST_PORT = int(sys.argv[1])
CERTS = sys.argv[2]


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def redirect(self, target):
        self.send_response(302)
        self.send_header("Location", target)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def status_only(self, code):
        body = b"this is not the file"
        self.send_response(code)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        path = self.path
        if path == "/latest":
            self.redirect("https://github.com/gionko/3DS-Link/releases/tag/v1.2.3")
        elif path == "/redirect":
            self.redirect("/redirect2")
        elif path == "/redirect2":
            self.redirect("/file")
        elif path == "/loop":
            self.redirect("/loop")
        elif path == "/to-http":
            self.redirect(f"http://localhost:{FIRST_PORT}/file")
        elif path == "/to-other-ca":
            self.redirect(f"https://localhost:{FIRST_PORT + 4}/file")
        elif path == "/file":
            self.send_response(200)
            self.send_header("Content-Length", str(len(FILE_BYTES)))
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(FILE_BYTES)
        elif path == "/404":
            self.status_only(404)
        elif path == "/500":
            self.status_only(500)
        elif path == "/truncated":
            self.send_response(200)
            self.send_header("Content-Length", "100000")
            self.end_headers()
            self.wfile.write(b"x" * 50000)
            self.wfile.flush()
            self.close_connection = True
        elif path == "/slow":
            self.send_response(200)
            self.send_header("Content-Length", "100000000")
            self.end_headers()
            try:
                for _ in range(1000):
                    self.wfile.write(b"y" * 1024)
                    self.wfile.flush()
                    time.sleep(0.1)
            except OSError:
                pass
            self.close_connection = True
        elif path == "/stall":
            self.send_response(200)
            self.send_header("Content-Length", "100000")
            self.end_headers()
            self.wfile.write(b"z" * 10)
            self.wfile.flush()
            time.sleep(30)
            self.close_connection = True
        else:
            self.status_only(404)


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        pass  # (the client hangs up in the middle on purpose)


def serve(port, name):
    server = Server(("127.0.0.1", port), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(os.path.join(CERTS, name + ".crt"), os.path.join(CERTS, name + ".key"))
    server.socket = context.wrap_socket(server.socket, server_side=True, do_handshake_on_connect=False)
    threading.Thread(target=server.serve_forever, daemon=True).start()


def silent(port):
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", port))
    listener.listen(8)
    kept = []

    def loop():
        while True:
            kept.append(listener.accept()[0])

    threading.Thread(target=loop, daemon=True).start()


for offset, name in enumerate(["good", "expired", "future", "otherhost", "otherca"]):
    serve(FIRST_PORT + offset, name)
silent(FIRST_PORT + 5)
print("ready", flush=True)
while True:
    time.sleep(60)
