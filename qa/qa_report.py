#!/usr/bin/env python3
"""
qa_report.py — monkey_dust QA Report Generator

Аналізує capture директорію, детектить аномалії, генерує Markdown/HTML звіт.

Використання:
  python3 tools/qa/qa_report.py                         # latest capture
  python3 tools/qa/qa_report.py --capture 20260531_030631
  python3 tools/qa/qa_report.py --all                   # compare all captures
"""

import argparse
import json
import math
import os
import subprocess
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Optional

import numpy as np
from PIL import Image, ImageDraw, ImageFont

# ── Константи ─────────────────────────────────────────────────────────────────

REPO_ROOT     = Path(__file__).resolve().parents[2]
CAPTURES_DIR  = REPO_ROOT / "tools" / "qa" / "captures"
REPORTS_DIR   = REPO_ROOT / "tools" / "qa" / "reports"
BUILD_DIR     = REPO_ROOT / "build"
TESTS_BIN     = BUILD_DIR / "tests" / "md_tests"
BEHAVIOR_BIN  = BUILD_DIR / "tests" / "md_behavior_tests"

# Пороги аномалій
FREEZE_DELTA_PCT    = 0.15   # < 0.15% mean delta → NPC frozen
JUMP_DELTA_PCT      = 30.0   # > 30% → camera/NPC jump
TWITCH_DELTA_PCT    = 2.0    # 2-30% → локальний рух (можливе тремтіння)
SKY_BLUE_MIN_PCT    = 10.0   # верхня 1/4 кадру: blue channel < 10% → небо зникло
NPC_ALPHA_WARN_PCT  = 20.0   # NPC зона: < 20% непрозорих пікселів → NPC прозорий
CONTACT_THUMB_W     = 160
CONTACT_THUMB_H     = 90
CONTACT_COLS        = 20


# ── Структури даних ───────────────────────────────────────────────────────────

@dataclass
class FrameAnomaly:
    frame_a: int
    frame_b: int       # -1 якщо один кадр
    kind: str          # "freeze" | "jump" | "sky_missing" | "npc_transparent" | "twitch"
    value: float       # числовий показник
    desc: str


@dataclass
class CaptureAnalysis:
    capture_id: str
    frame_count: int
    width: int
    height: int
    anomalies: list[FrameAnomaly] = field(default_factory=list)
    delta_pcts: list[float]       = field(default_factory=list)
    contact_sheet_path: Optional[Path] = None
    delta_chart_path: Optional[Path]   = None
    npc_transparent_frames: int   = 0
    sky_missing_frames: int       = 0
    freeze_pairs: int             = 0
    jump_pairs: int               = 0
    twitch_pairs: int             = 0


# ── Аналіз фреймів ────────────────────────────────────────────────────────────

def load_frame(path: Path) -> np.ndarray:
    """RGB float32 [0,1], shape (H, W, 3)."""
    img = Image.open(path).convert("RGB")
    return np.array(img, dtype=np.float32) / 255.0


def frame_delta_pct(a: np.ndarray, b: np.ndarray) -> float:
    """Середній піксельний delta у відсотках [0, 100]."""
    return float(np.mean(np.abs(a - b))) * 100.0


def check_sky(frame: np.ndarray) -> float:
    """Blue channel mean у верхній чверті кадру [0,100]."""
    h = frame.shape[0]
    top = frame[:h // 4, :, :]
    return float(np.mean(top[:, :, 2])) * 100.0


def check_npc_visibility(frame: np.ndarray) -> float:
    """
    NPC зона: центральна вертикальна смуга (25-75% по X), верхня 2/3 по Y.
    Повертає % пікселів з brightness > 0.15 (не чорний фон).
    """
    h, w = frame.shape[:2]
    roi = frame[: h * 2 // 3, w // 4: w * 3 // 4, :]
    brightness = np.mean(roi, axis=2)
    return float(np.mean(brightness > 0.15)) * 100.0


def analyze_capture(capture_dir: Path, report_dir: Path) -> CaptureAnalysis:
    capture_id = capture_dir.name
    frames_dir = capture_dir / "frames"
    frame_paths = sorted(frames_dir.glob("*.png"))

    if not frame_paths:
        return CaptureAnalysis(capture_id=capture_id, frame_count=0, width=0, height=0)

    sample = load_frame(frame_paths[0])
    H, W = sample.shape[:2]
    analysis = CaptureAnalysis(
        capture_id=capture_id,
        frame_count=len(frame_paths),
        width=W, height=H,
    )

    frames = []
    for p in frame_paths:
        frames.append(load_frame(p))

    # ── Frame diff analysis ──
    for i in range(len(frames) - 1):
        d = frame_delta_pct(frames[i], frames[i + 1])
        analysis.delta_pcts.append(d)

        fi = int(frame_paths[i].stem)
        fj = int(frame_paths[i + 1].stem)

        if d < FREEZE_DELTA_PCT:
            analysis.freeze_pairs += 1
            analysis.anomalies.append(FrameAnomaly(
                fi, fj, "freeze", d,
                f"NPC/сцена заморожені: delta={d:.2f}%"
            ))
        elif d > JUMP_DELTA_PCT:
            analysis.jump_pairs += 1
            analysis.anomalies.append(FrameAnomaly(
                fi, fj, "jump", d,
                f"Різкий стрибок камери/NPC: delta={d:.2f}%"
            ))
        elif TWITCH_DELTA_PCT < d <= JUMP_DELTA_PCT:
            analysis.twitch_pairs += 1
            # не додаємо кожен — лише кластери

    # Тремтіння: 3+ consecutive twitch з малим варіансом
    deltas = analysis.delta_pcts
    for i in range(2, len(deltas) - 1):
        window = deltas[i-2:i+2]
        if all(TWITCH_DELTA_PCT < d < 15.0 for d in window):
            fi = int(frame_paths[i].stem)
            analysis.anomalies.append(FrameAnomaly(
                fi, -1, "twitch", float(np.mean(window)),
                f"Тремтіння анімації (3+ кадри): mid_delta={np.mean(window):.2f}%"
            ))

    # ── Per-frame checks ──
    for i, (frame, path) in enumerate(zip(frames, frame_paths)):
        fnum = int(path.stem)
        sky_blue = check_sky(frame)
        npc_vis  = check_npc_visibility(frame)

        if sky_blue < SKY_BLUE_MIN_PCT:
            analysis.sky_missing_frames += 1
            if analysis.sky_missing_frames <= 5:  # перші 5 — достатньо
                analysis.anomalies.append(FrameAnomaly(
                    fnum, -1, "sky_missing", sky_blue,
                    f"Небо відсутнє: blue_mean={sky_blue:.1f}%"
                ))

        if npc_vis < NPC_ALPHA_WARN_PCT:
            analysis.npc_transparent_frames += 1
            if analysis.npc_transparent_frames <= 3:
                analysis.anomalies.append(FrameAnomaly(
                    fnum, -1, "npc_transparent", npc_vis,
                    f"NPC прозорий/відсутній: visibility={npc_vis:.1f}%"
                ))

    # ── Contact sheet ──
    analysis.contact_sheet_path = _make_contact_sheet(
        frame_paths, report_dir, capture_id
    )

    # ── Delta chart ──
    analysis.delta_chart_path = _make_delta_chart(
        analysis.delta_pcts, report_dir, capture_id
    )

    return analysis


def _make_contact_sheet(frame_paths: list[Path], report_dir: Path, capture_id: str) -> Path:
    """Всі фрейми в одній PNG-стрічці (CONTACT_COLS колонок)."""
    n = len(frame_paths)
    rows = math.ceil(n / CONTACT_COLS)
    W = CONTACT_THUMB_W * CONTACT_COLS
    H = CONTACT_THUMB_H * rows
    sheet = Image.new("RGB", (W, H), (20, 20, 20))

    for idx, p in enumerate(frame_paths):
        thumb = Image.open(p).convert("RGB").resize(
            (CONTACT_THUMB_W, CONTACT_THUMB_H), Image.LANCZOS
        )
        col = idx % CONTACT_COLS
        row = idx // CONTACT_COLS
        sheet.paste(thumb, (col * CONTACT_THUMB_W, row * CONTACT_THUMB_H))

    out_path = report_dir / f"{capture_id}_contact.png"
    sheet.save(out_path, optimize=True)
    return out_path


def _make_delta_chart(deltas: list[float], report_dir: Path, capture_id: str) -> Path:
    """PNG-графік frame-to-frame delta відсотків."""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import matplotlib.patches as mpatches

        fig, ax = plt.subplots(figsize=(14, 3), dpi=100)
        x = list(range(len(deltas)))
        colors = []
        for d in deltas:
            if d < FREEZE_DELTA_PCT:
                colors.append("#e74c3c")    # red — freeze
            elif d > JUMP_DELTA_PCT:
                colors.append("#9b59b6")    # purple — jump
            elif d > TWITCH_DELTA_PCT:
                colors.append("#f39c12")    # orange — twitch
            else:
                colors.append("#2ecc71")    # green — ok

        ax.bar(x, deltas, color=colors, width=1.0, linewidth=0)
        ax.axhline(FREEZE_DELTA_PCT, color="#e74c3c", lw=0.8, linestyle="--", alpha=0.7)
        ax.axhline(JUMP_DELTA_PCT,   color="#9b59b6", lw=0.8, linestyle="--", alpha=0.7)
        ax.set_xlabel("Кадр")
        ax.set_ylabel("Delta %")
        ax.set_title(f"Frame-to-frame delta — {capture_id}")
        ax.set_ylim(0, min(max(deltas) * 1.2 + 1, 100) if deltas else 10)

        patches = [
            mpatches.Patch(color="#e74c3c", label=f"Freeze (<{FREEZE_DELTA_PCT}%)"),
            mpatches.Patch(color="#f39c12", label="Twitch"),
            mpatches.Patch(color="#9b59b6", label=f"Jump (>{JUMP_DELTA_PCT}%)"),
            mpatches.Patch(color="#2ecc71", label="OK"),
        ]
        ax.legend(handles=patches, loc="upper right", fontsize=7)
        fig.tight_layout()

        out_path = report_dir / f"{capture_id}_delta.png"
        fig.savefig(out_path)
        plt.close(fig)
        return out_path
    except Exception as e:
        print(f"[warn] matplotlib delta chart failed: {e}")
        return None


# ── Unit tests ────────────────────────────────────────────────────────────────

@dataclass
class TestResults:
    total: int = 0
    passed: int = 0
    failed: int = 0
    skipped: int = 0
    duration_s: float = 0.0
    failures: list[str] = field(default_factory=list)
    ran: bool = False


def run_tests(binary: Path, timeout_s: int = 120) -> TestResults:
    res = TestResults()
    if not binary.exists():
        return res
    t0 = time.time()
    try:
        proc = subprocess.run(
            [str(binary), "--gtest_color=no"],
            capture_output=True, text=True, timeout=timeout_s,
            cwd=REPO_ROOT,
        )
        res.duration_s = time.time() - t0
        res.ran = True
        out = proc.stdout + proc.stderr
        for line in out.splitlines():
            if line.startswith("[  PASSED  ]"):
                try:
                    res.passed = int(line.split()[2])
                except Exception:
                    pass
            if line.startswith("[  FAILED  ]"):
                try:
                    res.failed = int(line.split()[2])
                except Exception:
                    pass
            if line.startswith("[ RUN      ]"):
                res.total += 1
            if "FAILED" in line and line.startswith("[  FAILED  ]") and "test" not in line.lower():
                res.failures.append(line.strip())
    except subprocess.TimeoutExpired:
        res.failures.append(f"TIMEOUT after {timeout_s}s")
    except Exception as e:
        res.failures.append(str(e))
    return res


# ── Report generation ─────────────────────────────────────────────────────────

def severity_icon(kind: str) -> str:
    return {"freeze": "🧊", "jump": "⚡", "sky_missing": "🌑",
            "npc_transparent": "👻", "twitch": "🫨"}.get(kind, "⚠️")


def gen_markdown(
    analysis: CaptureAnalysis,
    unit: TestResults,
    behavior: TestResults,
    report_dir: Path,
    git_info: dict,
) -> str:
    now = datetime.now().strftime("%Y-%m-%d %H:%M")
    cap = analysis

    def rel(p: Optional[Path]) -> str:
        if p and p.exists():
            return str(p.relative_to(report_dir))
        return ""

    # Підрахунок статусів
    anom_by_kind = {}
    for a in cap.anomalies:
        anom_by_kind.setdefault(a.kind, []).append(a)

    total_anomalies = len(cap.anomalies)
    status_emoji = "✅" if total_anomalies == 0 else ("⚠️" if total_anomalies < 5 else "❌")

    lines = [
        f"# QA Report — monkey_dust",
        f"",
        f"**Дата:** {now}  ",
        f"**Capture:** `{cap.capture_id}`  ",
        f"**Commit:** `{git_info.get('hash', 'unknown')}` {git_info.get('msg', '')}  ",
        f"**Branch:** `{git_info.get('branch', '?')}`  ",
        f"",
        f"---",
        f"",
        f"## {status_emoji} Загальний статус",
        f"",
        f"| Метрика | Значення |",
        f"|---------|----------|",
        f"| Кадрів | {cap.frame_count} |",
        f"| Роздільність | {cap.width}×{cap.height} |",
        f"| Аномалій (всього) | {total_anomalies} |",
        f"| ❄️ Заморожені пари | {cap.freeze_pairs} |",
        f"| ⚡ Стрибки камери | {cap.jump_pairs} |",
        f"| 🫨 Тремтіння | {cap.twitch_pairs} |",
        f"| 🌑 Небо зникло (кадрів) | {cap.sky_missing_frames} |",
        f"| 👻 NPC прозорий (кадрів) | {cap.npc_transparent_frames} |",
        f"",
    ]

    # Delta chart
    if cap.delta_chart_path:
        lines += [
            f"## 📊 Frame-to-frame Delta",
            f"",
            f"![Delta Chart]({rel(cap.delta_chart_path)})",
            f"",
        ]
        if cap.delta_pcts:
            lines += [
                f"- **Середнє:** {np.mean(cap.delta_pcts):.2f}%",
                f"- **Максимум:** {max(cap.delta_pcts):.2f}% (кадр ~{np.argmax(cap.delta_pcts)})",
                f"- **Мінімум:** {min(cap.delta_pcts):.2f}%",
                f"",
            ]

    # Contact sheet
    if cap.contact_sheet_path:
        lines += [
            f"## 🎞️ Contact Sheet ({cap.frame_count} кадрів)",
            f"",
            f"![Contact Sheet]({rel(cap.contact_sheet_path)})",
            f"",
        ]

    # Anomalies
    if cap.anomalies:
        lines += [f"## ⚠️ Виявлені аномалії ({total_anomalies})", f""]
        for kind, alist in sorted(anom_by_kind.items()):
            icon = severity_icon(kind)
            lines.append(f"### {icon} {kind.replace('_', ' ').title()} ({len(alist)})")
            lines.append("")
            lines.append("| Кадр | Значення | Опис |")
            lines.append("|------|----------|------|")
            for a in alist[:10]:  # max 10 per category
                frame_ref = f"{a.frame_a:04d}" if a.frame_b < 0 else f"{a.frame_a:04d}→{a.frame_b:04d}"
                lines.append(f"| {frame_ref} | {a.value:.2f} | {a.desc} |")
            if len(alist) > 10:
                lines.append(f"| ... | | +{len(alist)-10} ще |")
            lines.append("")
    else:
        lines += [f"## ✅ Аномалії не виявлено", f""]

    # Unit tests
    lines += [f"## 🧪 Unit Tests", f""]

    for label, res in [("md_tests", unit), ("md_behavior_tests", behavior)]:
        if not res.ran:
            lines.append(f"- **{label}**: ⛔ не зібрано / не запущено")
            continue
        icon = "✅" if res.failed == 0 else "❌"
        lines.append(
            f"- **{label}**: {icon} {res.passed} passed, {res.failed} failed "
            f"({res.duration_s:.1f}s)"
        )
        for f in res.failures[:5]:
            lines.append(f"  - `{f}`")

    lines.append("")

    # Build info
    lines += [
        f"## 🔨 Build",
        f"",
        f"```",
        f"cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DUSE_SDL3=ON",
        f"```",
        f"",
    ]

    # Recommendations
    recs = []
    if cap.npc_transparent_frames > 5:
        recs.append("NPC прозорі: перевірити faction color alpha та blend state пайплайну")
    if cap.sky_missing_frames > 10:
        recs.append("Небо зникає: Sky рендер повинен бути ПЕРШИМ у pass (depth_test=false)")
    if cap.twitch_pairs > 20:
        recs.append("Тремтіння анімації: перевірити breathing overlay weight та bone mask")
    if cap.freeze_pairs > 50:
        recs.append("NPC/анімація заморожені: перевірити animation update і QA сценарій руху")
    if cap.jump_pairs > 5:
        recs.append("Різкі стрибки камери: плавність orbit камери, interpolation")

    if recs:
        lines += [f"## 💡 Рекомендації", f""]
        for r in recs:
            lines.append(f"- {r}")
        lines.append("")

    return "\n".join(lines)


def gen_html(md_text: str) -> str:
    """Мінімальний HTML wrapper навколо Markdown."""
    try:
        import markdown
        body = markdown.markdown(md_text, extensions=["tables", "fenced_code"])
    except ImportError:
        body = f"<pre>{md_text}</pre>"

    return f"""<!DOCTYPE html>
<html lang="uk">
<head>
<meta charset="utf-8">
<title>QA Report — monkey_dust</title>
<style>
  body {{ font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", monospace;
         max-width: 1200px; margin: 40px auto; padding: 0 20px;
         background: #0d1117; color: #c9d1d9; }}
  h1 {{ color: #f0f6fc; border-bottom: 1px solid #30363d; padding-bottom: 8px; }}
  h2 {{ color: #e6edf3; border-bottom: 1px solid #21262d; padding-bottom: 4px; }}
  h3 {{ color: #79c0ff; }}
  table {{ border-collapse: collapse; width: 100%; }}
  th, td {{ border: 1px solid #30363d; padding: 6px 12px; text-align: left; }}
  th {{ background: #161b22; }}
  tr:nth-child(even) {{ background: #161b22; }}
  code, pre {{ background: #161b22; padding: 2px 6px; border-radius: 4px; }}
  img {{ max-width: 100%; border: 1px solid #30363d; border-radius: 4px; }}
  a {{ color: #58a6ff; }}
</style>
</head>
<body>
{body}
</body>
</html>
"""


def get_git_info() -> dict:
    def run(cmd):
        try:
            return subprocess.check_output(cmd, cwd=REPO_ROOT, text=True,
                                           stderr=subprocess.DEVNULL).strip()
        except Exception:
            return ""
    return {
        "hash":   run(["git", "rev-parse", "--short", "HEAD"]),
        "msg":    run(["git", "log", "-1", "--format=%s"]),
        "branch": run(["git", "rev-parse", "--abbrev-ref", "HEAD"]),
    }


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="monkey_dust QA Report")
    parser.add_argument("--capture", help="ID capture директорії (default: latest)")
    parser.add_argument("--no-tests", action="store_true", help="Не запускати unit tests")
    parser.add_argument("--open", action="store_true", help="Відкрити HTML у браузері")
    args = parser.parse_args()

    # Вибір capture
    if args.capture:
        capture_dir = CAPTURES_DIR / args.capture
    else:
        captures = sorted(CAPTURES_DIR.iterdir()) if CAPTURES_DIR.exists() else []
        captures = [c for c in captures if c.is_dir() and (c / "frames").exists()]
        if not captures:
            print("[qa_report] Немає captures. Спочатку запусти гру.")
            sys.exit(1)
        capture_dir = captures[-1]

    print(f"[qa_report] Аналізую: {capture_dir.name}")

    # Report директорія
    report_dir = REPORTS_DIR / capture_dir.name
    report_dir.mkdir(parents=True, exist_ok=True)

    # Аналіз capture
    t0 = time.time()
    analysis = analyze_capture(capture_dir, report_dir)
    print(f"[qa_report] Аналіз: {time.time()-t0:.1f}s, {len(analysis.anomalies)} аномалій")

    # Unit tests
    unit_res = TestResults()
    behavior_res = TestResults()
    if not args.no_tests:
        print("[qa_report] Запускаю md_tests...")
        unit_res = run_tests(TESTS_BIN, timeout_s=180)
        print(f"[qa_report] md_tests: {unit_res.passed}P {unit_res.failed}F ({unit_res.duration_s:.1f}s)")
        print("[qa_report] Запускаю md_behavior_tests...")
        behavior_res = run_tests(BEHAVIOR_BIN, timeout_s=180)
        print(f"[qa_report] behavior: {behavior_res.passed}P {behavior_res.failed}F ({behavior_res.duration_s:.1f}s)")

    git_info = get_git_info()

    # Генерація звіту
    md_text = gen_markdown(analysis, unit_res, behavior_res, report_dir, git_info)
    html_text = gen_html(md_text)

    md_path   = report_dir / "report.md"
    html_path = report_dir / "report.html"
    md_path.write_text(md_text, encoding="utf-8")
    html_path.write_text(html_text, encoding="utf-8")

    # Anomalies JSON (для CI / інструментів)
    anoms_json = [
        {"frame_a": a.frame_a, "frame_b": a.frame_b,
         "kind": a.kind, "value": a.value, "desc": a.desc}
        for a in analysis.anomalies
    ]
    (report_dir / "anomalies.json").write_text(
        json.dumps(anoms_json, ensure_ascii=False, indent=2), encoding="utf-8"
    )

    print(f"\n[qa_report] ✅ Звіт: {html_path}")
    print(f"[qa_report]    MD:   {md_path}")

    # Підсумок у термінал
    total = len(analysis.anomalies)
    status = "PASS" if total == 0 else ("WARN" if total < 5 else "FAIL")
    print(f"\n[qa_report] STATUS: {status}")
    print(f"  Кадрів: {analysis.frame_count}")
    print(f"  Аномалії: {total} (freeze={analysis.freeze_pairs} jump={analysis.jump_pairs} "
          f"twitch={analysis.twitch_pairs} sky={analysis.sky_missing_frames} "
          f"npc_transp={analysis.npc_transparent_frames})")
    if unit_res.ran:
        print(f"  Tests: {unit_res.passed + behavior_res.passed} passed, "
              f"{unit_res.failed + behavior_res.failed} failed")

    if args.open:
        subprocess.Popen(["xdg-open", str(html_path)])

    return 0 if status != "FAIL" else 1


if __name__ == "__main__":
    sys.exit(main())
