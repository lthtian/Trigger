"""Bounded, in-memory receiver for Android SMS forwarding tests."""

import argparse
import base64
import hashlib
import hmac
import json
import re
import secrets
import time
from collections import OrderedDict
from datetime import datetime
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote_plus, urlsplit


def extract_code(text):
    matches = re.findall(
        r"(?:验证码|校验码|动态码|\bcode\b)[^0-9\r\n]{0,24}([0-9]{4,8})(?![0-9])",
        text, flags=re.IGNORECASE,
    )
    codes = set(matches)
    return next(iter(codes)) if len(codes) == 1 else None


class EventStore:
    def __init__(self, ttl=300, capacity=32):
        self.ttl = ttl
        self.capacity = capacity
        self.records = OrderedDict()

    def expire(self, now):
        for key in list(self.records):
            if now - self.records[key]["receivedAt"] >= self.ttl:
                del self.records[key]

    def add(self, key, event, now):
        self.expire(now)
        if key in self.records:
            return False
        self.records[key] = event
        while len(self.records) > self.capacity:
            self.records.popitem(last=False)
        return True

    def events(self, now):
        self.expire(now)
        return list(reversed(self.records.values()))


class Receiver(HTTPServer):
    def service_actions(self):
        self.events.expire(time.time())


class Handler(BaseHTTPRequestHandler):
    def setup(self):
        self.request.settimeout(5)
        super().setup()

    def log_message(self, format, *args):
        pass

    def reply(self, status, data):
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/health":
            self.reply(200, {"ok": True})
        elif self.path == "/events":
            expected = ("Bearer " + self.server.read_token).encode()
            actual = self.headers.get("Authorization", "").encode()
            if not hmac.compare_digest(expected, actual):
                self.reply(401, {"error": "unauthorized"})
                return
            self.reply(200, {"events": self.server.events.events(time.time())})
        else:
            self.reply(404, {"error": "not_found"})

    def do_POST(self):
        path = urlsplit(self.path).path
        if path not in ("/sms", "/gateway"):
            self.reply(404, {"error": "not_found"})
            return
        content_type = "application/json" if path == "/gateway" else "application/x-www-form-urlencoded"
        if self.headers.get_content_type() != content_type:
            self.reply(415, {"error": "unsupported_type"})
            return
        try:
            lengths = self.headers.get_all("Content-Length", [])
            if len(lengths) != 1 or self.headers.get("Transfer-Encoding"):
                raise ValueError("ambiguous_length")
            length = int(lengths[0])
            if length > 8192:
                self.reply(413, {"error": "too_large"})
                return
            if length <= 0:
                raise ValueError("empty_body")
            body = self.rfile.read(length)
            if len(body) != length:
                raise ValueError("incomplete_body")
            if path == "/gateway":
                self.receive_gateway(body)
                return
            fields = parse_qs(body.decode("utf-8"), keep_blank_values=True,
                              strict_parsing=True, max_num_fields=8, errors="strict")
            required = ("from", "content", "timestamp", "sign")
            if any(len(fields.get(key, [])) != 1 for key in required):
                raise ValueError("missing_or_duplicate_field")
            sender, content, timestamp, signature = (fields[key][0] for key in required)
            if not sender or len(sender) > 128 or not content or len(content) > 2048:
                raise ValueError("invalid_message")
            forwarded_at = int(timestamp) / 1000
        except (ValueError, UnicodeError, OverflowError):
            self.reply(400, {"error": "invalid_request"})
            return
        now = time.time()
        expected = base64.b64encode(hmac.digest(
            self.server.secret.encode(),
            (timestamp + "\n" + self.server.secret).encode(), "sha256"
        ))
        # SmsForwarder URL-encodes its signature before form encoding.
        actual = unquote_plus(signature).encode()
        if not -60 <= now - forwarded_at < 300 or not hmac.compare_digest(expected, actual):
            self.reply(401, {"error": "unauthorized"})
            return
        key = hashlib.sha256(json.dumps([sender, content, timestamp]).encode()).hexdigest()
        event = {
            "id": secrets.token_hex(8), "sender": sender,
            "code": extract_code(content), "receivedAt": now,
            "forwardedAt": forwarded_at,
        }
        accepted = self.server.events.add(key, event, now)
        self.reply(200, {"ok": True, "accepted": accepted, "codeFound": event["code"] is not None})

    def receive_gateway(self, body):
        timestamp = self.headers.get("X-Timestamp", "")
        signature = self.headers.get("X-Signature", "").encode()
        try:
            forwarded_at = int(timestamp)
        except ValueError:
            self.reply(401, {"error": "unauthorized"})
            return
        now = time.time()
        expected = hmac.new(self.server.secret.encode(), body + timestamp.encode(), hashlib.sha256).hexdigest().encode()
        if not -60 <= now - forwarded_at < 300 or not hmac.compare_digest(expected, signature):
            self.reply(401, {"error": "unauthorized"})
            return
        try:
            envelope = json.loads(body)
            if not isinstance(envelope, dict) or envelope.get("event") != "sms:received":
                raise ValueError("unsupported_event")
            payload = envelope["payload"]
            device, delivery = envelope["deviceId"], envelope["id"]
            sender, message = payload["sender"], payload["message"]
            if any(not isinstance(value, str) or not 1 <= len(value) <= 128
                   for value in (device, delivery, sender)):
                raise ValueError("invalid_identity")
            if not isinstance(message, str) or not 1 <= len(message) <= 2048:
                raise ValueError("invalid_message")
            received = datetime.fromisoformat(payload["receivedAt"])
            if received.tzinfo is None:
                raise ValueError("missing_timezone")
            phone_received_at = received.timestamp()
        except (ValueError, TypeError, KeyError, OverflowError):
            self.reply(400, {"error": "invalid_request"})
            return
        if not -60 <= now - phone_received_at < 300:
            self.reply(200, {"ok": True, "accepted": False, "reason": "expired"})
            return
        event = {"id": delivery, "deviceId": device, "sender": sender,
                 "code": extract_code(message), "receivedAt": now,
                 "phoneReceivedAt": phone_received_at, "forwardedAt": forwarded_at}
        identity = [device, sender, message, phone_received_at, payload.get("simNumber")]
        key = "gateway/" + hashlib.sha256(json.dumps(identity).encode()).hexdigest()
        accepted = self.server.events.add(key, event, now)
        self.reply(200, {"ok": True, "accepted": accepted, "codeFound": event["code"] is not None})


def create_server(host, port, secret, read_token):
    if len(secret) < 24 or len(read_token) < 24 or secret == read_token:
        raise ValueError("Separate sender and reader credentials of at least 24 characters are required")
    server = Receiver((host, port), Handler)
    server.secret = secret
    server.read_token = read_token
    server.events = EventStore()
    return server


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", required=True)
    args = parser.parse_args()
    config = json.loads(Path(args.config).read_text(encoding="utf-8"))
    server = create_server("127.0.0.1", config["port"], config["secret"], config["readToken"])
    try:
        server.serve_forever()
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
