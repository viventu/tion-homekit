"""Synthetic CSV and HTML fixtures only; never contact or read a real device."""

import csv
from datetime import datetime, timedelta, timezone
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]


def load_script(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


soak = load_script("review-soak")
monitor = load_script("monitor-diagnostics")


def samples(count=61):
    result = []
    start = datetime(2026, 1, 1, tzinfo=timezone.utc)
    for index in range(count):
        row = dict.fromkeys(monitor.FIELDS, "0")
        row.update(timestamp_utc=(start + timedelta(minutes=index)).isoformat(),
                   reachable="1", reset_reason="Power-on", uart_task="1",
                   homekit_status=soak.CONNECTED, state_fresh="1",
                   control_applied_failed="0 / 0", max_heartbeat_tx_gap_ms="3010",
                   minimum_free_heap_bytes="140000", uptime_seconds=str(10 + index * 60),
                   uart_stack_free_bytes="1400", uart_stall_restarts="0 / armed",
                   valid_states=str(1 + index * 30), heartbeat_responses=str(1 + index * 20))
        result.append(row)
    return result


class SoakReviewTests(unittest.TestCase):
    def review(self, rows, expected_version=None):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "synthetic.csv"
            with path.open("w", newline="") as output:
                writer = csv.DictWriter(output, fieldnames=monitor.FIELDS)
                writer.writeheader()
                writer.writerows(rows)
            return soak.review(path, 1, 60, expected_version)

    def test_complete_and_in_progress(self):
        self.assertEqual(self.review(samples())["status"], "PASS")
        self.assertEqual(self.review(samples(2))["status"], "IN_PROGRESS")

    def test_expected_firmware_version(self):
        rows = samples()
        for row in rows:
            row["firmware_version"] = "0.2.0-gm1-diag"
        self.assertEqual(self.review(rows, "0.2.0-gm1-diag")["status"], "PASS")
        for version in ("0.2.0-rc2-diag", ""):
            rows[-1]["firmware_version"] = version
            self.assertIn("unexpected or missing firmware version",
                          self.review(rows, "0.2.0-gm1-diag")["problems"])

    def test_no_uart_progress_even_when_fresh_flag_is_stuck(self):
        rows = samples()
        for row in rows:
            row["valid_states"] = "1"
        self.assertEqual(self.review(rows)["status"], "FAIL")

    def test_stopped_uart_and_unfinished_heartbeat_gap(self):
        for field, value in (("uart_task", "0"), ("heartbeat_tx_age_ms", "5001"),
                             ("state_age_ms", "10001")):
            with self.subTest(field=field):
                rows = samples()
                rows[-1][field] = value
                self.assertEqual(self.review(rows)["status"], "FAIL")

    def test_restart_with_uptime_still_greater_than_previous_sample(self):
        rows = samples()
        for row in rows[1:]:
            row["uptime_seconds"] = str(int(row["uptime_seconds"]) - 30)
        report = self.review(rows)
        self.assertEqual(report["resets"], 1)
        self.assertEqual(report["status"], "FAIL")

    def test_counter_drop_detects_restart(self):
        rows = samples()
        rows[-1]["valid_states"] = "1"
        self.assertEqual(self.review(rows)["resets"], 1)

    def test_missing_intervals_and_network_failures(self):
        rows = samples()
        del rows[1]
        self.assertEqual(self.review(rows)["status"], "FAIL")
        rows = samples()
        rows[1]["reachable"] = ""
        self.assertEqual(self.review(rows)["status"], "FAIL")

    def test_wifi_and_command_failure_deltas_cannot_cancel(self):
        rows = samples()
        rows[1]["wifi_disconnects"] = "1"
        rows[1]["control_applied_failed"] = "0 / 1"
        report = self.review(rows)
        self.assertEqual(report["wifi_disconnects"], 1)
        self.assertEqual(report["control_failures"], 1)
        self.assertEqual(report["status"], "FAIL")

    def test_bad_timestamps_and_missing_metrics(self):
        for field, value in (("timestamp_utc", samples()[0]["timestamp_utc"]),
                             ("minimum_free_heap_bytes", "")):
            with self.subTest(field=field):
                rows = samples()
                rows[1][field] = value
                with self.assertRaises(ValueError):
                    self.review(rows)

    def test_uart_stack_and_stall_limit(self):
        for field, value in (("uart_stack_free_bytes", "300"),
                             ("uart_stall_restarts", "3 / limit reached")):
            with self.subTest(field=field):
                rows = samples()
                rows[-1][field] = value
                self.assertEqual(self.review(rows)["status"], "FAIL")

    def test_csv_from_older_monitor_without_new_columns(self):
        new_fields = {"uart_stack_free_bytes", "uart_stall_restarts", "control_unchanged",
                      "pending_state_replies", "control_available", "firmware_version"}
        fields = [field for field in monitor.FIELDS if field not in new_fields]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "legacy.csv"
            with path.open("w", newline="") as output:
                writer = csv.DictWriter(output, fieldnames=fields, extrasaction="ignore")
                writer.writeheader()
                writer.writerows(samples())
            report = soak.review(path, 1, 60)
        self.assertEqual(report["status"], "PASS")
        self.assertIsNone(report["minimum_uart_stack_free_bytes"])

    def test_invalid_duration(self):
        for value in (float("nan"), float("inf"), 0, -1):
            with self.assertRaises(ValueError):
                soak.review(Path("unused.csv"), value, 60)

    def test_html_whitelist(self):
        class Response:
            def __enter__(self):
                return self

            def __exit__(self, *args):
                pass

            def read(self, limit):
                return (b"<tr><td>UART task:</td><td>1</td></tr>"
                        b"<tr><td>Valid states:</td><td>42</td></tr>"
                        b"<tr><td>Firmware version:</td><td>0.2.0-gm1-diag</td></tr>"
                        b"<tr><td>Control available:</td><td>1</td></tr>"
                        b"<tr><td>Pending state replies:</td><td>2</td></tr>"
                        b"<tr><td>Private network:</td><td>do-not-record</td></tr>")

        with patch.object(monitor, "urlopen", return_value=Response()):
            row = monitor.sample("http://example.invalid/tion-status")
        self.assertEqual(row["reachable"], "1")
        self.assertEqual(row["valid_states"], "42")
        self.assertEqual(row["firmware_version"], "0.2.0-gm1-diag")
        self.assertEqual(row["control_available"], "1")
        self.assertEqual(row["pending_state_replies"], "2")
        self.assertNotIn("do-not-record", row.values())

    def test_failed_request_records_only_exception_type(self):
        with patch.object(monitor, "urlopen", side_effect=OSError("private-host")):
            row = monitor.sample("http://example.invalid/tion-status")
        self.assertEqual(row["error"], "OSError")
        self.assertNotIn("private-host", str(row))


if __name__ == "__main__":
    unittest.main()
