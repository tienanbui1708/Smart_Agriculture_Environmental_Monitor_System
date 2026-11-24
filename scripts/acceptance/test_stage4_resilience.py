"""Unit tests for the stage 4 analysis (no network).

    python -m unittest discover -s scripts/acceptance
"""
from datetime import datetime, timedelta, timezone
from pathlib import Path
import tempfile
import unittest

import stage4_resilience as stage4

T0 = datetime(2026, 12, 1, 8, 0, tzinfo=timezone.utc)


def record(seq, sampled_min, received_min, boot="b1"):
    return {"boot_id": boot, "seq": seq,
            "timestamp": (T0 + timedelta(minutes=sampled_min)).isoformat(),
            "received_at": (T0 + timedelta(minutes=received_min)).isoformat()}


class AnalysisTests(unittest.TestCase):
    def test_continuous_with_backlog(self):
        records = [record(1, 0, 0), record(2, 1, 1),
                   record(3, 2, 6), record(4, 3, 6),   # sampled offline, sent later
                   record(5, 6, 6)]
        result = stage4.analyze(records, T0 + timedelta(minutes=1.5), T0 + timedelta(minutes=5))
        self.assertTrue(result.continuous)
        self.assertEqual(result.backlog, 2)
        self.assertEqual(result.max_delivery_delay_s, 240)

    def test_gap_and_duplicate(self):
        records = [record(1, 0, 0), record(2, 1, 1), record(2, 1, 1), record(5, 4, 4)]
        result = stage4.analyze(records)
        self.assertFalse(result.continuous)
        self.assertEqual(result.boots[0].missing, [3, 4])
        self.assertEqual(result.boots[0].duplicates, 1)

    def test_reboot_starts_new_sequence(self):
        records = [record(10, 0, 0, "b1"), record(11, 1, 1, "b1"),
                   record(1000001, 2, 2, "b2"), record(1000002, 3, 3, "b2")]
        result = stage4.analyze(records)
        self.assertTrue(result.continuous)
        self.assertEqual([boot.boot_id for boot in result.boots], ["b1", "b2"])

    def test_compress(self):
        self.assertEqual(stage4.compress([1, 2, 3, 7, 9, 10]), "1-3, 7, 9-10")
        self.assertEqual(stage4.compress([]), "-")

    def test_report_verdict(self):
        good = stage4.analyze([record(1, 0, 0), record(2, 1, 5)], T0, T0 + timedelta(minutes=2))
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "r.md"
            self.assertTrue(stage4.report(path, "farm-01", good,
                                          {"start": T0, "phone_ok": True, "synced": True}, []))
            self.assertIn("ĐẠT", path.read_text(encoding="utf-8"))
            self.assertFalse(stage4.report(path, "farm-01", good,
                                           {"start": T0, "phone_ok": False, "synced": True}, []))


if __name__ == "__main__":
    unittest.main()
