#!/usr/bin/env python3

import json
import sys
from collections import Counter
from pathlib import Path

SUPPORTED_LOG_VERSIONS = {9}


def load_rows(path):
    rows = []
    skipped = Counter()
    parse_errors = 0
    for line_no, line in enumerate(Path(path).read_text().splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        try:
            row = json.loads(line)
        except json.JSONDecodeError:
            parse_errors += 1
            continue

        event = row.get("event")
        if event != "debug_sample":
            skipped[event or "missing_event"] += 1
            continue
        if row.get("version") not in SUPPORTED_LOG_VERSIONS:
            skipped[f"version_{row.get('version')}"] += 1
            continue
        rows.append(row)
    return rows, skipped, parse_errors


def screen(value):
    return "unknown" if value is None else str(value)


def main():
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} LOG_JSONL", file=sys.stderr)
        return 2

    path = Path(sys.argv[1])
    if not path.exists():
        print(f"{path}: not found", file=sys.stderr)
        return 1

    rows, skipped, parse_errors = load_rows(path)
    print(f"analysis_rows={len(rows)} skipped_rows={sum(skipped.values())} parse_errors={parse_errors}")
    if skipped:
        print("skipped_events: " + " ".join(f"{key}={value}" for key, value in sorted(skipped.items())))
    if not rows:
        return 0

    edge_states = Counter(row.get("edge_push", {}).get("state_name", "missing") for row in rows)
    print("edge_push_states: " + " ".join(f"{key}={value}" for key, value in sorted(edge_states.items())))

    latest = rows[-1]
    totals = latest.get("totals", {})
    queue = latest.get("queue", {})
    diagnostics = latest.get("diagnostics", {})
    print(f"movement_sent={queue.get('movement_sent')}")
    print(f"queue_depth_latest={queue.get('depth')}")
    print(f"transmit_failures={totals.get('transmit_failures')}")
    print(f"screen_changes={totals.get('screen_changes')}")
    print(
        "host_reports="
        f"{diagnostics.get('host_reports_accepted')} accepted / "
        f"{diagnostics.get('host_reports_ignored')} ignored "
        f"last={diagnostics.get('last_ignore_reason')}"
    )

    print("recent_edge_push:")
    for row in rows[-10:]:
        edge = row.get("edge_push", {})
        host = row.get("host", {})
        interval = row.get("interval", {})
        print(
            f"{row.get('timestamp')} "
            f"state={edge.get('state_name')} "
            f"src={screen(edge.get('source_screen'))} dst={screen(edge.get('target_screen'))} "
            f"axis={edge.get('axis')} dir={edge.get('direction')} "
            f"dist={edge.get('distance_to_edge')} margin={edge.get('margin')} gap={edge.get('gap_to_target')} "
            f"host_age_ms={edge.get('host_age_ms')} "
            f"host_delta=({host.get('delta_x')},{host.get('delta_y')}) "
            f"sent=({interval.get('sent_dx')},{interval.get('sent_dy')}) "
            f"screen_changes={interval.get('screen_changes')}"
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
