#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Minimal IS-04 v1.3 registry for the integration tests (Registration API, Query API, request log).

    python3 tests/integration/mock_registry.py --port 18210

Registration: POST /x-nmos/registration/v1.3/resource, DELETE .../resource/{type}s/{id},
POST .../health/nodes/{id}. Query: GET /x-nmos/query/v1.3/{type}s[/{id}] on the same port.
GET /_log returns every request seen (method, path, resource type and id) for assertions.
Only the standard library; no validation beyond what the tests need.
"""

import argparse
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

REG = "/x-nmos/registration/v1.3"
QUERY = "/x-nmos/query/v1.3"
TYPES = ("node", "device", "source", "flow", "sender", "receiver")

lock = threading.Lock()
resources = {t: {} for t in TYPES}
requests = []


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # quiet
        pass

    def reply(self, code, body=None, headers=None):
        data = b"" if body is None else json.dumps(body).encode()
        self.send_response(code)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        if body is not None:
            self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if data:
            self.wfile.write(data)

    def record(self, **extra):
        with lock:
            requests.append({"t": time.time(), "method": self.command, "path": self.path, **extra})

    def do_GET(self):
        path = self.path.split("?")[0].rstrip("/")
        self.record()
        if path == "/_log":
            with lock:
                return self.reply(200, list(requests))
        if path in ("", REG, QUERY, "/x-nmos"):
            return self.reply(200, ["resource/", "health/"] if path == REG else [])
        if path.startswith(QUERY + "/"):
            parts = path[len(QUERY) + 1:].split("/")
            kind = parts[0][:-1] if parts[0].endswith("s") else parts[0]
            if kind not in resources:
                return self.reply(404, {"code": 404, "error": "unknown type"})
            with lock:
                if len(parts) == 1:
                    return self.reply(200, list(resources[kind].values()))
                r = resources[kind].get(parts[1])
            return self.reply(200, r) if r else self.reply(404, {"code": 404, "error": "not found"})
        return self.reply(404, {"code": 404, "error": "not found"})

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(length) or b"{}")
        path = self.path.rstrip("/")
        if path == REG + "/resource":
            kind, data = body.get("type"), body.get("data", {})
            self.record(type=kind, id=data.get("id"))
            if kind not in resources:
                return self.reply(400, {"code": 400, "error": "bad type"})
            with lock:
                existed = data["id"] in resources[kind]
                resources[kind][data["id"]] = data
            location = f"{REG}/resource/{kind}s/{data['id']}"
            return self.reply(200 if existed else 201, data, {"Location": location})
        if path.startswith(REG + "/health/nodes/"):
            node_id = path.rsplit("/", 1)[1]
            self.record(type="health", id=node_id)
            with lock:
                known = node_id in resources["node"]
            if not known:
                return self.reply(404, {"code": 404, "error": "node not registered"})
            return self.reply(200, {"health": str(int(time.time()))})
        self.record()
        return self.reply(404, {"code": 404, "error": "not found"})

    def do_DELETE(self):
        path = self.path.rstrip("/")
        if path.startswith(REG + "/resource/"):
            parts = path[len(REG) + len("/resource/"):].split("/")
            kind = parts[0][:-1]
            rid = parts[1] if len(parts) > 1 else ""
            self.record(type=kind, id=rid)
            with lock:
                found = resources.get(kind, {}).pop(rid, None)
            return self.reply(204) if found is not None else self.reply(404, {"code": 404, "error": "not found"})
        self.record()
        return self.reply(404, {"code": 404, "error": "not found"})


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=18210)
    parser.add_argument("--host", default="0.0.0.0")
    args = parser.parse_args()
    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
