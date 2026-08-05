#!/usr/bin/env python3

"""Render MAR_TIMELINE server log records as a standalone SVG."""

import argparse
import html
import sys
from pathlib import Path


REQUIRED_FIELDS = (
    "job",
    "queue",
    "enqueued_us",
    "submitted_us",
    "completed_us",
    "returned_us",
    "status",
)
TIME_FIELDS = ("enqueued_us", "submitted_us", "completed_us", "returned_us")


def warn(message):
    print(f"warning: {message}", file=sys.stderr)


def parse_records(log_path):
    records = []
    with log_path.open(encoding="utf-8", errors="replace") as log_file:
        for line_number, raw_line in enumerate(log_file, start=1):
            line = raw_line.strip()
            if not line.startswith("MAR_TIMELINE"):
                continue

            fields = {}
            malformed = False
            for token in line.split()[1:]:
                if "=" not in token:
                    warn(f"{log_path}:{line_number}: malformed token {token!r}; skipping record")
                    malformed = True
                    break
                key, value = token.split("=", 1)
                fields[key] = value
            if malformed:
                continue

            missing = [field for field in REQUIRED_FIELDS if field not in fields]
            if missing:
                warn(
                    f"{log_path}:{line_number}: missing {', '.join(missing)}; "
                    "skipping record"
                )
                continue
            if fields["status"] != "completed":
                warn(f"{log_path}:{line_number}: status is not completed; skipping record")
                continue

            try:
                record = {
                    "job": int(fields["job"]),
                    "queue": int(fields["queue"]),
                    **{field: int(fields[field]) for field in TIME_FIELDS},
                }
            except ValueError:
                warn(f"{log_path}:{line_number}: non-integer ID or timestamp; skipping record")
                continue

            times = [record[field] for field in TIME_FIELDS]
            if times != sorted(times):
                warn(f"{log_path}:{line_number}: timestamps are out of order; skipping record")
                continue
            records.append(record)

    return sorted(records, key=lambda record: record["enqueued_us"])


def svg_text(x, y, text, *, size=14, weight="normal", fill="#172033", anchor="start"):
    return (
        f'<text x="{x}" y="{y}" font-family="-apple-system, BlinkMacSystemFont, '
        f'Segoe UI, sans-serif" font-size="{size}" font-weight="{weight}" '
        f'fill="{fill}" text-anchor="{anchor}">{html.escape(str(text))}</text>'
    )


def render_svg(records, output_path):
    left = 180
    right = 45
    top = 132
    row_height = 32
    bottom = 72
    width = 1200
    plot_width = width - left - right
    height = top + len(records) * row_height + bottom

    origin_us = min(record["enqueued_us"] for record in records)
    duration_us = max(record["returned_us"] for record in records) - origin_us
    duration_us = max(duration_us, 1)

    def x_position(timestamp_us):
        return left + ((timestamp_us - origin_us) / duration_us) * plot_width

    parts = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}" role="img" '
        'aria-labelledby="title description">',
        '<title id="title">Metal API Remoter scheduler timeline</title>',
        '<desc id="description">Waiting, Metal in-flight, and result-copyback intervals '
        'for remotely submitted Metal command buffers.</desc>',
        f'<rect width="{width}" height="{height}" fill="#ffffff"/>',
        svg_text(28, 36, "Metal API Remoter scheduler timeline", size=24, weight="700"),
        svg_text(
            28,
            62,
            "Lifecycle timestamps observed by MAR and Metal; not exact GPU hardware occupancy.",
            size=13,
            fill="#526078",
        ),
    ]

    legend = (
        ("#a8b0bd", "Waiting in MAR"),
        ("#3977d5", "Metal in-flight"),
        ("#e69a35", "Result copyback"),
    )
    legend_x = 28
    for color, label in legend:
        parts.append(f'<rect x="{legend_x}" y="82" width="20" height="11" rx="2" fill="{color}"/>')
        parts.append(svg_text(legend_x + 27, 92, label, size=12, fill="#354158"))
        legend_x += 150
    parts.append('<circle cx="493" cy="87.5" r="5" fill="#2d9b5f"/>')
    parts.append(svg_text(505, 92, "Client result ready", size=12, fill="#354158"))

    tick_count = 5
    for tick in range(tick_count + 1):
        fraction = tick / tick_count
        x = left + fraction * plot_width
        elapsed_ms = duration_us * fraction / 1000.0
        parts.append(
            f'<line x1="{x:.2f}" y1="{top - 12}" x2="{x:.2f}" '
            f'y2="{height - bottom + 8}" stroke="#e3e7ed" stroke-width="1"/>'
        )
        parts.append(svg_text(x, height - 34, f"{elapsed_ms:.1f} ms", size=11, fill="#667085", anchor="middle"))

    for row, record in enumerate(records):
        center_y = top + row * row_height + row_height / 2
        bar_y = center_y - 7
        enqueued_x = x_position(record["enqueued_us"])
        submitted_x = x_position(record["submitted_us"])
        completed_x = x_position(record["completed_us"])
        returned_x = x_position(record["returned_us"])

        if row % 2:
            parts.append(
                f'<rect x="16" y="{top + row * row_height}" width="{width - 32}" '
                f'height="{row_height}" fill="#f8fafc"/>'
            )
        parts.append(
            svg_text(
                left - 14,
                center_y + 5,
                f'Job {record["job"]} · Queue {record["queue"]}',
                size=12,
                fill="#354158",
                anchor="end",
            )
        )

        intervals = (
            (enqueued_x, submitted_x, "#a8b0bd"),
            (submitted_x, completed_x, "#3977d5"),
            (completed_x, returned_x, "#e69a35"),
        )
        for start_x, end_x, color in intervals:
            parts.append(
                f'<rect x="{start_x:.2f}" y="{bar_y:.2f}" '
                f'width="{max(end_x - start_x, 1.0):.2f}" height="14" rx="2" fill="{color}"/>'
            )
        parts.append(f'<circle cx="{returned_x:.2f}" cy="{center_y:.2f}" r="5" fill="#2d9b5f"/>')

    parts.append(
        f'<line x1="{left}" y1="{height - bottom + 8}" x2="{width - right}" '
        f'y2="{height - bottom + 8}" stroke="#8993a4" stroke-width="1"/>'
    )
    parts.append(svg_text(left + plot_width / 2, height - 10, "Elapsed time from first enqueue", size=12, fill="#526078", anchor="middle"))
    parts.append("</svg>")
    output_path.write_text("\n".join(parts) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("server_log", type=Path, help="server log containing MAR_TIMELINE records")
    parser.add_argument("output_svg", type=Path, help="path for the generated SVG")
    parser.add_argument("--limit", type=int, default=30, help="maximum jobs to render (default: 30)")
    args = parser.parse_args()

    if args.limit <= 0:
        parser.error("--limit must be positive")

    try:
        records = parse_records(args.server_log)
    except OSError as error:
        parser.error(str(error))
    if not records:
        parser.error(f"no complete MAR_TIMELINE records found in {args.server_log}")

    selected_records = records[: args.limit]
    args.output_svg.parent.mkdir(parents=True, exist_ok=True)
    try:
        render_svg(selected_records, args.output_svg)
    except OSError as error:
        parser.error(str(error))

    print(
        f"Rendered {len(selected_records)} of {len(records)} completed jobs to {args.output_svg}"
    )


if __name__ == "__main__":
    main()
