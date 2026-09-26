#!/usr/bin/env python3
"""Review private diagnostic CSVs against the Tion release-candidate soak gate."""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import json
import math
from pathlib import Path


CONNECTED = "Device is Connected to HomeKit"
# Smallest UART task stack margin accepted during a soak run.
MIN_UART_STACK_FREE_BYTES = 512


def number(row: dict[str, str], field: str) -> int:
    value = int(row[field])
    if value < 0:
        raise ValueError(f"negative counter: {field}")
    return value


def optional_number(row: dict[str, str], field: str) -> int | None:
    """Fields added after rc2 are absent from older CSVs and empty before boot."""
    value = row.get(field) or ""
    return number(row, field) if value else None


def stall_limit_reached(row: dict[str, str]) -> bool:
    return "limit reached" in (row.get("uart_stall_restarts") or "")


def failure_count(row: dict[str, str]) -> int:
    parts = row["control_applied_failed"].split("/")
    if len(parts) != 2 or any(not part.strip().isdigit() for part in parts):
        raise ValueError("invalid control counters")
    return int(parts[1])


def review(path: Path, expected_hours: float, interval_seconds: float,
           expected_version: str | None = None) -> dict:
    if not all(math.isfinite(value) and value > 0
               for value in (expected_hours, interval_seconds)):
        raise ValueError("expected hours and interval must be finite and positive")
    with path.open(newline="") as source:
        rows = list(csv.DictReader(source))
    if not rows:
        raise ValueError(f"{path}: no samples")

    timestamps = [datetime.fromisoformat(row["timestamp_utc"]) for row in rows]
    if any(value.tzinfo is None for value in timestamps):
        raise ValueError("timestamps must include a UTC offset")
    timestamps = [value.astimezone(timezone.utc) for value in timestamps]
    if any(current <= previous for previous, current in zip(timestamps, timestamps[1:])):
        raise ValueError("timestamps must increase strictly")
    if any(row["reachable"] not in ("", "0", "1") for row in rows):
        raise ValueError("invalid reachable flag")
    first_time, last_time = timestamps[0], timestamps[-1]
    duration_hours = (last_time - first_time).total_seconds() / 3600
    reachable = [row for row in rows if row["reachable"] == "1"]
    if not reachable:
        raise ValueError("no reachable samples")

    resets = 0
    stalled_uart_samples = 0
    wifi_disconnects = 0
    control_failures = 0
    counter_fields = ("uptime_seconds", "valid_states", "heartbeat_responses",
                      "wifi_disconnects", "rejected_frames", "uart_tx_failures")
    for row in reachable:
        for field in counter_fields:
            number(row, field)
        failure_count(row)
    for previous, current in zip(reachable, reachable[1:]):
        elapsed = (datetime.fromisoformat(current["timestamp_utc"]) -
                   datetime.fromisoformat(previous["timestamp_utc"])).total_seconds()
        uptime_delta = number(current, "uptime_seconds") - number(previous, "uptime_seconds")
        reset = (any(number(current, field) < number(previous, field)
                     for field in counter_fields) or
                 current["reset_reason"] != previous["reset_reason"] or
                 failure_count(current) < failure_count(previous) or
                 elapsed - uptime_delta > 15)
        resets += reset
        if not reset and elapsed >= 10 and (
                number(current, "valid_states") == number(previous, "valid_states") or
                number(current, "heartbeat_responses") == number(previous, "heartbeat_responses")):
            stalled_uart_samples += 1
        wifi_disconnects += max(0, number(current, "wifi_disconnects") -
                                number(previous, "wifi_disconnects"))
        control_failures += max(0, failure_count(current) - failure_count(previous))

    missed_intervals = sum(
        max(0, round((datetime.fromisoformat(current["timestamp_utc"]) -
                      datetime.fromisoformat(previous["timestamp_utc"])).total_seconds()
                     / interval_seconds) - 1)
        for previous, current in zip(rows, rows[1:])
    )
    expected_samples = max(1, round((last_time - first_time).total_seconds()
                                    / interval_seconds) + 1)
    coverage = len(reachable) / max(expected_samples, len(rows))
    raw_errors = max(number(row, "raw_errors") for row in reachable)
    rejected_frames = max(number(row, "rejected_frames") for row in reachable)
    uart_tx_failures = max(number(row, "uart_tx_failures") for row in reachable)
    max_heartbeat_gap_ms = max(number(row, "max_heartbeat_tx_gap_ms")
                               for row in reachable)
    min_free_heap_bytes = min(number(row, "minimum_free_heap_bytes")
                              for row in reachable)
    stale_samples = sum(row["state_fresh"] != "1" or
                        number(row, "state_age_ms") > 10000 or
                        number(row, "valid_states") == 0 for row in reachable)
    uart_task_stopped_samples = sum(row["uart_task"] != "1" for row in reachable)
    max_heartbeat_age_ms = max(number(row, "heartbeat_tx_age_ms") for row in reachable)
    homekit_disconnected_samples = sum(row["homekit_status"] != CONNECTED
                                      for row in reachable)
    stack_samples = [value for value in (optional_number(row, "uart_stack_free_bytes")
                                         for row in reachable) if value is not None]
    min_uart_stack_free_bytes = min(stack_samples) if stack_samples else None
    stall_limit_samples = sum(stall_limit_reached(row) for row in reachable)

    problems = []
    if expected_version is not None and any(
            row.get("firmware_version") != expected_version for row in reachable):
        problems.append("unexpected or missing firmware version")
    if resets:
        problems.append("ESP restarted")
    if coverage < 0.99:
        problems.append("diagnostic coverage below 99%")
    if missed_intervals:
        problems.append("host missed sampling intervals")
    if stale_samples:
        problems.append("UART state became stale")
    if uart_task_stopped_samples or stalled_uart_samples:
        problems.append("UART task or response counters stopped")
    if homekit_disconnected_samples:
        problems.append("HomeKit disconnected")
    if wifi_disconnects:
        problems.append("Wi-Fi disconnected")
    if max_heartbeat_age_ms >= 5000:
        problems.append("heartbeat TX age reached 5 seconds")
    if max_heartbeat_gap_ms >= 5000:
        problems.append("heartbeat TX gap reached 5 seconds")
    if raw_errors or rejected_frames or uart_tx_failures:
        problems.append("Tion or UART errors observed")
    if control_failures:
        problems.append("control transaction failed")
    if min_free_heap_bytes < 100_000:
        problems.append("minimum free heap below 100 KB")
    if (min_uart_stack_free_bytes is not None and
            min_uart_stack_free_bytes < MIN_UART_STACK_FREE_BYTES):
        problems.append("UART task stack margin below 512 bytes")
    if stall_limit_samples:
        problems.append("UART stall restart limit reached")

    complete = len(rows) >= 2 and duration_hours >= expected_hours - 2 * interval_seconds / 3600
    status = "FAIL" if problems else "PASS" if complete else "IN_PROGRESS"
    return {
        "file": str(path),
        "status": status,
        "duration_hours": round(duration_hours, 3),
        "samples": len(rows),
        "reachable_samples": len(reachable),
        "coverage": round(coverage, 5),
        "missed_intervals": missed_intervals,
        "resets": resets,
        "stale_samples": stale_samples,
        "uart_task_stopped_samples": uart_task_stopped_samples,
        "stalled_uart_samples": stalled_uart_samples,
        "max_heartbeat_age_ms": max_heartbeat_age_ms,
        "homekit_disconnected_samples": homekit_disconnected_samples,
        "wifi_disconnects": wifi_disconnects,
        "max_heartbeat_gap_ms": max_heartbeat_gap_ms,
        "raw_errors": raw_errors,
        "rejected_frames": rejected_frames,
        "uart_tx_failures": uart_tx_failures,
        "control_failures": control_failures,
        "minimum_free_heap_bytes": min_free_heap_bytes,
        "minimum_uart_stack_free_bytes": min_uart_stack_free_bytes,
        "stall_limit_samples": stall_limit_samples,
        "problems": problems,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", type=Path, nargs="+")
    parser.add_argument("--expected-hours", type=float, default=72)
    parser.add_argument("--interval", type=float, default=60)
    parser.add_argument("--expected-version", help="Require this firmware in every sample")
    args = parser.parse_args()
    if not all(math.isfinite(value) and value > 0
               for value in (args.expected_hours, args.interval)):
        parser.error("expected hours and interval must be finite and positive")
    reports = []
    for path in args.files:
        try:
            reports.append(review(path, args.expected_hours, args.interval, args.expected_version))
        except (OSError, ValueError, KeyError, TypeError, csv.Error) as exc:
            reports.append({"file": str(path), "status": "FAIL",
                            "problems": [f"invalid or unreadable CSV: {type(exc).__name__}"]})
    print(json.dumps(reports, ensure_ascii=False, indent=2))
    if any(report["status"] == "FAIL" for report in reports):
        raise SystemExit(1)
    if any(report["status"] == "IN_PROGRESS" for report in reports):
        raise SystemExit(2)


if __name__ == "__main__":
    main()
