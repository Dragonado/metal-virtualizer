#!/usr/bin/env python3

"""Render MAR_TIMELINE server log records as a standalone SVG.

The chart has two panels:

  Per-job lifecycle  Each job normalized to its own enqueue, so the wait /
                     GPU-computing / copyback phases are directly comparable
                     across jobs regardless of when the job arrived.
  Concurrency        The same jobs in absolute wall-clock time, packed into
                     lanes, so overlap and queueing depth stay visible.

Absolute time alone cannot carry the first panel: a job lives for tens of
milliseconds inside a run that spans seconds, so every phase collapses to
sub-pixel width. Normalizing per job is what makes the phases readable.
"""

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

PHASES = (
    ("wait", "enqueued_us", "submitted_us", "Waiting in MAR"),
    ("inflight", "submitted_us", "completed_us", "GPU computing"),
    ("copyback", "completed_us", "returned_us", "Result copyback"),
)

# Light and dark are separately chosen steps, not an automatic flip.
# The two categorical hues pass every gate of the palette validator in both
# modes; "wait" is a deliberate de-emphasis neutral for idle time, so it sits
# below the chroma floor on purpose while still clearing CVD separation and
# 3:1 contrast against its surface.
THEMES = {
    "light": {
        "surface": "#fcfcfb",
        "text_primary": "#0b0b0b",
        "text_secondary": "#52514e",
        "muted": "#898781",
        "grid": "#e1e0d9",
        "axis": "#c3c2b7",
        "wait": "#898781",
        "inflight": "#2a78d6",
        "copyback": "#eb6834",
    },
    "dark": {
        "surface": "#1a1a19",
        "text_primary": "#ffffff",
        "text_secondary": "#c3c2b7",
        "muted": "#898781",
        "grid": "#2c2c2a",
        "axis": "#383835",
        "wait": "#898781",
        "inflight": "#3987e5",
        "copyback": "#d95926",
    },
}

FONT = 'system-ui, -apple-system, "Segoe UI", sans-serif'

# Rough advance widths for the system sans, as a fraction of font size. Only
# used to lay out the legend and to decide whether a label fits; a few percent
# of error is harmless at these sizes.
_NARROW = set(" .,:;'|!ilj()[]/\\ft")
_WIDE = set("mwMW@")


def text_width(text, size):
    total = 0.0
    for char in str(text):
        if char in _NARROW:
            total += 0.30
        elif char in _WIDE:
            total += 0.85
        elif char.isupper() or char.isdigit():
            total += 0.60
        else:
            total += 0.52
    return total * size


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
                    "seq": int(fields["seq"]) if "seq" in fields else None,
                    **{field: int(fields[field]) for field in TIME_FIELDS},
                }
            except ValueError:
                warn(f"{log_path}:{line_number}: non-integer ID or timestamp; skipping record")
                continue

            times = [record[field] for field in TIME_FIELDS]
            if times != sorted(times):
                warn(f"{log_path}:{line_number}: timestamps are out of order; skipping record")
                continue

            record["lifetime_us"] = record["returned_us"] - record["enqueued_us"]
            records.append(record)

    return sorted(records, key=lambda record: record["enqueued_us"])


def select_window(records, gap_us, start_us, window_us, use_all):
    """Pick a contiguous slice of the run.

    A log usually holds several client batches separated by idle seconds. Taking
    the first N records by enqueue time straddles those gaps, which stretches the
    absolute-time axis across dead air. Default to the densest burst instead.
    """
    if use_all:
        return records, "entire log"

    if start_us is not None or window_us is not None:
        origin = records[0]["enqueued_us"]
        lower = origin + (start_us or 0)
        upper = lower + window_us if window_us is not None else records[-1]["enqueued_us"]
        selected = [r for r in records if lower <= r["enqueued_us"] <= upper]
        return selected, "explicit window"

    bursts = [[records[0]]]
    for record in records[1:]:
        if record["enqueued_us"] - bursts[-1][-1]["enqueued_us"] > gap_us:
            bursts.append([record])
        else:
            bursts[-1].append(record)

    densest = max(bursts, key=len)
    if len(bursts) > 1:
        label = f"densest of {len(bursts)} bursts"
    else:
        label = "single burst"
    return densest, label


def number_window(window_records):
    """Give every job in the window a readable 1..N label.

    The server's `seq` field is admission order across the whole process. When
    it is present the numbers stay greppable against the log; older logs have
    no `seq`, so fall back to position within the window.
    """
    for index, record in enumerate(sorted(window_records, key=lambda r: r["enqueued_us"]), start=1):
        record["number"] = record["seq"] if record["seq"] is not None else index


def nice_step(rough):
    """Round a rough tick step up to 1, 2, 2.5 or 5 times a power of ten."""
    if rough <= 0:
        return 1.0
    exponent = 10 ** (len(f"{int(rough)}") - 1) if rough >= 1 else 1.0
    while exponent > rough:
        exponent /= 10.0
    while exponent * 10 <= rough:
        exponent *= 10.0
    for multiple in (1.0, 2.0, 2.5, 5.0, 10.0):
        if exponent * multiple >= rough:
            return exponent * multiple
    return exponent * 10.0


def format_ms(value):
    """Three significant figures, which keeps 0.387 and 984 both readable."""
    if value >= 100:
        return f"{value:,.0f}"
    if value >= 10:
        return f"{value:,.1f}"
    if value >= 1:
        return f"{value:,.2f}"
    return f"{value:,.3f}"


def format_tick(value, step):
    """Axis ticks carry exactly as many decimals as the step needs."""
    for decimals in range(4):
        if abs(step * 10**decimals - round(step * 10**decimals)) < 1e-9:
            return f"{value:,.{decimals}f}"
    return f"{value:,.4f}"


def axis_bounds(target_ms):
    """A tick step and axis maximum that cover target_ms without much slack."""
    target_ms = max(target_ms, 1e-6)
    step = nice_step(target_ms / 8.0)
    return step, step * (int(target_ms / step) + 1)


def esc(text):
    return html.escape(str(text))


class Canvas:
    def __init__(self, theme):
        self.parts = []
        self.theme = theme

    def add(self, markup):
        self.parts.append(markup)

    def text(self, x, y, content, *, size=12, weight="400", fill=None, anchor="start", tabular=False):
        fill = fill or self.theme["text_primary"]
        numeric = ' font-variant-numeric="tabular-nums"' if tabular else ""
        self.add(
            f'<text x="{x:.1f}" y="{y:.1f}" font-family=\'{FONT}\' font-size="{size}" '
            f'font-weight="{weight}" fill="{fill}" text-anchor="{anchor}"{numeric}>'
            f"{esc(content)}</text>"
        )

    def line(self, x1, y1, x2, y2, stroke, width=1):
        self.add(
            f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" '
            f'stroke="{stroke}" stroke-width="{width}"/>'
        )

    def bar(self, x, y, width, height, fill, *, radius=0.0, tooltip=None):
        """A rect whose right (data) end may be rounded while the left stays square."""
        width = max(width, 0.0)
        radius = min(radius, width, height / 2.0)
        if radius > 0.5:
            path = (
                f"M{x:.2f},{y:.2f} H{x + width - radius:.2f} "
                f"Q{x + width:.2f},{y:.2f} {x + width:.2f},{y + radius:.2f} "
                f"V{y + height - radius:.2f} "
                f"Q{x + width:.2f},{y + height:.2f} {x + width - radius:.2f},{y + height:.2f} "
                f"H{x:.2f} Z"
            )
            shape = f'<path d="{path}" fill="{fill}"/>'
        else:
            shape = (
                f'<rect x="{x:.2f}" y="{y:.2f}" width="{width:.2f}" '
                f'height="{height:.2f}" fill="{fill}"/>'
            )
        if tooltip:
            shape = f"<g><title>{esc(tooltip)}</title>{shape}</g>"
        self.add(shape)

    def svg(self, width, height, title, description):
        header = [
            '<?xml version="1.0" encoding="UTF-8"?>',
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height:.0f}" '
            f'viewBox="0 0 {width} {height:.0f}" role="img" aria-labelledby="title description">',
            f'<title id="title">{esc(title)}</title>',
            f'<desc id="description">{esc(description)}</desc>',
            f'<rect width="{width}" height="{height:.0f}" fill="{self.theme["surface"]}"/>',
        ]
        return "\n".join(header + self.parts + ["</svg>"]) + "\n"


def phase_durations_ms(record):
    return [
        (name, (record[end] - record[start]) / 1000.0, label)
        for name, start, end, label in PHASES
    ]


def draw_lifecycle_rows(canvas, theme, rows, axis_max_ms, geometry):
    """Panel A: every job normalized to its own enqueue, on one shared axis."""
    left, right, top, row_height, bar_height = geometry
    plot_width = right - left

    def x_of(ms):
        return left + (min(ms, axis_max_ms) / axis_max_ms) * plot_width

    step, _ = axis_bounds(axis_max_ms)

    plot_bottom = top + len(rows) * row_height

    tick = 0.0
    while tick <= axis_max_ms + step / 2:
        x = x_of(tick)
        canvas.line(x, top - 8, x, plot_bottom + 4, theme["grid"])
        canvas.text(
            x, plot_bottom + 24, format_tick(tick, step), size=11,
            fill=theme["muted"], anchor="middle", tabular=True,
        )
        tick += step

    for index, record in enumerate(rows):
        center_y = top + index * row_height + row_height / 2.0
        bar_y = center_y - bar_height / 2.0
        canvas.text(
            left - 12, center_y + 4, f'Job {record["number"]}',
            size=11, fill=theme["text_secondary"], anchor="end",
        )

        lifetime_ms = record["lifetime_us"] / 1000.0
        clipped = lifetime_ms > axis_max_ms
        phases = phase_durations_ms(record)
        last_drawn = max(
            (i for i, (_, duration, _) in enumerate(phases) if duration > 0),
            default=len(phases) - 1,
        )
        cursor_ms = 0.0
        for position, (name, duration_ms, label) in enumerate(phases):
            start_x = x_of(cursor_ms)
            end_x = x_of(cursor_ms + duration_ms)
            cursor_ms += duration_ms
            if duration_ms <= 0 or end_x <= start_x:
                continue

            width = end_x - start_x
            # 2px of surface separates touching segments; only take the gap out
            # of segments wide enough to spare it, so positions stay truthful.
            if position < last_drawn and width > 4.0:
                width -= 2.0
            square_end = clipped and end_x >= right - 0.01
            canvas.bar(
                start_x, bar_y, max(width, 0.7), bar_height, theme[name],
                radius=0.0 if square_end else (4.0 if position == last_drawn else 0.0),
                tooltip=(
                    f'Job {record["number"]} · {label}: {format_ms(duration_ms)} ms'
                    f' (command buffer {record["job"]}, queue {record["queue"]})'
                ),
            )

        if clipped:
            # The bar runs past the axis. Say so with a chevron rather than
            # letting a flat end imply the job ended at the axis maximum.
            colour = theme[phases[last_drawn][0]]
            canvas.add(
                f'<path d="M{right + 4:.1f},{bar_y:.1f} L{right + 11:.1f},'
                f'{center_y:.1f} L{right + 4:.1f},{bar_y + bar_height:.1f} Z" fill="{colour}"/>'
            )

        # The end label doubles as this chart's table view: a standalone SVG has
        # no tooltip fallback in every viewer, so no value is gated behind hover.
        canvas.text(
            (right + 17) if clipped else (x_of(lifetime_ms) + 10), center_y + 4,
            f"{format_ms(lifetime_ms)} ms",
            size=10.5,
            fill=theme["text_secondary"] if clipped else theme["muted"],
            tabular=True,
        )

    canvas.line(left, plot_bottom + 4, right, plot_bottom + 4, theme["axis"])
    canvas.text(
        left + plot_width / 2.0, plot_bottom + 46,
        "Milliseconds since that job's own enqueue",
        size=11, fill=theme["text_secondary"], anchor="middle",
    )
    return plot_bottom + 60


def draw_concurrency(canvas, theme, records, left, right, top):
    """Panel B: the same jobs in absolute time, lane-packed to show overlap."""
    plot_width = right - left
    origin = min(r["enqueued_us"] for r in records)
    span_ms = max((max(r["returned_us"] for r in records) - origin) / 1000.0, 0.001)

    step, axis_max_ms = axis_bounds(span_ms)

    def x_of(ms):
        return left + (ms / axis_max_ms) * plot_width

    lane_ends = []
    lanes = []
    for record in sorted(records, key=lambda r: r["enqueued_us"]):
        for index, end in enumerate(lane_ends):
            if record["enqueued_us"] >= end:
                lane_ends[index] = record["returned_us"]
                lanes.append(index)
                break
        else:
            lane_ends.append(record["returned_us"])
            lanes.append(len(lane_ends) - 1)

    lane_height = 9
    bar_height = 6
    plot_bottom = top + len(lane_ends) * lane_height

    tick = 0.0
    while tick <= axis_max_ms + step / 2:
        x = x_of(tick)
        canvas.line(x, top - 6, x, plot_bottom + 4, theme["grid"])
        canvas.text(
            x, plot_bottom + 22, format_tick(tick, step), size=11,
            fill=theme["muted"], anchor="middle", tabular=True,
        )
        tick += step

    for record, lane in zip(sorted(records, key=lambda r: r["enqueued_us"]), lanes):
        bar_y = top + lane * lane_height
        cursor_us = record["enqueued_us"]
        for name, start_field, end_field, label in PHASES:
            duration_us = record[end_field] - record[start_field]
            start_x = x_of((cursor_us - origin) / 1000.0)
            end_x = x_of((cursor_us + duration_us - origin) / 1000.0)
            cursor_us += duration_us
            if duration_us <= 0:
                continue
            canvas.bar(
                start_x, bar_y, max(end_x - start_x, 0.7), bar_height, theme[name],
                tooltip=f'Job {record["number"]} · {label}',
            )

    canvas.line(left, plot_bottom + 4, right, plot_bottom + 4, theme["axis"])
    canvas.text(
        left + plot_width / 2.0, plot_bottom + 44,
        "Elapsed milliseconds from the first enqueue in this window",
        size=11, fill=theme["text_secondary"], anchor="middle",
    )
    return plot_bottom + 58, len(lane_ends)


def draw_legend(canvas, theme, x, y, right_edge):
    entries = [(theme[name], label) for name, _, _, label in PHASES]
    size = 11.5
    widths = [16 + 6 + text_width(label, size) for _, label in entries]
    total = sum(widths) + 20 * (len(entries) - 1)
    cursor = max(x, right_edge - total)
    for (color, label), width in zip(entries, widths):
        canvas.add(
            f'<rect x="{cursor:.1f}" y="{y - 8:.1f}" width="16" height="9" rx="2" fill="{color}"/>'
        )
        canvas.text(cursor + 22, y, label, size=size, fill=theme["text_secondary"])
        cursor += width + 20


def percentile_ms(records, fraction):
    values = sorted(r["lifetime_us"] / 1000.0 for r in records)
    return values[min(int(fraction * len(values)), len(values) - 1)]


def render_svg(rows, window_records, all_records, output_path, theme_name, window_label, row_label):
    theme = THEMES[theme_name]
    canvas = Canvas(theme)

    # A heavy tail (one 984 ms job among 98) would flatten every other bar on a
    # full-range axis, so the axis covers p95 and longer bars are clipped and
    # marked. Their true value still sits in the end label.
    _, axis_max_ms = axis_bounds(percentile_ms(window_records, 0.95))
    clipped = sum(1 for r in rows if r["lifetime_us"] / 1000.0 > axis_max_ms)

    width = 1180
    margin = 32
    label_column = 132
    value_column = 78
    left = margin + label_column
    right = width - margin - value_column

    canvas.text(margin, 44, "Metal API Remoter scheduler timeline", size=23, weight="700")
    canvas.text(
        margin, 68,
        "Lifecycle timestamps observed by MAR and Metal; not exact GPU hardware occupancy.",
        size=13, fill=theme["text_secondary"],
    )

    canvas.line(margin, 92, width - margin, 92, theme["grid"])

    canvas.text(margin, 122, "Per-job lifecycle", size=14, weight="600")
    caption = f"{row_label} of {len(window_records)} jobs in this window, longest first."
    if clipped:
        plural = "bars run" if clipped > 1 else "bar runs"
        caption += f" {clipped} {plural} past the axis (▶); true value at right."
    canvas.text(margin, 140, caption, size=11, fill=theme["muted"])
    draw_legend(canvas, theme, margin, 122, width - margin)

    lifecycle_bottom = draw_lifecycle_rows(
        canvas, theme, rows, axis_max_ms, (left, right, 166, 30, 15)
    )

    canvas.line(margin, lifecycle_bottom, width - margin, lifecycle_bottom, theme["grid"])
    concurrency_top = lifecycle_bottom + 40
    canvas.text(margin, concurrency_top - 18, "How many jobs ran at the same time", size=14, weight="600")

    bottom, lane_count = draw_concurrency(
        canvas, theme, window_records, left, right, concurrency_top + 22
    )
    # Drawn after the plot so it can quote the stack depth the packing found.
    # Overlapping jobs cannot share a row, so the number of rows needed *is*
    # the number of jobs alive at once.
    canvas.text(
        margin, concurrency_top,
        f"All {len(window_records)} jobs at their real position in time. Overlapping jobs are "
        f"stacked, so the tallest stack is the busiest moment: {lane_count} jobs at once.",
        size=11, fill=theme["muted"],
    )
    canvas.text(
        left - 12, concurrency_top + 30,
        f"peak {lane_count}", size=11, fill=theme["text_secondary"], anchor="end",
    )

    canvas.text(
        margin, bottom + 6,
        f"{len(window_records)} of {len(all_records)} completed jobs in the log · "
        f"window selected as {window_label}.",
        size=11, fill=theme["muted"],
    )

    height = bottom + 28
    output_path.write_text(
        canvas.svg(
            width, height,
            "Metal API Remoter scheduler timeline",
            "Per-job waiting, GPU-computing and result-copyback phases for remotely "
            "submitted Metal command buffers, plus their overlap in wall-clock time.",
        ),
        encoding="utf-8",
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("server_log", type=Path, help="server log containing MAR_TIMELINE records")
    parser.add_argument("output_svg", type=Path, help="path for the generated SVG")
    parser.add_argument("--limit", type=int, default=10, help="jobs to detail in the top panel (default: 10)")
    parser.add_argument(
        "--rows", choices=("spread", "slowest", "first"), default="spread",
        help=(
            "which jobs the top panel details: spread = evenly sampled across the "
            "lifetime distribution, slowest = the N longest, first = the N earliest "
            "by enqueue (default: spread)"
        ),
    )
    parser.add_argument(
        "--gap-ms", type=float, default=400.0,
        help="idle gap that separates one client batch from the next (default: 400)",
    )
    parser.add_argument("--start-ms", type=float, help="window start, relative to the first enqueue")
    parser.add_argument("--window-ms", type=float, help="window length in milliseconds")
    parser.add_argument("--all", action="store_true", help="use every record instead of the densest burst")
    parser.add_argument("--theme", choices=("light", "dark"), default="light", help="colour theme (default: light)")
    args = parser.parse_args()

    if args.limit <= 0:
        parser.error("--limit must be positive")
    if args.gap_ms <= 0:
        parser.error("--gap-ms must be positive")

    try:
        records = parse_records(args.server_log)
    except OSError as error:
        parser.error(str(error))
    if not records:
        parser.error(f"no complete MAR_TIMELINE records found in {args.server_log}")

    window_records, window_label = select_window(
        records,
        gap_us=int(args.gap_ms * 1000),
        start_us=None if args.start_ms is None else int(args.start_ms * 1000),
        window_us=None if args.window_ms is None else int(args.window_ms * 1000),
        use_all=args.all,
    )
    if not window_records:
        parser.error("the requested window contains no completed jobs")

    number_window(window_records)

    ranked = sorted(window_records, key=lambda r: -r["lifetime_us"])
    if args.rows == "slowest":
        rows = ranked[: args.limit]
        row_label = f"The {len(rows)} slowest"
    elif args.rows == "first":
        rows = sorted(window_records, key=lambda r: r["enqueued_us"])[: args.limit]
        rows.sort(key=lambda r: -r["lifetime_us"])
        row_label = f"The {len(rows)} earliest"
    else:
        # Even ranks across the sorted distribution, so the panel shows the whole
        # spread rather than only the tail. Rank 0 keeps the slowest job visible.
        count = min(args.limit, len(ranked))
        if count == len(ranked):
            rows = ranked
        else:
            picks = {round(i * (len(ranked) - 1) / (count - 1)) for i in range(count)} if count > 1 else {0}
            rows = [ranked[i] for i in sorted(picks)]
        row_label = f"{len(rows)} sampled across the range"

    args.output_svg.parent.mkdir(parents=True, exist_ok=True)
    try:
        render_svg(
            rows, window_records, records, args.output_svg,
            args.theme, window_label, row_label,
        )
    except OSError as error:
        parser.error(str(error))

    print(
        f"Rendered {len(rows)} detailed rows from {len(window_records)} jobs "
        f"({window_label}) of {len(records)} completed jobs to {args.output_svg}"
    )


if __name__ == "__main__":
    main()
