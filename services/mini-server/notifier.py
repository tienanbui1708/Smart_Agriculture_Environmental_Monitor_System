"""Telegram notifications for SAEMS alerts.

Configure in .env:
    TELEGRAM_BOT_TOKEN=123456:ABC...   (from @BotFather)
    TELEGRAM_CHAT_ID=123456789         (comma-separated for several chats)

The bot token is a secret: it is never logged, and request URLs containing
it are never included in log messages.
"""
from __future__ import annotations

import json
import logging
import os
import queue
import threading
import time
import urllib.error
import urllib.request

TELEGRAM_API = "https://api.telegram.org"
RETRY_DELAYS_S = (2, 5, 15)


class NullNotifier:
    configured = False

    def send(self, text: str) -> None:
        logging.info("Telegram not configured; alert kept in database only")

    def send_now(self, text: str) -> bool:
        return False


class TelegramNotifier:
    configured = True

    def __init__(self, token: str, chat_ids: list[str], timeout_s: float = 10.0):
        if not token or not chat_ids:
            raise ValueError("Telegram token and chat id are required")
        self._token = token
        self._chat_ids = chat_ids
        self._timeout_s = timeout_s
        self._queue: queue.Queue[str] = queue.Queue(maxsize=200)
        self._worker = threading.Thread(target=self._run, name="telegram", daemon=True)
        self._worker.start()

    def send(self, text: str) -> None:
        """Queue a message; delivery and retries happen in the background."""
        try:
            self._queue.put_nowait(text)
        except queue.Full:
            logging.error("Telegram queue full; dropping notification")

    def send_now(self, text: str) -> bool:
        """Deliver synchronously (used by the test endpoint)."""
        return all(self._post(chat_id, text) for chat_id in self._chat_ids)

    def _run(self) -> None:
        while True:
            text = self._queue.get()
            for chat_id in self._chat_ids:
                for attempt, delay in enumerate((0, *RETRY_DELAYS_S)):
                    if delay:
                        time.sleep(delay)
                    if self._post(chat_id, text):
                        break
                    logging.warning("Telegram delivery attempt %d failed", attempt + 1)
                else:
                    logging.error("Telegram notification dropped after retries")

    def _post(self, chat_id: str, text: str) -> bool:
        body = json.dumps(
            {"chat_id": chat_id, "text": text, "disable_web_page_preview": True}
        ).encode("utf-8")
        request = urllib.request.Request(
            f"{TELEGRAM_API}/bot{self._token}/sendMessage",
            data=body,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=self._timeout_s) as response:
                return 200 <= response.status < 300
        except urllib.error.HTTPError as error:
            # Status only: the URL contains the bot token.
            logging.warning("Telegram API returned HTTP %s", error.code)
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            logging.warning("Telegram API unreachable: %s", type(error).__name__)
        return False


def notifier_from_env():
    token = os.getenv("TELEGRAM_BOT_TOKEN", "").strip()
    chat_ids = [
        value.strip()
        for value in os.getenv("TELEGRAM_CHAT_ID", "").split(",")
        if value.strip()
    ]
    if not token or not chat_ids:
        return NullNotifier()
    return TelegramNotifier(token, chat_ids)
