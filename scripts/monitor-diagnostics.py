#!/usr/bin/env python3
"""Record a bounded, read-only HomeSpan diagnostic run as CSV."""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
from html.parser import HTMLParser
import math
import os
from pathlib import Path
import time
from urllib.request import urlopen


FIELDS = (
    "timestamp_utc",
    "firmware_version",
    "reachable",
    "error",
    "reset_reason",
    "wifi_disconnects",
    "homekit_status",
    "uart_task",
    "first_heartbeat_ms",
    "heartbeat_responses",
    "heartbeat_tx_age_ms",
    "max_heartbeat_tx_gap_ms",
    "valid_states",
    "pending_state_replies",
    "state_age_ms",
    "state_fresh",
    "rejected_frames",
    "uart_tx_failures",
    "uart_stack_free_bytes",
    "uart_stall_restarts",
    "control_available",
    "control_armed",
    "control_phase_failure",
    "control_refusal",
    "control_applied_failed",
    "control_unchanged",
    "device_type",
    "device_work_mode",
    "tion_firmware",
    "tion_hardware",
    "raw_request_id",
    "power",
    "fan_speed_maximum",
    "sound_enabled",
    "led_enabled",
    "gate_position",
    "filter_warning",
    "heater_mode_raw_bit",
    "raw_heater_percentage",
    "target_temperature_c",
    "outdoor_temperature_c",
    "raw_current_temperature_c",
    "raw_filter_counter",
    "raw_errors",
    "uptime_seconds",
    "free_heap_bytes",
    "minimum_free_heap_bytes",
    "largest_free_block_bytes",
)

LABELS = {
    "Firmware version": "firmware_version",
    "Reset Reason": "reset_reason",
    "WiFi Disconnects": "wifi_disconnects",
    "HomeKit Status": "homekit_status",
    "UART task": "uart_task",
    "First heartbeat (ms)": "first_heartbeat_ms",
    "Heartbeat responses": "heartbeat_responses",
    "Heartbeat TX age (ms)": "heartbeat_tx_age_ms",
    "Maximum heartbeat TX gap (ms)": "max_heartbeat_tx_gap_ms",
    "Valid states": "valid_states",
    "Pending state replies": "pending_state_replies",
    "State age (ms)": "state_age_ms",
    "State fresh": "state_fresh",
    "Rejected frames": "rejected_frames",
    "UART TX failures": "uart_tx_failures",
    "UART stack free (bytes)": "uart_stack_free_bytes",
    "UART stall restarts": "uart_stall_restarts",
    "Control available": "control_available",
    "Control armed": "control_armed",
    "Control phase / failure": "control_phase_failure",
    "Control refusal": "control_refusal",
    "Control applied / failed": "control_applied_failed",
    "Control unchanged": "control_unchanged",
    "Device type": "device_type",
    "Device work mode": "device_work_mode",
    "Tion firmware": "tion_firmware",
    "Tion hardware": "tion_hardware",
    "Raw request ID": "raw_request_id",
    "Power": "power",
    "Fan speed / maximum": "fan_speed_maximum",
    "Sound enabled": "sound_enabled",
    "LED enabled": "led_enabled",
    "Gate position (0=outdoor, 1=recirculation)": "gate_position",
    "Filter warning": "filter_warning",
    "Heater mode allows / raw bit": "heater_mode_raw_bit",
    "Heater allowed / active": "heater_mode_raw_bit",  # Legacy diagnostic label.
    "Raw heater percentage": "raw_heater_percentage",
    "Target temperature (C)": "target_temperature_c",
    "Outdoor temperature (C)": "outdoor_temperature_c",
    "Raw current temperature (C)": "raw_current_temperature_c",
    "Raw filter counter": "raw_filter_counter",
    "Raw errors": "raw_errors",
    "Uptime (s)": "uptime_seconds",
    "Free heap (bytes)": "free_heap_bytes",
    "Minimum free heap (bytes)": "minimum_free_heap_bytes",
    "Largest free block (bytes)": "largest_free_block_bytes",
}


class TableRows(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.rows: list[list[str]] = []
        self._row: list[str] | None = None
        self._cell: list[str] | None = None

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if tag == "tr":
            self._row = []
        elif tag in ("td", "th") and self._row is not None:
            self._cell = []

    def handle_data(self, data: str) -> None:
        if self._cell is not None:
            self._cell.append(data)

    def handle_endtag(self, tag: str) -> None:
        if tag in ("td", "th") and self._row is not None and self._cell is not None:
            self._row.append(" ".join("".join(self._cell).split()))
            self._cell = None
        elif tag == "tr" and self._row is not None:
            self.rows.append(self._row)
            self._row = None


def sample(url: str) -> dict[str, str]:
    result = {field: "" for field in FIELDS}
    result["timestamp_utc"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
    try:
        with urlopen(url, timeout=12) as response:
            document = response.read(256_000).decode("utf-8", errors="replace")
        parser = TableRows()
        parser.feed(document)
        for row in parser.rows:
            if len(row) < 2:
                continue
            field = LABELS.get(row[0].rstrip(":"))
            if field is not None:
                result[field] = row[1]
        if result["uart_task"] == "" or result["valid_states"] == "":
            result["error"] = "missing_diagnostic_rows"
        else:
            result["reachable"] = "1"
    except Exception as exc:  # Record failures without logging hostnames or secrets.
        result["error"] = type(exc).__name__
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True, help="Local /tion-status URL")
    parser.add_argument("--output", type=Path, required=True, help="Private CSV path")
    parser.add_argument("--hours", type=float, default=8.0)
    parser.add_argument("--interval", type=float, default=60.0)
    args = parser.parse_args()
    if not all(math.isfinite(value) and value > 0
               for value in (args.hours, args.interval)):
        parser.error("hours and interval must be finite and positive")
    if args.output.exists():
        parser.error("output already exists; choose a new file")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + args.hours * 3600
    next_sample = time.monotonic()
    descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=FIELDS)
        writer.writeheader()
        while time.monotonic() < deadline:
            writer.writerow(sample(args.url))
            output.flush()
            next_sample = max(next_sample + args.interval, time.monotonic())
            time.sleep(max(0, min(next_sample - time.monotonic(), deadline - time.monotonic())))


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
