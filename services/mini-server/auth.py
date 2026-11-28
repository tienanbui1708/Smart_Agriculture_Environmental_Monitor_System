"""User login for the SAEMS API.

- One administrator account from the environment:
  SAEMS_ADMIN_USER (default "admin") and SAEMS_ADMIN_PASSWORD_HASH
  (PBKDF2-SHA256, created with ``python auth.py set-password``).
- POST /auth/login returns a random bearer token. Only its SHA-256 is stored
  (data/auth.db), with an expiry (SAEMS_SESSION_TTL_HOURS, default 720 h),
  so a leaked database cannot be replayed and logout revokes immediately.
- Repeated failures from one client are throttled.

The long-lived API_ACCESS_TOKEN keeps working for scripts and older app
builds; see api.py.

Command line (run on the server, the password is never echoed or logged):
    python auth.py set-password [--env .env] [--user admin]
    python auth.py hash-password
"""
from __future__ import annotations

import argparse
import base64
from contextlib import closing
from datetime import datetime, timedelta, timezone
import getpass
import hashlib
import hmac
import os
from pathlib import Path
import secrets
import sqlite3
import sys
import threading
import time

PBKDF2_ITERATIONS = 310_000
HASH_PREFIX = "pbkdf2_sha256"
MIN_PASSWORD_LENGTH = 10


def hash_password(password: str, iterations: int = PBKDF2_ITERATIONS,
                  salt: bytes | None = None) -> str:
    salt = salt or secrets.token_bytes(16)
    digest = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, iterations)
    # ":" separator: "$" could be expanded by shells or .env loaders.
    return ":".join((
        HASH_PREFIX,
        str(iterations),
        base64.b64encode(salt).decode("ascii"),
        base64.b64encode(digest).decode("ascii"),
    ))


def verify_password(password: str, encoded: str) -> bool:
    try:
        prefix, iterations, salt, expected = encoded.split(":")
        if prefix != HASH_PREFIX:
            return False
        digest = hashlib.pbkdf2_hmac(
            "sha256", password.encode("utf-8"), base64.b64decode(salt), int(iterations))
        return hmac.compare_digest(digest, base64.b64decode(expected))
    except (ValueError, TypeError):
        return False


def _token_hash(token: str) -> str:
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


class SessionStore:
    """Opaque bearer tokens with expiry, stored hashed in SQLite."""

    def __init__(self, path: Path, ttl_seconds: int):
        self.path = Path(path)
        self.ttl_seconds = ttl_seconds

    def _connect(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        connection = sqlite3.connect(self.path, timeout=10)
        connection.execute(
            """
            CREATE TABLE IF NOT EXISTS sessions (
                token_hash TEXT PRIMARY KEY,
                username TEXT NOT NULL,
                created_at TEXT NOT NULL,
                expires_at TEXT NOT NULL
            )
            """
        )
        return connection

    def create(self, username: str, now: datetime | None = None) -> tuple[str, str]:
        now = now or datetime.now(timezone.utc)
        token = secrets.token_urlsafe(32)
        expires_at = (now + timedelta(seconds=self.ttl_seconds)).isoformat()
        with closing(self._connect()) as connection:
            connection.execute("DELETE FROM sessions WHERE expires_at < ?", (now.isoformat(),))
            connection.execute(
                "INSERT INTO sessions (token_hash, username, created_at, expires_at) "
                "VALUES (?, ?, ?, ?)",
                (_token_hash(token), username, now.isoformat(), expires_at),
            )
            connection.commit()
        return token, expires_at

    def lookup(self, token: str, now: datetime | None = None) -> str | None:
        if not token:
            return None
        now = now or datetime.now(timezone.utc)
        with closing(self._connect()) as connection:
            row = connection.execute(
                "SELECT username, expires_at FROM sessions WHERE token_hash = ?",
                (_token_hash(token),),
            ).fetchone()
            if row is None:
                return None
            if row[1] < now.isoformat():
                connection.execute("DELETE FROM sessions WHERE token_hash = ?",
                                   (_token_hash(token),))
                connection.commit()
                return None
            return row[0]

    def revoke(self, token: str) -> bool:
        with closing(self._connect()) as connection:
            cursor = connection.execute("DELETE FROM sessions WHERE token_hash = ?",
                                        (_token_hash(token),))
            connection.commit()
            return cursor.rowcount > 0


class LoginThrottle:
    """At most max_failures failed logins per client within window_seconds;
    then the client waits lockout_seconds."""

    def __init__(self, max_failures: int = 5, window_seconds: int = 900,
                 lockout_seconds: int = 300, clock=time.monotonic):
        self.max_failures = max_failures
        self.window_seconds = window_seconds
        self.lockout_seconds = lockout_seconds
        self.clock = clock
        self._failures: dict[str, list[float]] = {}
        self._locked_until: dict[str, float] = {}
        self._lock = threading.Lock()

    def retry_after(self, client: str) -> int:
        with self._lock:
            remaining = self._locked_until.get(client, 0.0) - self.clock()
            return max(int(remaining + 0.999), 0)

    def failure(self, client: str) -> None:
        with self._lock:
            now = self.clock()
            recent = [stamp for stamp in self._failures.get(client, [])
                      if now - stamp < self.window_seconds]
            recent.append(now)
            self._failures[client] = recent
            if len(recent) >= self.max_failures:
                self._locked_until[client] = now + self.lockout_seconds
                self._failures[client] = []

    def success(self, client: str) -> None:
        with self._lock:
            self._failures.pop(client, None)
            self._locked_until.pop(client, None)


class CredentialCache:
    """Remember recently verified HTTP Basic credentials for a few minutes,
    so the dashboard does not pay a full PBKDF2 per request."""

    def __init__(self, ttl_seconds: int = 600, clock=time.monotonic):
        self.ttl_seconds = ttl_seconds
        self.clock = clock
        self._entries: dict[str, float] = {}
        self._lock = threading.Lock()

    def _key(self, username: str, password: str) -> str:
        return hashlib.sha256(f"{username}\0{password}".encode("utf-8")).hexdigest()

    def contains(self, username: str, password: str) -> bool:
        with self._lock:
            expiry = self._entries.get(self._key(username, password))
            return expiry is not None and expiry > self.clock()

    def add(self, username: str, password: str) -> None:
        with self._lock:
            self._entries[self._key(username, password)] = self.clock() + self.ttl_seconds


# ---- command line -----------------------------------------------------------

def _prompt_new_password() -> str:
    while True:
        first = getpass.getpass("New admin password: ")
        if len(first) < MIN_PASSWORD_LENGTH:
            print(f"Use at least {MIN_PASSWORD_LENGTH} characters.", file=sys.stderr)
            continue
        if getpass.getpass("Repeat password: ") != first:
            print("Passwords do not match.", file=sys.stderr)
            continue
        return first


def set_env_values(env_path: Path, values: dict[str, str]) -> None:
    """Replace or append KEY=value lines, keeping every other line and the
    file permissions; written atomically."""
    lines = env_path.read_text(encoding="utf-8").splitlines() if env_path.exists() else []
    remaining = dict(values)
    output = []
    for line in lines:
        key = line.split("=", 1)[0].strip()
        if key in remaining:
            output.append(f"{key}={remaining.pop(key)}")
        else:
            output.append(line)
    output.extend(f"{key}={value}" for key, value in remaining.items())
    mode = env_path.stat().st_mode & 0o777 if env_path.exists() else 0o600
    temporary = env_path.with_name(env_path.name + ".tmp")
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, mode)
    with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
        handle.write("\n".join(output) + "\n")
    os.replace(temporary, env_path)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="SAEMS admin login helper")
    commands = parser.add_subparsers(dest="command", required=True)
    set_password = commands.add_parser(
        "set-password", help="store SAEMS_ADMIN_USER/SAEMS_ADMIN_PASSWORD_HASH in .env")
    set_password.add_argument("--env", default=str(Path(__file__).resolve().parent / ".env"))
    set_password.add_argument("--user", default="admin")
    commands.add_parser("hash-password", help="print a password hash only")
    args = parser.parse_args(argv)

    encoded = hash_password(_prompt_new_password())
    if args.command == "hash-password":
        print(f"SAEMS_ADMIN_PASSWORD_HASH={encoded}")
        return 0
    if not args.user or any(character in args.user for character in "=\n\r "):
        print("Invalid user name.", file=sys.stderr)
        return 2
    set_env_values(Path(args.env), {
        "SAEMS_ADMIN_USER": args.user,
        "SAEMS_ADMIN_PASSWORD_HASH": encoded,
    })
    print(f"Saved login for user '{args.user}' in {args.env}. "
          "Restart saems-api to apply.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
