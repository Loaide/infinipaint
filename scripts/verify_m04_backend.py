"""Recompute M04 targeted results from extracted captures (Python stdlib only).

Run from the repo root: python scripts/verify_m04_backend.py
Exit 0 means the raw evidence reproduces the saved summaries, not that the
backend gate passed. A reproduced parity failure is reported as such.
"""

import argparse
import csv
import gzip
import json
import re
import sys
from pathlib import Path


PERIOD_NS = 11_111_111
PORTRAIT = (1800, 2880)
LANDSCAPE = (2880, 1800)
TIMED_RUN = re.compile(r"(\d+)-(g3i|vki)-((?:sparse|overlap)-\d+)-n(\d+)-t(-?\d+)")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def text(path):
    return path.read_text(encoding="utf-8").strip()


def compare_summary(path, computed):
    saved = json.loads(text(path))
    for key in sorted(saved.keys() | computed.keys()):
        require(key in saved and key in computed and saved[key] == computed[key],
                f"{path}: {key}: saved {saved.get(key)!r}, computed {computed.get(key)!r}")


def frames(path):
    with path.open(encoding="utf-8", newline="") as stream:
        return [{k: v if k == "phase" else int(v) for k, v in row.items()}
                for row in csv.DictReader(stream)]


def distribution(values):
    values = sorted(values)
    n = len(values)
    return [values[(n * p + 99) // 100 - 1] for p in (50, 95, 99, 100)] if n else None


def identity(run, mode, scene, extent):
    mode_read, scene_read, width, height, fixture = text(run / "identity.txt").split()
    require((mode_read, scene_read, int(width), int(height)) == (mode, scene, *extent)
            and re.fullmatch(r"[0-9a-f]{16}", fixture), f"{run}: identity mismatch")
    return fixture


def pacing(run, n, timer):
    rows = [r for r in frames(run / "frames.csv") if r["phase"] == "measure"]
    require(len(rows) == n and all(b["seq"] == a["seq"] + 1 for a, b in zip(rows, rows[1:])),
            f"{run}: missing or noncontiguous measured frames")
    require(all(r["upload_bytes"] == 0 for r in rows), f"{run}: unexpected resident upload")
    actual = [r for r in rows if r["actual_present_ns"] > 0]
    gaps = [b["actual_present_ns"] - a["actual_present_ns"] for a, b in zip(actual, actual[1:])
            if b["seq"] == a["seq"] + 1 and b["actual_present_ns"] > a["actual_present_ns"]]
    missed = [max(0, round(gap / PERIOD_NS) - 1) for gap in gaps]
    age = [r["actual_present_ns"] - r["tick_ns"] for r in actual]
    gpu = [r["gpu_ns"] for r in rows if r["gpu_ns"] > 0]
    stats = {
        "n": n,
        "gpu_expected": sum(r["seq"] % 8 == 0 for r in rows) if timer == 1 else 0,
        "gpu_valid": len(gpu),
        "present_valid": len(actual),
        "wall_ns": distribution(r["end_ns"] - r["begin_ns"] for r in rows),
        "thread_cpu_ns": distribution(r["thread_cpu_ns"] for r in rows if r["thread_cpu_ns"] > 0),
        "gpu_ns": distribution(gpu),
        "present_gap_ns": distribution(gaps),
        "comparable_intervals": len(gaps),
        "tick_to_present_ns": distribution(age),
        "refreshes_missed_if_90Hz": sum(missed),
        **{key: distribution(r[key] for r in rows)
           for key in ("sync_ns", "submit_ns", "present_call_ns", "draws", "upload_bytes")},
    }
    # Missing/nonmonotonic feedback cannot be used to classify the entire run.
    if len(actual) != n or len(gaps) != n - 1:
        return dict(stats, gate_pass=None, present_span_s=None,
                    reason="missing/nonmonotonic presentation feedback")
    span = actual[-1]["actual_present_ns"] - actual[0]["actual_present_ns"]
    rate = (n - 1) * 1e9 / span
    delivery = len(gaps) / (len(gaps) + sum(missed))
    consecutive = any(m > 1 for m in missed) or any(a and b for a, b in zip(missed, missed[1:]))
    stats.update(observed_hz=rate, delivery_fraction=delivery, consecutive_misses=consecutive,
                 present_span_s=span / 1e9, gate_pass=rate >= 89.5 and delivery >= .995
                 and stats["present_gap_ns"][2] <= 11_610_000 and not consecutive)
    # Campaign windows are nonoverlapping 90-frame blocks; omit the trailing partial block.
    if n >= 90:
        start = max(range(0, n - 89, 90), key=lambda i: max(age[i:i + 90]))
        stats["worst_age_window"] = {"first_seq": rows[start]["seq"], "last_seq": rows[start + 89]["seq"],
                                     "max_tick_to_present_ns": max(age[start:start + 90])}
    return stats


def performance(root):
    runs, fixtures = {}, {}
    for run in sorted(root.iterdir()):
        match = TIMED_RUN.fullmatch(run.name)
        if not run.is_dir() or not match:
            continue
        index, mode, scene, n, timer = match.groups()
        key = (mode, scene, int(n), int(timer))
        require(key not in runs, f"{run}: duplicate timed run")
        require(text(run / "receipt.txt") == f"{int(index)} ok", f"{run}: invalid receipt")
        fixture = identity(run, mode, scene, PORTRAIT)
        require(fixtures.setdefault(scene, fixture) == fixture, f"{run}: fixture changed")
        stats = pacing(run, int(n), int(timer))
        compare_summary(run / "summary.json", stats)
        runs[key] = stats

    def take(mode, scene, n=360, timer=1):
        key = (mode, scene, n, timer)
        require(key in runs, f"{root}: missing run {key}")
        return runs.pop(key)

    bounds, sustained_candidates, confirmations = {}, [], {}
    for kind, cap in (("sparse", 1024), ("overlap", 2048)):
        for mode in ("g3i", "vki"):
            tested = {}
            load = 128
            while load <= cap:
                tested[load] = take(mode, f"{kind}-{load}")["gate_pass"]
                if tested[load] is not True:
                    break
                load *= 2
            if True not in tested.values():
                tested[64] = take(mode, f"{kind}-64")["gate_pass"]
            lo = max((n for n, p in tested.items() if p is True), default=None)
            hi = min((n for n, p in tested.items() if p is False), default=None)
            for load in sorted({n for n in (lo, hi) if n is not None}):
                scene = f"{kind}-{load}"
                stats = take(mode, scene, 1000)
                confirmations[f"{mode}-{scene}"] = stats
                if load == lo:
                    if stats["gate_pass"] is not True:
                        lo = None
                    elif mode == "vki":
                        sustained_candidates.append((stats["wall_ns"][2], scene))
                if load == hi and stats["gate_pass"] is not False:
                    hi = None
            bounds[f"{kind}-{mode}"] = {
                "last_confirmed_pass_strokes": lo, "first_confirmed_fail_strokes": hi,
                "tested": {str(n): p for n, p in tested.items()}, "cap": cap,
                "chords_per_stroke": 16, "unknown_loads": [n for n, p in tested.items() if p is None],
            }
    compare_summary(root / "capacity.json", bounds)
    require(sustained_candidates, f"{root}: no sustained candidate")
    load = max(sustained_candidates)[1]
    minimal = take("vki", load, 1000, -1)
    compare_summary(root / "near-limit-minimal.json", minimal)
    sustained = dict(take("vki", load, 55000), load=load)
    compare_summary(root / "sustained.json", sustained)
    require((sustained["present_span_s"] or 0) >= 600, f"{root}: sustained span below 600 s")
    require(not runs, f"{root}: unexpected timed runs: {sorted(runs)}")
    return {"capacity": bounds, "capacity_confirmations": confirmations,
            "sustained": sustained, "near_limit_minimal": minimal}


def pixels(run, extent):
    size = extent[0] * extent[1] * 4
    path = run / "pixels.rgba.gz"
    with gzip.open(path, "rb") as stream:
        data = stream.read(size + 1)
    require(len(data) == size, f"{path}: expected exactly {size} RGBA bytes")
    return data


def pixel_delta(a, b):
    require(len(a) == len(b), "pixel sizes differ")
    peak = 0
    for offset in range(0, len(a), 65536):
        x, y = a[offset:offset + 65536], b[offset:offset + 65536]
        if x != y:
            peak = max(peak, max(abs(m - n) for m, n in zip(x, y)))
    return peak


def lifecycle(root):
    refs, fixtures = {}, set()
    for index, extent, mode in ((1, LANDSCAPE, "g2"), (2, LANDSCAPE, "vki"),
                                (3, PORTRAIT, "g2"), (4, PORTRAIT, "vki")):
        suffix = "-landscape" if extent == LANDSCAPE else ""
        run = root / f"{index:03d}-{mode}-life-pixel-n1-t0{suffix}"
        require(text(run / "receipt.txt") == f"{index} ok", f"{run}: invalid receipt")
        fixtures.add(identity(run, mode, "life-pixel", extent))
        refs[extent, mode] = pixels(run, extent)
    dest = root / "lifecycle"
    require(text(dest / "receipt.txt") == "ok", f"{dest}: invalid receipt")
    log = text(dest / "backend.log")
    waits = re.findall(r"vk teardown waitIdle=(-?\d+)", log)
    allocations = re.findall(r"vk teardown explicit_buffer_allocated=(\d+) peak=(\d+)", log)
    require(waits == ["0"] * 4 and [remaining for remaining, _ in allocations] == ["0"] * 4,
            f"{dest}: expected four successful, fully retired Vulkan teardowns")

    recovered = {}
    for stage, extent in enumerate((PORTRAIT, LANDSCAPE, PORTRAIT, PORTRAIT)):
        run = dest / f"stage{stage}"
        fixtures.add(identity(run, "vki", "life-pixel" if stage else "life-live", extent))
        rows = frames(run / "frames.csv")
        expected = [(1, 0)] if stage else [(n, 28) for n in range(1, 9)]
        require([(r["seq"], r["upload_bytes"]) for r in rows] == expected,
                f"{run}: unexpected frame sequence or uploads")
        if stage:
            recovered[stage] = pixels(run, extent)
    require(len(fixtures) == 1, f"{root}: lifecycle fixture mismatch")
    deltas = {f"fresh_vki_vs_g2_{name}": pixel_delta(refs[extent, "vki"], refs[extent, "g2"])
              for name, extent in (("portrait", PORTRAIT), ("landscape", LANDSCAPE))}
    for stage, image in recovered.items():
        extent = LANDSCAPE if stage == 1 else PORTRAIT
        deltas[f"stage{stage}_vs_fresh_vki"] = pixel_delta(image, refs[extent, "vki"])
        deltas[f"stage{stage}_vs_g2"] = pixel_delta(image, refs[extent, "g2"])
    deltas["portrait_after_background_vs_after_rotation"] = pixel_delta(recovered[2], recovered[3])
    failed = {key: value for key, value in deltas.items() if value > 2}
    parity = {"max_channel_delta": deltas, "threshold": 2, "failed": failed, "pass": not failed}
    compare_summary(dest / "parity.json", parity)
    return dict(parity, suffix_uploads=expected_suffix_uploads(dest), clean_teardowns=len(waits),
                explicit_buffer_peak_bytes=max(int(peak) for _, peak in allocations))


def expected_suffix_uploads(dest):
    return [r["upload_bytes"] for r in frames(dest / "stage0" / "frames.csv")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--targeted", type=Path, default=Path("captures/backend-targeted-02"))
    parser.add_argument("--lifecycle", type=Path, default=Path("captures/backend-lifecycle-04"))
    args = parser.parse_args()
    try:
        results = performance(args.targeted)
        results["lifecycle"] = lifecycle(args.lifecycle)
    except (OSError, ValueError, TypeError, KeyError, AttributeError, EOFError, csv.Error) as exc:
        print(f"M04 evidence verification failed: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(results, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
