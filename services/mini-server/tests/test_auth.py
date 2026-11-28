"""Login, sessions and API authentication (temporary files, no MQTT)."""
import base64
from datetime import datetime, timedelta, timezone
import os
from pathlib import Path
import stat
import tempfile
import unittest

import auth

FAST = 1000  # PBKDF2 iterations for tests only


class PasswordTests(unittest.TestCase):
    def test_hash_and_verify(self):
        encoded = auth.hash_password("correct horse", iterations=FAST)
        self.assertTrue(encoded.startswith("pbkdf2_sha256:1000:"))
        self.assertNotIn("$", encoded)
        self.assertTrue(auth.verify_password("correct horse", encoded))
        self.assertFalse(auth.verify_password("correct horsE", encoded))
        self.assertFalse(auth.verify_password("x", "garbage"))
        self.assertFalse(auth.verify_password("x", ""))
        # Same password, different salt.
        self.assertNotEqual(encoded, auth.hash_password("correct horse", iterations=FAST))


class SessionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(ignore_cleanup_errors=True)
        self.store = auth.SessionStore(Path(self.temp.name) / "auth.db", 3600)

    def tearDown(self):
        self.temp.cleanup()

    def test_create_lookup_revoke(self):
        token, expires_at = self.store.create("admin")
        self.assertGreaterEqual(len(token), 40)
        self.assertEqual(self.store.lookup(token), "admin")
        self.assertIsNone(self.store.lookup(token + "x"))
        self.assertIsNone(self.store.lookup(""))
        self.assertTrue(self.store.revoke(token))
        self.assertIsNone(self.store.lookup(token))
        self.assertFalse(self.store.revoke(token))
        self.assertTrue(expires_at > datetime.now(timezone.utc).isoformat())

    def test_expiry(self):
        past = datetime.now(timezone.utc) - timedelta(hours=2)
        token, _ = self.store.create("admin", now=past)
        self.assertIsNone(self.store.lookup(token))

    def test_token_is_not_stored_in_clear(self):
        token, _ = self.store.create("admin")
        data = (Path(self.temp.name) / "auth.db").read_bytes()
        self.assertNotIn(token.encode(), data)


class ThrottleTests(unittest.TestCase):
    def test_lockout_and_reset(self):
        now = [0.0]
        throttle = auth.LoginThrottle(max_failures=3, window_seconds=60,
                                      lockout_seconds=30, clock=lambda: now[0])
        for _ in range(2):
            throttle.failure("1.2.3.4")
        self.assertEqual(throttle.retry_after("1.2.3.4"), 0)
        throttle.failure("1.2.3.4")
        self.assertEqual(throttle.retry_after("1.2.3.4"), 30)
        self.assertEqual(throttle.retry_after("5.6.7.8"), 0)
        now[0] = 31.0
        self.assertEqual(throttle.retry_after("1.2.3.4"), 0)
        throttle.failure("1.2.3.4")
        throttle.success("1.2.3.4")
        throttle.failure("1.2.3.4")
        throttle.failure("1.2.3.4")
        self.assertEqual(throttle.retry_after("1.2.3.4"), 0)


class EnvFileTests(unittest.TestCase):
    def test_set_env_values_keeps_other_lines(self):
        with tempfile.TemporaryDirectory() as folder:
            env = Path(folder) / ".env"
            env.write_text("MQTT_PASSWORD=keep\n# comment\nSAEMS_ADMIN_USER=old\n",
                           encoding="utf-8")
            if os.name == "posix":
                env.chmod(0o600)
            auth.set_env_values(env, {"SAEMS_ADMIN_USER": "admin",
                                      "SAEMS_ADMIN_PASSWORD_HASH": "pbkdf2_sha256:1:a:b"})
            lines = env.read_text(encoding="utf-8").splitlines()
            self.assertEqual(lines, ["MQTT_PASSWORD=keep", "# comment",
                                     "SAEMS_ADMIN_USER=admin",
                                     "SAEMS_ADMIN_PASSWORD_HASH=pbkdf2_sha256:1:a:b"])
            if os.name == "posix":
                self.assertEqual(stat.S_IMODE(env.stat().st_mode), 0o600)


class ApiAuthTests(unittest.TestCase):
    def setUp(self):
        try:
            from fastapi.testclient import TestClient
        except (ImportError, RuntimeError) as error:
            self.skipTest(f"TestClient unavailable: {error}")
        import api

        self.api = api
        self.temp = tempfile.TemporaryDirectory(ignore_cleanup_errors=True)
        self.saved = (api.API_ACCESS_TOKEN, api.ADMIN_USER, api.ADMIN_PASSWORD_HASH,
                      api.sessions, api.login_throttle, api.basic_cache, api.DB_PATH)
        api.API_ACCESS_TOKEN = "t" * 40
        api.ADMIN_USER = "admin"
        api.ADMIN_PASSWORD_HASH = auth.hash_password("s3cret-pass", iterations=FAST)
        api.sessions = auth.SessionStore(Path(self.temp.name) / "auth.db", 3600)
        api.login_throttle = auth.LoginThrottle(max_failures=3)
        api.basic_cache = auth.CredentialCache()
        api.DB_PATH = Path(self.temp.name) / "telemetry.db"
        self.client = TestClient(api.app)

    def tearDown(self):
        (self.api.API_ACCESS_TOKEN, self.api.ADMIN_USER, self.api.ADMIN_PASSWORD_HASH,
         self.api.sessions, self.api.login_throttle, self.api.basic_cache,
         self.api.DB_PATH) = self.saved
        self.temp.cleanup()

    def login(self, password="s3cret-pass"):
        return self.client.post("/auth/login",
                                json={"username": "admin", "password": password})

    def test_requires_authentication(self):
        self.assertEqual(self.client.get("/health").status_code, 200)
        self.assertEqual(self.client.get("/api/v1/alerts").status_code, 401)
        self.assertEqual(self.client.get(
            "/api/v1/alerts", headers={"Authorization": "Bearer wrong"}).status_code, 401)

    def test_legacy_api_token_still_works(self):
        response = self.client.get("/auth/me",
                                   headers={"Authorization": "Bearer " + "t" * 40})
        self.assertEqual(response.json()["method"], "api_token")

    def test_login_session_and_logout(self):
        response = self.login()
        self.assertEqual(response.status_code, 200, response.text)
        token = response.json()["access_token"]
        headers = {"Authorization": f"Bearer {token}"}
        me = self.client.get("/auth/me", headers=headers).json()
        self.assertEqual((me["username"], me["method"]), ("admin", "session"))
        self.assertEqual(self.client.get("/api/v1/alerts", headers=headers).status_code, 200)
        self.assertEqual(self.client.post("/auth/logout", headers=headers).json()["status"],
                         "logged_out")
        self.assertEqual(self.client.get("/api/v1/alerts", headers=headers).status_code, 401)

    def test_wrong_password_and_throttle(self):
        for _ in range(3):
            self.assertEqual(self.login("nope").status_code, 401)
        response = self.login()
        self.assertEqual(response.status_code, 429)
        self.assertIn("Retry-After", response.headers)

    def test_dashboard_basic_auth(self):
        def basic(user, password):
            value = base64.b64encode(f"{user}:{password}".encode()).decode()
            return {"Authorization": f"Basic {value}"}
        self.assertEqual(self.client.get("/auth/me", headers=basic("admin", "s3cret-pass"))
                         .json()["username"], "admin")
        self.assertEqual(self.client.get("/auth/me", headers=basic("saems", "t" * 40))
                         .json()["method"], "basic")
        self.assertEqual(self.client.get("/auth/me", headers=basic("admin", "bad"))
                         .status_code, 401)

    def test_login_not_configured(self):
        self.api.ADMIN_PASSWORD_HASH = ""
        self.assertEqual(self.login().status_code, 503)


if __name__ == "__main__":
    unittest.main()
