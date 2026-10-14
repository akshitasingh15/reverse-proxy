# Simple test backend. Run several on different ports:
#   python3 backend/server.py 9001
#   python3 backend/server.py 9002
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9001
NAME = sys.argv[2] if len(sys.argv) > 2 else f"backend-{PORT}"


class Handler(BaseHTTPRequestHandler):
    def _reply(self, body: str):
        data = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        xff = self.headers.get("X-Forwarded-For", "-")
        self._reply(f"Hello from {NAME}, path={self.path}, client={xff}\n")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode()
        self._reply(f"{NAME} received: {body}\n")

    def log_message(self, *args):  # keep output quiet during benchmarks
        pass


if __name__ == "__main__":
    print(f"[{NAME}] listening on :{PORT}", flush=True)
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
