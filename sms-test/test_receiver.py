import base64
import hashlib
import hmac
import http.client
import json
import threading
import time
import unittest
import urllib.parse

import receiver


SECRET = "test-secret-with-no-real-account-access"
READ_TOKEN = "test-reader-with-no-real-account-access"


def signed_form(content, sender="106000000", timestamp=None):
    timestamp = str(int(time.time() * 1000) if timestamp is None else timestamp)
    signature = base64.b64encode(hmac.digest(
        SECRET.encode(), (timestamp + "\n" + SECRET).encode(), "sha256"
    )).decode()
    return urllib.parse.urlencode({
        "from": sender, "content": content, "timestamp": timestamp,
        "sign": urllib.parse.quote_plus(signature)
    }).encode()


class ReceiverTests(unittest.TestCase):
    def setUp(self):
        self.server = receiver.create_server("127.0.0.1", 0, SECRET, READ_TOKEN)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.port = self.server.server_address[1]

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def request(self, method, path, body=None, headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=3)
        try:
            connection.request(method, path, body, headers or {})
            response = connection.getresponse()
            data = response.read()
            return response.status, json.loads(data)
        finally:
            connection.close()

    def post(self, body):
        return self.request("POST", "/sms", body, {
            "Content-Type": "application/x-www-form-urlencoded"
        })

    def events(self):
        return self.request("GET", "/events", headers={
            "Authorization": "Bearer " + READ_TOKEN
        })

    def test_health_and_authenticated_receive(self):
        self.assertEqual(self.request("GET", "/health"), (200, {"ok": True}))
        status, result = self.post(signed_form("【TriggerTest】验证码 012345，5分钟有效。"))
        self.assertEqual(status, 200)
        self.assertTrue(result["accepted"])
        status, data = self.events()
        self.assertEqual(status, 200)
        self.assertEqual(data["events"][0]["code"], "012345")
        self.assertEqual(data["events"][0]["sender"], "106000000")
        self.assertNotIn("content", data["events"][0])
        self.assertIn("receivedAt", data["events"][0])

    def test_missing_or_wrong_signature_rejected(self):
        for body in (b"from=106&content=123456", signed_form("验证码123456") + b"&sign=wrong"):
            status, _ = self.post(body)
            self.assertIn(status, (400, 401))
        self.assertEqual(self.events()[1]["events"], [])

    def test_reader_is_separate_from_sender(self):
        self.assertEqual(self.request("GET", "/events")[0], 401)
        self.assertEqual(self.request("GET", "/events", headers={
            "Authorization": "Bearer " + SECRET
        })[0], 401)

    def test_stale_and_future_messages_rejected(self):
        now = int(time.time() * 1000)
        for offset in (-301000, 61000):
            self.assertEqual(self.post(signed_form("验证码123456", timestamp=now + offset))[0], 401)

    def test_retry_does_not_duplicate(self):
        body = signed_form("验证码123456")
        self.assertTrue(self.post(body)[1]["accepted"])
        self.assertFalse(self.post(body)[1]["accepted"])
        self.assertEqual(len(self.events()[1]["events"]), 1)

    def test_malformed_oversized_and_unsupported_requests(self):
        self.assertEqual(self.post(b"\xff")[0], 400)
        self.assertEqual(self.post(b"a" * 8193)[0], 413)
        self.assertEqual(self.request("POST", "/sms", b"{}", {"Content-Type": "application/json"})[0], 415)
        self.assertEqual(self.request("GET", "/absent")[0], 404)
        self.assertEqual(self.request("GET", "/sms")[0], 404)
        self.assertEqual(self.request("POST", "/events", b"")[0], 404)

    def test_phone_channel_probe_is_accepted_without_guessing_code(self):
        self.assertEqual(self.post(signed_form("通道测试内容"))[0], 200)
        self.assertIsNone(self.events()[1]["events"][0]["code"])


class CodeTests(unittest.TestCase):
    def test_extracts_keyword_associated_numeric_codes(self):
        for text, expected in (
            ("验证码：0088，请勿泄露", "0088"),
            ("您的登录验证码为123456，5分钟内有效", "123456"),
            ("Your verification code is 98765432.", "98765432"),
            ("订单编号123456，余额88.00", None),
            ("验证码123456或验证码654321", None),
            ("验证码是13800000000", None),
        ):
            with self.subTest(text=text):
                self.assertEqual(receiver.extract_code(text), expected)

    def test_store_expires_and_bounds_memory(self):
        store = receiver.EventStore(ttl=300, capacity=2)
        first = {"id": "one", "receivedAt": 1000}
        store.add("a", first, now=1000)
        self.assertEqual(store.events(now=1299), [first])
        self.assertEqual(store.events(now=1300), [])
        for index in range(3):
            store.add(str(index), {"id": str(index), "receivedAt": 2000 + index}, now=2000 + index)
        self.assertEqual([event["id"] for event in store.events(now=2003)], ["2", "1"])


if __name__ == "__main__":
    unittest.main()
