import hashlib
import hmac
import json
import time
from datetime import datetime, timedelta, timezone

from test_receiver import READ_TOKEN, SECRET, ReceiverTests


class GatewayTests(ReceiverTests):
    def payload(self, message="【TriggerTest】验证码 008866，5分钟有效。"):
        return {
            "deviceId": "test-phone", "event": "sms:received", "id": "test-delivery",
            "payload": {"messageId": "test-message", "message": message,
                        "sender": "106000000", "simNumber": 1,
                        "receivedAt": datetime.now(timezone.utc).isoformat()}
        }

    def gateway(self, payload, timestamp=None, tamper=False):
        body = json.dumps(payload, ensure_ascii=False).encode()
        timestamp = str(int(time.time()) if timestamp is None else timestamp)
        signature = hmac.new(SECRET.encode(), body + timestamp.encode(), hashlib.sha256).hexdigest()
        if tamper:
            body = body.replace(b"008866", b"999999")
        return self.request("POST", "/gateway", body, {
            "Content-Type": "application/json", "X-Timestamp": timestamp,
            "X-Signature": signature,
        })

    def test_gateway_receives_signed_json_and_deduplicates(self):
        payload = self.payload()
        status, result = self.gateway(payload)
        self.assertEqual(status, 200)
        self.assertTrue(result["accepted"])
        self.assertFalse(self.gateway(payload)[1]["accepted"])
        event = self.events()[1]["events"][0]
        self.assertEqual(event["code"], "008866")
        self.assertEqual(event["deviceId"], "test-phone")
        self.assertNotIn("message", event)

    def test_gateway_rejects_altered_body_or_stale_signature(self):
        self.assertEqual(self.gateway(self.payload(), tamper=True)[0], 401)
        self.assertEqual(self.gateway(self.payload(), timestamp=int(time.time()) - 301)[0], 401)
        self.assertEqual(self.events()[1]["events"], [])

    def test_gateway_deduplicates_sms_with_different_delivery_ids(self):
        payload = self.payload()
        self.assertTrue(self.gateway(payload)[1]["accepted"])
        payload["id"] = "another-delivery"
        self.assertFalse(self.gateway(payload)[1]["accepted"])
        self.assertEqual(len(self.events()[1]["events"]), 1)

    def test_gateway_keeps_later_sms_and_other_devices(self):
        payload = self.payload()
        self.assertTrue(self.gateway(payload)[1]["accepted"])
        payload["id"] = "later-delivery"
        received = datetime.fromisoformat(payload["payload"]["receivedAt"])
        payload["payload"]["receivedAt"] = (received + timedelta(seconds=1)).isoformat()
        self.assertTrue(self.gateway(payload)[1]["accepted"])
        payload["id"] = "other-device-delivery"
        payload["deviceId"] = "other-phone"
        self.assertTrue(self.gateway(payload)[1]["accepted"])
        self.assertEqual(len(self.events()[1]["events"]), 3)

    def test_gateway_rejects_bad_payload(self):
        for payload in ({}, [], {"event": "sms:sent"}, self.payload(message=123)):
            self.assertEqual(self.gateway(payload)[0], 400)

    def test_gateway_excludes_expired_sms_even_with_fresh_delivery(self):
        payload = self.payload()
        payload["payload"]["receivedAt"] = "2020-01-01T00:00:00+00:00"
        status, result = self.gateway(payload)
        self.assertEqual(status, 200)
        self.assertFalse(result["accepted"])
        self.assertEqual(self.events()[1]["events"], [])
