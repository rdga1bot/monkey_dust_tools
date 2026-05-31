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
    npc_delta_pcts: list[float]   = field(default_factory=list)   # delta тільки в NPC зоні
    contact_sheet_path: Optional[Path] = None
    delta_chart_path: Optional[Path]   = None
    gif_path: Optional[Path]           = None
    annotated_dir: Optional[Path]      = None
    tracking_chart_path: Optional[Path] = None
    npc_transparent_frames: int   = 0
    sky_missing_frames: int       = 0
    freeze_pairs: int             = 0
    jump_pairs: int               = 0
    twitch_pairs: int             = 0
    npc_freeze_pairs: int         = 0
    pose_freezes: list            = field(default_factory=list)
    npc_positions: list           = field(default_factory=list)
    npc_detected_pct: float       = 0.0  # % кадрів де NPC знайдено


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


def npc_roi(frame: np.ndarray) -> np.ndarray:
    """Центральна зона кадру де зазвичай знаходиться NPC (35-65% X, 20-80% Y)."""
    h, w = frame.shape[:2]
    return frame[h // 5: h * 4 // 5, w * 35 // 100: w * 65 // 100, :]


def check_npc_visibility(frame: np.ndarray) -> float:
    """% пікселів в NPC зоні з brightness > 0.15 (не чорний фон і не небо)."""
    roi = npc_roi(frame)
    brightness = np.mean(roi, axis=2)
    return float(np.mean(brightness > 0.15)) * 100.0


def npc_delta_pct(a: np.ndarray, b: np.ndarray) -> float:
    """Delta тільки в NPC зоні."""
    return frame_delta_pct(npc_roi(a), npc_roi(b))


# ── NPC Tracking ──────────────────────────────────────────────────────────────

def npc_centroid(frame: np.ndarray) -> Optional[tuple[float, float]]:
    """
    Повертає (cx, cy) нормалізовані [0,1] — центр мас NPC силуету в NPC зоні.
    NPC силует: яскраві пікселі, що не є небом (не домінує синій) і не є terrain
    (однорідний помаранчевий).  Повертає None якщо NPC не знайдено.
    """
    roi = npc_roi(frame)
    h, w = roi.shape[:2]
    # Небо: blue > red + 0.1 AND blue > 0.3
    sky_mask   = (roi[:,:,2] > roi[:,:,0] + 0.08) & (roi[:,:,2] > 0.28)
    # Terrain: R > G > B, warm, відносно однорідний (не NPC)
    # NPC: яскравіший та ближче до білого/сірого ніж terrain
    bright     = np.mean(roi, axis=2)
    r, g, b    = roi[:,:,0], roi[:,:,1], roi[:,:,2]
    # NPC pixels: яскраві, не небо, відносно нейтральні (|R-B| < 0.25)
    npc_mask   = (bright > 0.45) & ~sky_mask & (np.abs(r - b) < 0.25)
    if npc_mask.sum() < 30:
        return None
    ys, xs = np.where(npc_mask)
    return (float(np.mean(xs)) / w, float(np.mean(ys)) / h)


def track_npc_positions(frames: list[np.ndarray]) -> list[Optional[tuple[float, float]]]:
    """Трекінг позиції NPC через всі кадри."""
    return [npc_centroid(f) for f in frames]


def npc_pose_histogram(frame: np.ndarray) -> np.ndarray:
    """
    Гістограма NPC зони (32 bins, RGB flattened) — fingerprint пози.
    Два кадри з однаковою позою матимуть майже ідентичну гістограму.
    """
    roi = npc_roi(frame)
    hist = np.concatenate([
        np.histogram(roi[:,:,c], bins=32, range=(0, 1))[0].astype(np.float32)
        for c in range(3)
    ])
    norm = hist.sum()
    return hist / norm if norm > 0 else hist


def histogram_similarity(h1: np.ndarray, h2: np.ndarray) -> float:
    """Bhattacharyya distance [0=identical, 1=totally different]."""
    bc = np.sum(np.sqrt(h1 * h2))
    return float(1.0 - bc)


# ── Pose Freeze Detector ──────────────────────────────────────────────────────

@dataclass
class PoseFreezeEvent:
    start_frame: int
    end_frame: int
    duration: int        # кількість кадрів
    mean_similarity: float  # 0=identical poses


def detect_pose_freezes(
    frames: list[np.ndarray],
    frame_paths: list[Path],
    freeze_threshold: float = 0.03,   # Bhattacharyya < 0.03 → pose identical
    min_duration: int = 5,            # мінімум 5 однакових поз підряд
) -> list[PoseFreezeEvent]:
    """
    Виявляє послідовності кадрів де поза NPC не змінюється.
    Відрізняється від frame-freeze: камера може рухатись, а NPC завмерти.
    """
    hists = [npc_pose_histogram(f) for f in frames]
    events: list[PoseFreezeEvent] = []
    run_start = 0
    run_sims: list[float] = []

    for i in range(1, len(hists)):
        sim = histogram_similarity(hists[i-1], hists[i])
        if sim < freeze_threshold:
            if not run_sims:
                run_start = i - 1
            run_sims.append(sim)
        else:
            if len(run_sims) >= min_duration:
                events.append(PoseFreezeEvent(
                    start_frame=int(frame_paths[run_start].stem),
                    end_frame=int(frame_paths[i-1].stem),
                    duration=len(run_sims) + 1,
                    mean_similarity=float(np.mean(run_sims)),
                ))
            run_sims = []

    if len(run_sims) >= min_duration:
        events.append(PoseFreezeEvent(
            start_frame=int(frame_paths[run_start].stem),
            end_frame=int(frame_paths[len(frames)-1].stem),
            duration=len(run_sims) + 1,
            mean_similarity=float(np.mean(run_sims)),
        ))
    return events


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

    # ── Frame diff analysis (full frame + NPC region) ──
    for i in range(len(frames) - 1):
        d     = frame_delta_pct(frames[i], frames[i + 1])
        d_npc = npc_delta_pct(frames[i], frames[i + 1])
        analysis.delta_pcts.append(d)
        analysis.npc_delta_pcts.append(d_npc)

        fi = int(frame_paths[i].stem)
        fj = int(frame_paths[i + 1].stem)

        if d < FREEZE_DELTA_PCT:
            analysis.freeze_pairs += 1
            analysis.anomalies.append(FrameAnomaly(
                fi, fj, "freeze", d,
                f"Сцена заморожена: delta={d:.2f}%"
            ))
        elif d > JUMP_DELTA_PCT:
            analysis.jump_pairs += 1
            analysis.anomalies.append(FrameAnomaly(
                fi, fj, "jump", d,
                f"Різкий стрибок: delta={d:.2f}%"
            ))
        elif TWITCH_DELTA_PCT < d <= JUMP_DELTA_PCT:
            analysis.twitch_pairs += 1

        # NPC заморожений при русі камери (камера рухається, NPC стоїть)
        if d > 1.0 and d_npc < 0.3:
            analysis.npc_freeze_pairs += 1
            if analysis.npc_freeze_pairs <= 10:
                analysis.anomalies.append(FrameAnomaly(
                    fi, fj, "npc_freeze",
                    d_npc,
                    f"NPC не анімується (камера={d:.1f}% NPC={d_npc:.2f}%)"
                ))

    # Тремтіння: 3+ consecutive twitch
    deltas = analysis.delta_pcts
    for i in range(2, len(deltas) - 1):
        window = deltas[i-2:i+2]
        if all(TWITCH_DELTA_PCT < d < 15.0 for d in window):
            fi = int(frame_paths[i].stem)
            analysis.anomalies.append(FrameAnomaly(
                fi, -1, "twitch", float(np.mean(window)),
                f"Тремтіння (3+ кадри): mid_delta={np.mean(window):.2f}%"
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

    # ── Delta chart (full + NPC) ──
    analysis.delta_chart_path = _make_delta_chart(
        analysis.delta_pcts, analysis.npc_delta_pcts, report_dir, capture_id
    )

    # ── NPC Tracking ──
    print(f"  NPC tracking...", end=" ", flush=True)
    analysis.npc_positions = track_npc_positions(frames)
    detected = sum(1 for p in analysis.npc_positions if p is not None)
    analysis.npc_detected_pct = detected / len(frames) * 100.0
    print(f"detected {detected}/{len(frames)} frames")

    # NPC не знайдений довгий час = аномалія
    missing_run = 0
    for i, pos in enumerate(analysis.npc_positions):
        if pos is None:
            missing_run += 1
            if missing_run == 10:
                analysis.anomalies.append(FrameAnomaly(
                    int(frame_paths[max(0, i-9)].stem), -1,
                    "npc_missing", 0.0,
                    f"NPC не виявлено протягом 10+ кадрів підряд"
                ))
        else:
            missing_run = 0

    # Tracking chart
    analysis.tracking_chart_path = _make_tracking_chart(
        analysis.npc_positions, analysis.delta_pcts,
        frame_paths, report_dir, capture_id
    )

    # ── Pose Freeze Detector ──
    print(f"  Pose freeze detection...", end=" ", flush=True)
    analysis.pose_freezes = detect_pose_freezes(frames, frame_paths)
    print(f"{len(analysis.pose_freezes)} events")
    for pf in analysis.pose_freezes[:5]:
        analysis.anomalies.append(FrameAnomaly(
            pf.start_frame, pf.end_frame, "pose_freeze",
            float(pf.duration),
            f"Поза NPC заморожена {pf.duration} кадрів "
            f"(кадри {pf.start_frame:04d}-{pf.end_frame:04d})"
        ))

    # ── Animated GIF (4fps для огляду) ──
    analysis.gif_path = _make_gif(frame_paths, report_dir, capture_id, fps=4)

    # ── Annotated frames для аномалій ──
    analysis.annotated_dir = _make_annotated_frames(
        analysis.anomalies, frames, frame_paths, report_dir, capture_id,
        analysis.npc_positions
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


def _make_tracking_chart(
    positions: list[Optional[tuple[float, float]]],
    deltas: list[float],
    frame_paths: list[Path],
    report_dir: Path,
    capture_id: str,
) -> Optional[Path]:
    """
    Два графіки:
    1. NPC X/Y координати по кадрах (траєкторія)
    2. Scatter plot NPC позиції (де NPC знаходився у frame space)
    """
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt

        fig = plt.figure(figsize=(14, 8), dpi=100)

        # ── Subplot 1: X/Y over time ──
        ax1 = fig.add_subplot(2, 2, (1, 2))
        xs = [p[0] if p else np.nan for p in positions]
        ys = [p[1] if p else np.nan for p in positions]
        frame_nums = list(range(len(positions)))

        ax1.plot(frame_nums, xs, color="#3498db", lw=1.2, label="X (left-right)")
        ax1.plot(frame_nums, ys, color="#e74c3c", lw=1.2, label="Y (up-down)")
        ax1.set_ylim(0, 1)
        ax1.set_ylabel("Normalized position")
        ax1.set_title("NPC Position over Time")
        ax1.legend(loc="upper right", fontsize=8)
        ax1.grid(alpha=0.3)

        # Highlight freeze zones
        freeze_mask = [1.0 if (d < FREEZE_DELTA_PCT) else 0.0 for d in deltas] + [0.0]
        ax1.fill_between(frame_nums, 0, 1, where=[f > 0 for f in freeze_mask],
                         alpha=0.15, color="#e74c3c", label="Scene freeze")

        # ── Subplot 2: Scatter (NPC 2D trajectory) ──
        ax2 = fig.add_subplot(2, 2, 3)
        valid = [(p, i) for i, p in enumerate(positions) if p is not None]
        if valid:
            pts, idxs = zip(*valid)
            px = [p[0] for p in pts]
            py = [1.0 - p[1] for p in pts]  # flip Y для правильної орієнтації
            sc = ax2.scatter(px, py, c=idxs, cmap="viridis", s=8, alpha=0.7)
            plt.colorbar(sc, ax=ax2, label="Кадр")
        ax2.set_xlim(0, 1); ax2.set_ylim(0, 1)
        ax2.set_xlabel("X"); ax2.set_ylabel("Y")
        ax2.set_title("NPC 2D Trajectory")
        ax2.grid(alpha=0.3)

        # ── Subplot 3: NPC displacement per frame ──
        ax3 = fig.add_subplot(2, 2, 4)
        disps = []
        prev = None
        for p in positions:
            if p is not None and prev is not None:
                d = math.sqrt((p[0]-prev[0])**2 + (p[1]-prev[1])**2)
                disps.append(d * 100.0)
            else:
                disps.append(np.nan)
            prev = p
        ax3.bar(range(len(disps)), disps, color="#9b59b6", width=1.0, linewidth=0)
        ax3.set_xlabel("Кадр"); ax3.set_ylabel("Displacement %")
        ax3.set_title("NPC Frame Displacement")
        ax3.grid(alpha=0.3, axis="y")

        fig.tight_layout()
        out = report_dir / f"{capture_id}_tracking.png"
        fig.savefig(out)
        plt.close(fig)
        return out
    except Exception as e:
        print(f"[warn] tracking chart failed: {e}")
        return None


def _make_gif(frame_paths: list[Path], report_dir: Path, capture_id: str,
              fps: int = 4, max_frames: int = 60) -> Optional[Path]:
    """Animated GIF із фреймів (зменшений до 480p, max_frames кадрів для розміру)."""
    try:
        step = max(1, len(frame_paths) // max_frames)
        selected = frame_paths[::step][:max_frames]
        TARGET_W = 480
        pil_frames = []
        for p in selected:
            img = Image.open(p).convert("RGB")
            ratio = TARGET_W / img.width
            thumb = img.resize((TARGET_W, int(img.height * ratio)), Image.LANCZOS)
            pil_frames.append(thumb)

        if not pil_frames:
            return None
        out = report_dir / f"{capture_id}_preview.gif"
        pil_frames[0].save(
            out, save_all=True, append_images=pil_frames[1:],
            duration=int(1000 / fps), loop=0, optimize=False
        )
        return out
    except Exception as e:
        print(f"[warn] GIF failed: {e}")
        return None


def _make_annotated_frames(
    anomalies: list[FrameAnomaly],
    frames: list[np.ndarray],
    frame_paths: list[Path],
    report_dir: Path,
    capture_id: str,
    npc_positions: Optional[list] = None,
) -> Optional[Path]:
    """
    Для кожної унікальної аномалії зберегти annotated PNG:
    - Червона рамка навколо NPC зони
    - Текст з типом аномалії і значенням
    """
    ann_dir = report_dir / "annotated"
    ann_dir.mkdir(exist_ok=True)

    # Унікальні фрейми з аномаліями (max 30)
    seen_frames = set()
    to_annotate = []
    for a in anomalies:
        if a.frame_a not in seen_frames and len(to_annotate) < 30:
            seen_frames.add(a.frame_a)
            to_annotate.append(a)

    idx_map = {int(p.stem): i for i, p in enumerate(frame_paths)}

    COLORS = {
        "freeze":          (30, 144, 255),   # blue
        "jump":            (148, 0, 211),    # purple
        "sky_missing":     (255, 215, 0),    # gold
        "npc_transparent": (255, 20, 147),   # pink
        "twitch":          (255, 140, 0),    # orange
        "npc_freeze":      (220, 20, 60),    # crimson
    }

    for anom in to_annotate:
        fi = anom.frame_a
        if fi not in idx_map:
            continue
        frame_arr = frames[idx_map[fi]]
        H, W = frame_arr.shape[:2]

        img = Image.fromarray((frame_arr * 255).astype(np.uint8))
        draw = ImageDraw.Draw(img)
        color = COLORS.get(anom.kind, (255, 0, 0))

        # NPC bounding box (35-65% X, 20-80% Y)
        x0, x1 = int(W * 0.35), int(W * 0.65)
        y0, y1 = int(H * 0.20), int(H * 0.80)
        for lw in range(3):
            draw.rectangle([x0-lw, y0-lw, x1+lw, y1+lw], outline=color)

        # NPC centroid cross (if tracked)
        if npc_positions and idx_map.get(fi) is not None:
            pos = npc_positions[idx_map[fi]]
            if pos is not None:
                roi_w = x1 - x0
                roi_h = y1 - y0
                cx = x0 + int(pos[0] * roi_w)
                cy = y0 + int(pos[1] * roi_h)
                r = 8
                draw.ellipse([cx-r, cy-r, cx+r, cy+r], outline=(0,255,0), width=2)
                draw.line([cx-12, cy, cx+12, cy], fill=(0,255,0), width=2)
                draw.line([cx, cy-12, cx, cy+12], fill=(0,255,0), width=2)

        # Label
        label = f"{anom.kind.upper()}  {anom.value:.2f}  #{fi:04d}"
        draw.rectangle([0, 0, W, 32], fill=(0, 0, 0, 180))
        draw.text((8, 6), label, fill=color)

        out = ann_dir / f"{fi:04d}_{anom.kind}.png"
        img.save(out)

    if not list(ann_dir.iterdir()):
        return None
    return ann_dir


def _make_delta_chart(deltas: list[float], npc_deltas: list[float],
                      report_dir: Path, capture_id: str) -> Path:
    """PNG-графік frame-to-frame delta відсотків."""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import matplotlib.patches as mpatches

        fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(14, 5), dpi=100, sharex=True)
        x = list(range(len(deltas)))

        # Top: full-frame delta
        colors = []
        for d in deltas:
            if d < FREEZE_DELTA_PCT:   colors.append("#e74c3c")
            elif d > JUMP_DELTA_PCT:   colors.append("#9b59b6")
            elif d > TWITCH_DELTA_PCT: colors.append("#f39c12")
            else:                      colors.append("#2ecc71")
        ax1.bar(x, deltas, color=colors, width=1.0, linewidth=0)
        ax1.axhline(FREEZE_DELTA_PCT, color="#e74c3c", lw=0.8, linestyle="--", alpha=0.7)
        ax1.axhline(JUMP_DELTA_PCT,   color="#9b59b6", lw=0.8, linestyle="--", alpha=0.7)
        ax1.set_ylabel("Delta % (full)")
        ax1.set_title(f"Frame-to-frame delta — {capture_id}")
        ax1.set_ylim(0, min(max(deltas) * 1.2 + 1, 100) if deltas else 10)

        # Bottom: NPC region delta
        npc_colors = ["#dc143c" if d < 0.3 else "#1e90ff" for d in npc_deltas]
        ax2.bar(x[:len(npc_deltas)], npc_deltas, color=npc_colors, width=1.0, linewidth=0)
        ax2.set_ylabel("Delta % (NPC zone)")
        ax2.set_xlabel("Кадр")
        ax2.set_ylim(0, min(max(npc_deltas) * 1.2 + 0.5, 50) if npc_deltas else 5)
        ax2.axhline(0.3, color="#dc143c", lw=0.8, linestyle="--", alpha=0.7,
                    label="NPC freeze threshold")
        ax2.legend(loc="upper right", fontsize=7)

        patches = [
            mpatches.Patch(color="#e74c3c", label=f"Freeze (<{FREEZE_DELTA_PCT}%)"),
            mpatches.Patch(color="#f39c12", label="Twitch"),
            mpatches.Patch(color="#9b59b6", label=f"Jump (>{JUMP_DELTA_PCT}%)"),
            mpatches.Patch(color="#2ecc71", label="OK"),
        ]
        ax1.legend(handles=patches, loc="upper right", fontsize=7)
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
            "npc_transparent": "👻", "twitch": "🫨",
            "npc_freeze": "🎭"}.get(kind, "⚠️")


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
        f"| ❄️ Сцена заморожена (пари) | {cap.freeze_pairs} |",
        f"| 🎭 NPC не анімується (пари) | {cap.npc_freeze_pairs} |",
        f"| ⚡ Стрибки камери | {cap.jump_pairs} |",
        f"| 🫨 Тремтіння | {cap.twitch_pairs} |",
        f"| 🌑 Небо зникло (кадрів) | {cap.sky_missing_frames} |",
        f"| 👻 NPC прозорий (кадрів) | {cap.npc_transparent_frames} |",
        f"| 🎯 NPC виявлено (%) | {cap.npc_detected_pct:.1f}% |",
        f"| 🧊 Pose freeze events | {len(cap.pose_freezes)} |",
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

    # NPC Tracking
    if cap.tracking_chart_path:
        lines += [
            f"## 🎯 NPC Tracking",
            f"",
            f"**NPC виявлено:** {cap.npc_detected_pct:.1f}% кадрів",
            f"",
            f"![Tracking]({rel(cap.tracking_chart_path)})",
            f"",
        ]
        if cap.pose_freezes:
            lines += [f"### 🧊 Pose Freeze Events ({len(cap.pose_freezes)})", ""]
            lines += ["| Початок | Кінець | Тривалість (кадрів) | Схожість поз |",
                      "|---------|--------|---------------------|--------------|"]
            for pf in cap.pose_freezes[:15]:
                lines.append(
                    f"| {pf.start_frame:04d} | {pf.end_frame:04d} | "
                    f"{pf.duration} | {pf.mean_similarity:.4f} |"
                )
            lines.append("")

    # Animated GIF
    if cap.gif_path:
        lines += [
            f"## 🎬 Preview GIF (4fps, до 60 кадрів)",
            f"",
            f"![Preview]({rel(cap.gif_path)})",
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
            for a in alist[:10]:
                frame_ref = f"{a.frame_a:04d}" if a.frame_b < 0 else f"{a.frame_a:04d}→{a.frame_b:04d}"
                lines.append(f"| {frame_ref} | {a.value:.2f} | {a.desc} |")
            if len(alist) > 10:
                lines.append(f"| ... | | +{len(alist)-10} ще |")
            lines.append("")

        # Annotated frames gallery
        if cap.annotated_dir and cap.annotated_dir.exists():
            ann_files = sorted(cap.annotated_dir.glob("*.png"))
            if ann_files:
                lines += [f"### 🖼️ Annotated Frames ({len(ann_files)})", ""]
                for af in ann_files[:20]:
                    lines.append(f"![{af.stem}](annotated/{af.name})")
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
    if cap.npc_detected_pct < 50.0:
        recs.append(f"NPC виявлено лише в {cap.npc_detected_pct:.0f}% кадрів: NPC прозорий, поза камери або tracking потребує калібрування")
    if len(cap.pose_freezes) > 3:
        total_freeze_frames = sum(pf.duration for pf in cap.pose_freezes)
        recs.append(f"Pose freeze: {len(cap.pose_freezes)} events, {total_freeze_frames} кадрів — NPC не анімується. Перевірити breathing overlay, idle clip, animation LOD")
    if cap.npc_freeze_pairs > 20:
        recs.append("NPC не рухається (камера рухається, NPC стоїть): перевірити animation update, BT wander логіку")
    if cap.freeze_pairs > 50:
        recs.append("Вся сцена заморожена: перевірити QA сценарій і чи рухається камера")
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
