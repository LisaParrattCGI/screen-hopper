#!/usr/bin/env python3

import json
import math
import statistics
import sys
from pathlib import Path

MACOS_CURSOR_SCALE = 96.0 / 67.0
MACOS_FIXED_SCALE = 65536.0
MACOS_VELOCITY_FLOOR = 1.0 / MACOS_FIXED_SCALE
MACOS_ACCEL_CURVES = [
    (0, 65536, 0, 0, 0, 524288, 0),
    (8192, 60293, 26214, 5243, 0, 537395, 1245184),
    (32768, 60948, 36045, 6554, 0, 543949, 1179648),
    (45056, 61604, 46531, 7864, 0, 550502, 1114112),
    (57344, 62259, 57672, 9830, 0, 557056, 1048576),
    (65536, 62915, 69468, 11796, 0, 563610, 983040),
    (98304, 63570, 81920, 14418, 0, 570163, 917504),
    (131072, 64225, 95027, 17695, 0, 576717, 851968),
    (163840, 64881, 108790, 21627, 0, 583270, 786432),
    (196608, 65536, 123208, 26214, 0, 589824, 786432),
]
SUPPORTED_LOG_VERSIONS = {3, 4, 5, 6}
EDGE_EPSILON = 2.0
EDGE_DELTA_EPSILON = 2.0


def has_path(row, path):
    value = row
    for key in path:
        if not isinstance(value, dict) or key not in value:
            return False
        value = value[key]
    return True


def is_analysis_row(row):
    if not isinstance(row, dict):
        return False

    event = row.get("event")
    if event is not None and event != "debug_sample":
        return False

    required_paths = [
        ("version",),
        ("timestamp",),
        ("host", "delta_x"),
        ("host", "delta_y"),
        ("diagnostics", "host_correction_x"),
        ("diagnostics", "host_correction_y"),
        ("interval", "predicted_dx"),
        ("interval", "predicted_dy"),
        ("interval", "sent_dx"),
        ("interval", "sent_dy"),
        ("interval", "host_correction_x"),
        ("interval", "host_correction_y"),
        ("interval", "movement_sent"),
        ("interval", "prediction_reports_applied"),
        ("interval", "placement_reports_delivered"),
        ("interval", "transmit_failures"),
        ("interval", "screen_changes_predicted"),
        ("mouse", "hopper"),
        ("queue", "depth"),
        ("queue", "movement_sent"),
        ("totals", "prediction_reports_applied"),
        ("totals", "placement_reports_delivered"),
        ("totals", "transmit_failures"),
        ("totals", "screen_changes_predicted"),
    ]
    return all(has_path(row, path) for path in required_paths)


def percentile(values, fraction):
    ordered = sorted(values)
    if not ordered:
        return 0
    return ordered[min(len(ordered) - 1, round((len(ordered) - 1) * fraction))]


def magnitude(x, y):
    return math.hypot(x, y)


def fixed(value):
    return value / MACOS_FIXED_SCALE


def curve_from_fixed(curve):
    return tuple(fixed(value) for value in curve)


def lerp(a, b, ratio):
    return a + (b - a) * ratio


def interpolate_curve(tracking_speed):
    low = curve_from_fixed(MACOS_ACCEL_CURVES[0])
    if tracking_speed <= low[0]:
        return low
    for raw_curve in MACOS_ACCEL_CURVES[1:]:
        high = curve_from_fixed(raw_curve)
        if tracking_speed <= high[0]:
            span = high[0] - low[0]
            if span == 0:
                return high
            ratio = (tracking_speed - low[0]) / span
            return tuple(lerp(a, b, ratio) for a, b in zip(low, high))
        low = high
    return low


def curve_segment1(curve, value):
    _, gain_linear, gain_parabolic, gain_cubic, gain_quartic, _, _ = curve
    return (
        gain_linear * value
        + (gain_parabolic * value) ** 2
        + (gain_cubic * value) ** 3
        + (gain_quartic * value) ** 4
    )


def curve_segment1_slope(curve, value):
    _, gain_linear, gain_parabolic, gain_cubic, gain_quartic, _, _ = curve
    return (
        gain_linear
        + 2.0 * value * gain_parabolic**2
        + 3.0 * value**2 * gain_cubic**3
        + 4.0 * value**3 * gain_quartic**4
    )


def curve_secondary(curve):
    _, _, _, _, _, tangent_linear, tangent_root = curve
    first_tangent = 1 if 0.0 < tangent_root < tangent_linear else 0
    if first_tangent == 0:
        y0 = curve_segment1(curve, tangent_linear)
        m0 = curve_segment1_slope(curve, tangent_linear)
        b0 = y0 - m0 * tangent_linear
        y1 = m0 * tangent_root + b0
    else:
        y0 = 0.0
        y1 = curve_segment1(curve, tangent_root)
        m0 = curve_segment1_slope(curve, tangent_root)
        b0 = 0.0
    return {
        "first_tangent": first_tangent,
        "m0": m0,
        "b0": b0,
        "y0": y0,
        "y1": y1,
        "m_root": m0 * y1 * 2.0,
        "b_root": y1 * y1 - (m0 * y1 * 2.0) * tangent_root,
    }


def curve_value(curve, value):
    secondary = curve_secondary(curve)
    tangent_linear = curve[5]
    tangent_root = curve[6]
    first_tangent = tangent_linear if secondary["first_tangent"] == 0 else tangent_root
    if first_tangent != 0.0 and value <= first_tangent:
        return curve_segment1(curve, value)
    if secondary["first_tangent"] == 0 and tangent_root != 0.0 and value <= tangent_root:
        return secondary["m0"] * value + secondary["b0"]
    return math.sqrt(max(0.0, secondary["m_root"] * value + secondary["b_root"]))


def apply_model(dx, dy, mouse):
    if dx == 0 and dy == 0:
        return 0.0, 0.0
    raw_dx = float(dx)
    raw_dy = float(dy)
    velocity = max(math.floor(magnitude(raw_dx, raw_dy)), MACOS_VELOCITY_FLOOR)
    fixed_multiplier = mouse.get("fixed_multiplier", 1.0)
    adjusted = max(velocity * fixed_multiplier, MACOS_VELOCITY_FLOOR)
    curve = interpolate_curve(mouse.get("tracking", 0.6875))
    device_scale = mouse.get("resolution", 400.0) / mouse.get("frame_rate", 67.0)
    standardized = adjusted / device_scale
    accelerated_magnitude = curve_value(curve, standardized) * MACOS_CURSOR_SCALE
    multiplier = accelerated_magnitude / adjusted
    return raw_dx * multiplier, raw_dy * multiplier


def dot(ax, ay, bx, by):
    return ax * bx + ay * by


def projection_gain(dx, dy, x, y):
    sent_power = dx * dx + dy * dy
    if sent_power == 0:
        return None
    return dot(x, y, dx, dy) / sent_power


def perpendicular_error(dx, dy, x, y):
    sent_mag = magnitude(dx, dy)
    if sent_mag == 0:
        return None
    return abs(dx * y - dy * x) / sent_mag


def finite(value):
    return value is not None and math.isfinite(value)


def likely_axis_clamped(row, axis, bound_name):
    host = row.get("host", {})
    interval = row.get("interval", {})
    position = host.get(axis)
    delta = host.get(f"delta_{axis}")
    predicted = interval.get(f"predicted_d{axis}")
    sent = interval.get(f"sent_d{axis}")
    if not all(finite(value) for value in [position, delta, predicted, sent]):
        return False

    previous = position - delta
    if position <= EDGE_EPSILON:
        if previous + predicted < -EDGE_EPSILON or previous + sent < -EDGE_EPSILON:
            return True

    bound = host.get(bound_name)
    if finite(bound) and bound > 0 and position >= bound - EDGE_EPSILON:
        if previous + predicted > bound + EDGE_EPSILON or previous + sent > bound + EDGE_EPSILON:
            return True

    blocked = abs(delta) <= max(EDGE_DELTA_EPSILON, abs(predicted) * 0.35)
    if not blocked:
        return False

    if position <= EDGE_EPSILON and (predicted < -EDGE_DELTA_EPSILON or sent < -EDGE_DELTA_EPSILON):
        return True

    if finite(bound) and bound > 0:
        if position >= bound - EDGE_EPSILON and (predicted > EDGE_DELTA_EPSILON or sent > EDGE_DELTA_EPSILON):
            return True

    return False


def likely_host_edge_clamped(row):
    return likely_axis_clamped(row, "x", "display_width") or likely_axis_clamped(row, "y", "display_height")


def describe(name, values):
    values = [value for value in values if finite(value)]
    if not values:
        print(f"{name}: none")
        return
    print(
        f"{name}: min={min(values):.3f} median={statistics.median(values):.3f} "
        f"p90={percentile(values, 0.90):.3f} max={max(values):.3f}"
    )


def signed_ratio(observed, modeled):
    if not finite(observed) or not finite(modeled) or abs(modeled) < 1e-9:
        return None
    return observed / modeled


def speed_bucket(sent_mag):
    limits = [1, 2, 4, 8, 16, 32, 64, 128, 256, 512]
    lower = 0
    for upper in limits:
        if sent_mag <= upper:
            return f"{lower:g}-{upper:g}"
        lower = upper
    return f">{limits[-1]:g}"


def collect_interval_metrics(rows):
    metrics = []
    for row in rows:
        interval = row.get("interval")
        if not interval:
            continue

        sent_dx = interval.get("sent_dx", 0)
        sent_dy = interval.get("sent_dy", 0)
        sent_mag = magnitude(sent_dx, sent_dy)
        host_dx = row["host"]["delta_x"]
        host_dy = row["host"]["delta_y"]
        predicted_dx = interval.get("predicted_dx", 0)
        predicted_dy = interval.get("predicted_dy", 0)
        coalesced_dx, coalesced_dy = apply_model(sent_dx, sent_dy, row["mouse"]["hopper"])
        observed_gain = projection_gain(sent_dx, sent_dy, host_dx, host_dy)
        model_gain = projection_gain(sent_dx, sent_dy, predicted_dx, predicted_dy)
        coalesced_gain = projection_gain(sent_dx, sent_dy, coalesced_dx, coalesced_dy)
        acceleration = row.get("acceleration", {})
        residual_x = host_dx - predicted_dx
        residual_y = host_dy - predicted_dy
        coalesced_residual_x = host_dx - coalesced_dx
        coalesced_residual_y = host_dy - coalesced_dy
        single_report = (
            interval.get("movement_sent", 0) == 1
            and interval.get("prediction_reports_applied", 0) == 1
        )

        metrics.append(
            {
                "row": row,
                "host_edge_clamped": likely_host_edge_clamped(row),
                "single_report": single_report,
                "movement_sent": interval.get("movement_sent", 0),
                "sent_mag": sent_mag,
                "host_mag": magnitude(host_dx, host_dy),
                "prediction_mag": magnitude(predicted_dx, predicted_dy),
                "residual_mag": magnitude(residual_x, residual_y),
                "observed_gain": observed_gain,
                "model_gain": model_gain,
                "coalesced_gain": coalesced_gain,
                "acceleration_delta_us": acceleration.get("delta_us"),
                "acceleration_rate_multiplier": acceleration.get("rate_multiplier"),
                "acceleration_velocity": acceleration.get("velocity"),
                "acceleration_adjusted_velocity": acceleration.get("adjusted_velocity"),
                "gain_ratio": signed_ratio(observed_gain, model_gain),
                "coalesced_gain_ratio": signed_ratio(observed_gain, coalesced_gain),
                "observed_perp": perpendicular_error(sent_dx, sent_dy, host_dx, host_dy),
                "model_perp": perpendicular_error(sent_dx, sent_dy, predicted_dx, predicted_dy),
                "coalesced_perp": perpendicular_error(sent_dx, sent_dy, coalesced_dx, coalesced_dy),
                "residual_perp": perpendicular_error(sent_dx, sent_dy, residual_x, residual_y),
                "coalesced_dx": coalesced_dx,
                "coalesced_dy": coalesced_dy,
                "coalesced_residual_mag": magnitude(coalesced_residual_x, coalesced_residual_y),
                "clean": (
                    sent_mag > 0
                    and single_report
                    and interval.get("placement_reports_delivered", 0) == 0
                    and interval.get("transmit_failures", 0) == 0
                    and interval.get("screen_changes_predicted", 0) == 0
                    and not likely_host_edge_clamped(row)
                ),
            }
        )
    return metrics


def print_gain_table(metrics):
    buckets = {}
    for metric in metrics:
        if not metric["clean"] or not finite(metric["gain_ratio"]):
            continue
        buckets.setdefault(speed_bucket(metric["sent_mag"]), []).append(metric)

    if not buckets:
        print("gain_by_sent_speed: none")
        return

    print("gain_by_sent_speed:")
    print("  bucket      n  sent_med  obs_gain_med  model_gain_med  obs/model_med  residual_med  perp_med")
    for bucket in sorted(buckets, key=lambda item: float(item.split("-")[0].replace(">", "999999"))):
        values = buckets[bucket]
        observed = [value["observed_gain"] for value in values if finite(value["observed_gain"])]
        modeled = [value["model_gain"] for value in values if finite(value["model_gain"])]
        coalesced = [value["coalesced_gain"] for value in values if finite(value["coalesced_gain"])]
        ratios = [value["gain_ratio"] for value in values if finite(value["gain_ratio"])]
        coalesced_ratios = [value["coalesced_gain_ratio"] for value in values if finite(value["coalesced_gain_ratio"])]
        residuals = [value["residual_mag"] for value in values]
        coalesced_residuals = [value["coalesced_residual_mag"] for value in values]
        perps = [value["residual_perp"] for value in values if finite(value["residual_perp"])]
        sent = [value["sent_mag"] for value in values]
        print(
            f"  {bucket:>9} {len(values):4d} "
            f"{statistics.median(sent):9.3f} "
            f"{statistics.median(observed):13.4f} "
            f"{statistics.median(modeled):14.4f} "
            f"{statistics.median(ratios):13.3f} "
            f"{statistics.median(residuals):12.3f} "
            f"{statistics.median(perps):8.3f}"
        )
        print(
            f"  {'coalesced':>9} {'':4s} "
            f"{'':9s} "
            f"{'':13s} "
            f"{statistics.median(coalesced):14.4f} "
            f"{statistics.median(coalesced_ratios):13.3f} "
            f"{statistics.median(coalesced_residuals):12.3f}"
        )


def print_model_verdict(metrics):
    clean = [metric for metric in metrics if metric["clean"] and finite(metric["gain_ratio"])]
    print(f"clean_model_intervals={len(clean)}")
    if len(clean) < 10:
        print("model_verdict=insufficient single-report intervals; current log is too aggregated for curve-shape diagnosis")
        return

    ratios = [metric["gain_ratio"] for metric in clean]
    residual_perps = [metric["residual_perp"] for metric in clean if finite(metric["residual_perp"])]
    normal_residuals = [metric["residual_mag"] for metric in clean]
    coalesced_residuals = [metric["coalesced_residual_mag"] for metric in clean]
    low_speed = [metric["gain_ratio"] for metric in clean if metric["sent_mag"] <= 16]
    high_speed = [metric["gain_ratio"] for metric in clean if metric["sent_mag"] > 64]
    ratio_median = statistics.median(ratios)
    ratio_p10 = percentile(ratios, 0.10)
    ratio_p90 = percentile(ratios, 0.90)
    spread = ratio_p90 - ratio_p10
    perp_median = statistics.median(residual_perps) if residual_perps else 0

    print(f"obs_model_gain_ratio: median={ratio_median:.3f} p10={ratio_p10:.3f} p90={ratio_p90:.3f} spread={spread:.3f}")
    print(f"residual_perpendicular_median={perp_median:.3f}")
    print(
        "coalesced_interval_hypothesis: "
        f"per_report_residual_med={statistics.median(normal_residuals):.3f} "
        f"coalesced_residual_med={statistics.median(coalesced_residuals):.3f}"
    )
    if low_speed and high_speed:
        print(
            "speed_ratio_shift: "
            f"low_med={statistics.median(low_speed):.3f} "
            f"high_med={statistics.median(high_speed):.3f} "
            f"delta={statistics.median(high_speed) - statistics.median(low_speed):.3f}"
        )

    if statistics.median(coalesced_residuals) < statistics.median(normal_residuals) * 0.65:
        print("model_verdict=coalescing/windowing likely; host resembles acceleration after grouping reports more than per-report acceleration")
    elif perp_median > 10:
        print("model_verdict=timing/windowing or axis delivery error dominates; host movement is not parallel to sent movement")
    elif abs(ratio_median - 1) > 0.25 and spread < 0.35:
        print("model_verdict=mostly constant scale error; suspect resolution, coordinate scale, or tracking parameter normalization")
    elif spread >= 0.35:
        print("model_verdict=speed-dependent gain error; suspect acceleration curve shape or obsolete macOS model")
    else:
        print("model_verdict=model shape broadly plausible; remaining error likely timing, correction cadence, or small-sample quantization")


def print_multi_report_verdict(metrics):
    multi = [
        metric
        for metric in metrics
        if not metric["single_report"]
        and not metric["host_edge_clamped"]
        and metric["sent_mag"] > 0
        and finite(metric["gain_ratio"])
    ]
    print(f"multi_report_model_intervals={len(multi)}")
    if len(multi) < 10:
        return

    ratios = [metric["gain_ratio"] for metric in multi]
    normal_residuals = [metric["residual_mag"] for metric in multi]
    coalesced_residuals = [metric["coalesced_residual_mag"] for metric in multi]
    report_counts = [metric["movement_sent"] for metric in multi]
    average_report_magnitudes = [
        metric["sent_mag"] / metric["movement_sent"]
        for metric in multi
        if metric["movement_sent"] > 0
    ]
    print(
        "multi_obs_model_gain_ratio: "
        f"median={statistics.median(ratios):.3f} "
        f"p10={percentile(ratios, 0.10):.3f} "
        f"p90={percentile(ratios, 0.90):.3f}"
    )
    print(
        "multi_report_shape: "
        f"reports_med={statistics.median(report_counts):.1f} "
        f"avg_sent_per_report_med={statistics.median(average_report_magnitudes):.3f} "
        f"per_report_residual_med={statistics.median(normal_residuals):.3f} "
        f"coalesced_residual_med={statistics.median(coalesced_residuals):.3f}"
    )
    if statistics.median(coalesced_residuals) > statistics.median(normal_residuals) * 2.0 and statistics.median(ratios) > 1.15:
        print("multi_report_verdict=per-report model underpredicts medium-speed movement; full-interval coalescing is too strong, so suspect curve source/tracking normalization or partial event grouping")
    elif statistics.median(coalesced_residuals) < statistics.median(normal_residuals) * 0.65:
        print("multi_report_verdict=host movement is closer to grouped/coalesced acceleration than per-report acceleration")
    else:
        print("multi_report_verdict=multi-report movement broadly matches per-report prediction")


def main():
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {Path(sys.argv[0]).name} LOG.jsonl")

    rows = []
    json_rows = 0
    skipped_events = {}
    skipped_malformed = 0
    errors = []
    for line_number, line in enumerate(Path(sys.argv[1]).read_text(errors="replace").splitlines(), 1):
        if not line.strip():
            continue
        try:
            row = json.loads(line)
            json_rows += 1
        except json.JSONDecodeError as error:
            errors.append((line_number, str(error)))
            continue

        if is_analysis_row(row):
            rows.append(row)
        else:
            if isinstance(row, dict):
                event = row.get("event")
                if event is None:
                    event = "non_analysis_json"
                skipped_events[event] = skipped_events.get(event, 0) + 1
            else:
                skipped_malformed += 1

    skipped_rows = json_rows - len(rows)
    print(f"json_rows={json_rows} analysis_rows={len(rows)} skipped_rows={skipped_rows} parse_errors={len(errors)}")
    if skipped_events:
        skipped_summary = ", ".join(f"{event}={count}" for event, count in sorted(skipped_events.items()))
        print(f"skipped_events: {skipped_summary}")
    if skipped_malformed:
        print(f"skipped_malformed_json_values={skipped_malformed}")
    if errors:
        for line_number, error in errors[:5]:
            print(f"parse_error line={line_number}: {error}")
        return 1
    if not rows:
        return 0

    versions = sorted({row.get("version") for row in rows})
    print(f"versions={versions}")

    unsupported_versions = [version for version in versions if version not in SUPPORTED_LOG_VERSIONS]
    if unsupported_versions:
        print(f"warning: this analyzer expects interval diagnostics versions {sorted(SUPPORTED_LOG_VERSIONS)}")

    corrections = [
        magnitude(row["diagnostics"]["host_correction_x"], row["diagnostics"]["host_correction_y"])
        for row in rows
    ]
    residuals = [
        magnitude(
            row["host"]["delta_x"] - row["interval"]["predicted_dx"],
            row["host"]["delta_y"] - row["interval"]["predicted_dy"],
        )
        for row in rows
        if "interval" in row
    ]
    interval_predictions = [
        magnitude(row["interval"]["predicted_dx"], row["interval"]["predicted_dy"])
        for row in rows
        if "interval" in row
    ]
    interval_host = [
        magnitude(row["host"]["delta_x"], row["host"]["delta_y"])
        for row in rows
        if "interval" in row
    ]

    describe("host_correction_mag", corrections)
    describe("host_minus_interval_prediction_mag", residuals)
    describe("interval_host_delta_mag", interval_host)
    describe("interval_prediction_mag", interval_predictions)

    totals = rows[-1].get("totals", {})
    queue_depths = [row["queue"]["depth"] for row in rows]
    intervals = [row.get("interval", {}) for row in rows]
    print(f"queue_depth_max={max(queue_depths)}")
    print(f"movement_sent={totals.get('movement_sent', rows[-1]['queue']['movement_sent'])}")
    print(f"prediction_reports_applied={totals.get('prediction_reports_applied')}")
    print(f"placement_reports_delivered={totals.get('placement_reports_delivered')}")
    print(f"transmit_failures={totals.get('transmit_failures')}")
    print(f"screen_changes_predicted={totals.get('screen_changes_predicted')}")
    print(f"intervals_with_transmit_failures={sum(1 for interval in intervals if interval.get('transmit_failures', 0) > 0)}")
    print(f"intervals_with_placement={sum(1 for interval in intervals if interval.get('placement_reports_delivered', 0) > 0)}")
    print(f"intervals_without_prediction={sum(1 for interval in intervals if interval.get('movement_sent', 0) > 0 and interval.get('prediction_reports_applied', 0) == 0)}")

    metrics = collect_interval_metrics(rows)
    clean = [metric for metric in metrics if metric["clean"]]
    edge_clamped = [metric for metric in metrics if metric["host_edge_clamped"]]
    multi_report = [metric for metric in metrics if not metric["single_report"]]
    print(f"intervals_total={len(metrics)} clean_for_model={len(clean)}")
    print(f"intervals_likely_host_edge_clamped={len(edge_clamped)}")
    print(f"intervals_multi_report={len(multi_report)}")
    describe("clean_sent_mag", [metric["sent_mag"] for metric in clean])
    describe("clean_observed_gain", [metric["observed_gain"] for metric in clean])
    describe("clean_model_gain", [metric["model_gain"] for metric in clean])
    describe("clean_coalesced_interval_gain", [metric["coalesced_gain"] for metric in clean])
    describe("clean_obs_model_gain_ratio", [metric["gain_ratio"] for metric in clean])
    describe("clean_obs_coalesced_gain_ratio", [metric["coalesced_gain_ratio"] for metric in clean])
    describe("clean_residual_perpendicular", [metric["residual_perp"] for metric in clean])
    describe("clean_acceleration_delta_us", [metric["acceleration_delta_us"] for metric in clean])
    describe("clean_acceleration_rate_multiplier", [metric["acceleration_rate_multiplier"] for metric in clean])
    describe("clean_acceleration_velocity", [metric["acceleration_velocity"] for metric in clean])
    describe("clean_acceleration_adjusted_velocity", [metric["acceleration_adjusted_velocity"] for metric in clean])
    print_model_verdict(metrics)
    print_multi_report_verdict(metrics)
    print_gain_table(metrics)

    print("largest residuals:")
    residual_rows = [
        (
            row,
            magnitude(
                row["host"]["delta_x"] - row["interval"]["predicted_dx"],
                row["host"]["delta_y"] - row["interval"]["predicted_dy"],
            ),
        )
        for row in rows
        if "interval" in row
    ]
    for row, residual in sorted(residual_rows, key=lambda item: item[1], reverse=True)[:10]:
        print(
            f"{row['timestamp']} residual={residual:.3f} "
            f"host=({row['host']['delta_x']:.3f},{row['host']['delta_y']:.3f}) "
            f"pred=({row['interval']['predicted_dx']:.3f},{row['interval']['predicted_dy']:.3f}) "
            f"sent=({row['interval']['sent_dx']},{row['interval']['sent_dy']}) "
            f"reports={row['interval']['movement_sent']} "
            f"placements={row['interval']['placement_reports_delivered']} "
            f"single_report={row['interval']['movement_sent'] == 1 and row['interval']['prediction_reports_applied'] == 1} "
            f"rate_multiplier={row.get('acceleration', {}).get('rate_multiplier', 'n/a')} "
            f"delta_us={row.get('acceleration', {}).get('delta_us', 'n/a')} "
            f"edge_clamped={likely_host_edge_clamped(row)} "
            f"correction=({row['interval']['host_correction_x']:.3f},{row['interval']['host_correction_y']:.3f})"
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
