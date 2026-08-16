#!/usr/bin/env python3
"""
md_dash.py -- один офлайн HTML-файл, що зводить стан 3-репо проєкту.
Ніякого сервера, ніяких зовнішніх CDN. Кожен блок показує час даних,
які показує -- або чесно каже "немає даних", якщо джерела нема.

Викликається як `tools/md_ctl dash` (Фаза 4 PROMPT_DOC_RESTRUCTURE.md).
"""
import html
import json
import re
import shutil
import subprocess
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SUBMODULES = ["engine", "tools"]
OUT_PATH = REPO_ROOT / "tmp_" / "md_ctl_dash.html"


def _run(cmd, cwd=None, timeout=30):
    try:
        p = subprocess.run(cmd, cwd=str(cwd) if cwd else None,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=timeout)
        return p.returncode, p.stdout
    except (subprocess.TimeoutExpired, FileNotFoundError) as e:
        return 1, str(e)


def _mtime_str(path: Path) -> str:
    if not path.exists():
        return None
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(path.stat().st_mtime))


# ── Git ──────────────────────────────────────────────────────────────────
def gather_git():
    repos = {"main (.)": REPO_ROOT}
    for sm in SUBMODULES:
        repos[f"{sm}/"] = REPO_ROOT / sm

    rows = []
    for label, path in repos.items():
        _, dirty = _run(["git", "status", "--short"], cwd=path)
        dirty_lines = [l for l in dirty.splitlines() if l.strip()]
        _, branch = _run(["git", "branch", "--show-current"], cwd=path)
        branch = branch.strip() or "(detached)"
        code, counts = _run(["git", "rev-list", "--left-right", "--count",
                              "@{u}...HEAD"], cwd=path)
        if code == 0 and counts.strip():
            behind, ahead = (counts.strip().split() + ["?", "?"])[:2]
        else:
            behind, ahead = "?", "?"
        rows.append({
            "label": label, "branch": branch,
            "dirty": len(dirty_lines),
            "dirty_sample": dirty_lines[0] if dirty_lines else "",
            "ahead": ahead, "behind": behind,
        })

    # submodule ref drift: main's recorded submodule commit vs actual HEAD
    drift = []
    _, sm_status = _run(["git", "submodule", "status"], cwd=REPO_ROOT)
    for line in sm_status.splitlines():
        m = re.match(r"^([ +\-U])([0-9a-f]{40})\s+(\S+)", line)
        if not m:
            continue
        flag, recorded_sha, sm_path = m.groups()
        _, actual_sha = _run(["git", "rev-parse", "HEAD"], cwd=REPO_ROOT / sm_path)
        actual_sha = actual_sha.strip()
        in_sync = flag == " " and recorded_sha == actual_sha
        drift.append({
            "path": sm_path, "recorded": recorded_sha[:10],
            "actual": actual_sha[:10] if actual_sha else "?",
            "in_sync": in_sync,
        })
    return rows, drift


# ── Build ────────────────────────────────────────────────────────────────
def gather_build():
    targets = {
        "game (monkey_dust)": REPO_ROOT / "build/game/monkey_dust",
        "editor (monkey_dust_editor)": REPO_ROOT / "build/tools/monkey_dust_editor",
        "editor panels (hot-reload .so)": REPO_ROOT / "build/hot/libeditor_panels.so",
    }
    rows = []
    for label, path in targets.items():
        rows.append({
            "label": label, "built": path.exists(),
            "mtime": _mtime_str(path) or "—",
        })
    return rows


# ── Tests ────────────────────────────────────────────────────────────────
def gather_tests():
    result = {"gtest_count": None, "behavior_count": None,
              "last_run": None, "last_run_summary": None}

    for name, key in (("md_tests", "gtest_count"), ("md_behavior_tests", "behavior_count")):
        binpath = REPO_ROOT / "build/tests" / name
        if binpath.exists():
            code, out = _run([str(binpath), "--gtest_list_tests"], timeout=15)
            if code == 0:
                # count leaf test lines: indented, no trailing '.'
                n = sum(1 for l in out.splitlines()
                        if l.startswith("  ") and not l.rstrip().endswith("."))
                result[key] = n

    log_dir = REPO_ROOT / "tmp_" / "md_ctl_logs"
    if log_dir.exists():
        candidates = sorted(
            [p for p in log_dir.glob("*.log") if "test" in p.name.lower() or "scenario" in p.name.lower()],
            key=lambda p: p.stat().st_mtime)
        if candidates:
            latest = candidates[-1]
            result["last_run"] = _mtime_str(latest)
            text = latest.read_text(errors="replace")
            m = re.search(r"\[==========\].*?\n.*?\[  PASSED  \]\s*(\d+)\s*tests?", text)
            fail_m = re.search(r"\[  FAILED  \]\s*(\d+)\s*tests?", text)
            if m:
                summary = f"{m.group(1)} passed"
                if fail_m:
                    summary += f", {fail_m.group(1)} failed"
                result["last_run_summary"] = summary
    return result


# ── QA ───────────────────────────────────────────────────────────────────
def gather_qa():
    reports_dir = REPO_ROOT / "tools" / "qa" / "reports"
    if not reports_dir.exists():
        return None
    dirs = sorted([d for d in reports_dir.iterdir() if d.is_dir()],
                  key=lambda d: d.stat().st_mtime)
    if not dirs:
        return None
    latest = dirs[-1]
    report_html = latest / "report.html"
    return {
        "capture_id": latest.name,
        "mtime": _mtime_str(latest),
        "has_report": report_html.exists(),
        "rel_path": str(report_html.relative_to(REPO_ROOT)) if report_html.exists() else None,
    }


# ── Doctor ───────────────────────────────────────────────────────────────
def gather_doctor():
    code, out = _run(["python3", "tools/md_doctor.py", "--json"], cwd=REPO_ROOT, timeout=60)
    try:
        data = json.loads(out)
        return data
    except (json.JSONDecodeError, ValueError):
        return {"error": out[-500:] if out else "no output", "returncode": code}


# ── Issues ───────────────────────────────────────────────────────────────
def gather_issues():
    if shutil.which("gh") is None:
        return None
    code, out = _run(["gh", "issue", "list", "-R", "rdga1bot/monkey_dust",
                       "--json", "number,title,state,updatedAt", "--limit", "20"],
                      cwd=REPO_ROOT, timeout=20)
    if code != 0:
        return None
    try:
        return json.loads(out)
    except (json.JSONDecodeError, ValueError):
        return None


# ── Budgets ──────────────────────────────────────────────────────────────
def gather_budgets():
    constitution = REPO_ROOT / "CLAUDE_CONSTITUTION.md"
    budget_lines = []
    if constitution.exists():
        text = constitution.read_text(errors="replace")
        m = re.search(r"## §13 · PERFORMANCE BUDGET\s*```(.*?)```", text, re.S)
        if m:
            budget_lines = [l.strip() for l in m.group(1).splitlines() if l.strip()]

    baseline_path = REPO_ROOT / "tools" / "qa" / "baselines" / "perf_baseline.json"
    baseline = None
    if baseline_path.exists():
        try:
            baseline = json.loads(baseline_path.read_text())
        except (json.JSONDecodeError, ValueError):
            baseline = None
    return {"budget_lines": budget_lines, "baseline": baseline,
            "baseline_mtime": _mtime_str(baseline_path)}


# ── Render ───────────────────────────────────────────────────────────────
def esc(s):
    return html.escape(str(s))


def render(git_rows, sm_drift, build_rows, tests, qa, doctor, issues, budgets):
    now = time.strftime("%Y-%m-%d %H:%M:%S")

    git_html = "".join(
        f"<tr><td>{esc(r['label'])}</td><td>{esc(r['branch'])}</td>"
        f"<td class='{'bad' if r['dirty'] else 'ok'}'>{r['dirty']}</td>"
        f"<td>{esc(r['dirty_sample'])}</td>"
        f"<td>{esc(r['ahead'])}/{esc(r['behind'])}</td></tr>"
        for r in git_rows)

    drift_html = "".join(
        f"<tr><td>{esc(d['path'])}</td><td>{esc(d['recorded'])}</td>"
        f"<td>{esc(d['actual'])}</td>"
        f"<td class='{'ok' if d['in_sync'] else 'bad'}'>{'OK' if d['in_sync'] else 'DRIFT'}</td></tr>"
        for d in sm_drift) or "<tr><td colspan=4 class='empty'>немає даних</td></tr>"

    build_html = "".join(
        f"<tr><td>{esc(r['label'])}</td>"
        f"<td class='{'ok' if r['built'] else 'bad'}'>{'built' if r['built'] else 'NOT BUILT'}</td>"
        f"<td>{esc(r['mtime'])}</td></tr>"
        for r in build_rows)

    tests_html = f"""
    <p>gtest (md_tests): <b>{esc(tests['gtest_count']) if tests['gtest_count'] is not None else 'немає даних (бінарник відсутній)'}</b></p>
    <p>behavior (md_behavior_tests): <b>{esc(tests['behavior_count']) if tests['behavior_count'] is not None else 'немає даних (бінарник відсутній)'}</b></p>
    <p>Останній прогін: <b>{esc(tests['last_run']) if tests['last_run'] else 'немає даних — прогнати `md_ctl test`'}</b>
    {f"({esc(tests['last_run_summary'])})" if tests['last_run_summary'] else ''}</p>
    """

    if qa is None:
        qa_html = "<p class='empty'>немає даних — жодного tools/qa/reports/ ще не згенеровано</p>"
    else:
        qa_html = f"<p>Останній capture: <b>{esc(qa['capture_id'])}</b> ({esc(qa['mtime'])})</p>"
        if qa["has_report"]:
            qa_html += f"<p><a href='../{esc(qa['rel_path'])}'>відкрити report.html</a></p>"
        else:
            qa_html += "<p class='empty'>report.html відсутній у цьому capture</p>"

    if "error" in doctor:
        doctor_html = f"<p class='bad'>помилка запуску: {esc(doctor['error'])}</p>"
    else:
        n_failed = doctor.get("failed_findings", "?")
        doctor_html = f"<p>Findings: <b class='{'ok' if n_failed == 0 else 'bad'}'>{esc(n_failed)}</b> ({esc(doctor.get('checks', '?'))} checks ran, {esc(doctor.get('skipped', 0))} skipped)</p>"
        per_check = doctor.get("per_check", {})
        if per_check:
            doctor_html += "<table><tr><th>check</th><th>failed</th><th>skipped</th></tr>" + "".join(
                f"<tr><td>{esc(k)}</td><td>{esc(v.get('failed', '?'))}</td><td>{esc(v.get('skipped', '?'))}</td></tr>"
                for k, v in per_check.items()) + "</table>"

    if issues is None:
        issues_html = "<p class='empty'>немає даних — `gh` недоступний або не автентифікований</p>"
    elif not issues:
        issues_html = "<p class='empty'>0 відкритих issues</p>"
    else:
        issues_html = "<table><tr><th>#</th><th>title</th><th>state</th><th>updated</th></tr>" + "".join(
            f"<tr><td>{esc(i['number'])}</td><td>{esc(i['title'])}</td>"
            f"<td>{esc(i['state'])}</td><td>{esc(i['updatedAt'])}</td></tr>" for i in issues) + "</table>"

    budget_lines_html = "<pre>" + esc("\n".join(budgets["budget_lines"])) + "</pre>" if budgets["budget_lines"] else "<p class='empty'>§13 не знайдено в CLAUDE_CONSTITUTION.md</p>"
    if budgets["baseline"]:
        budgets_html = budget_lines_html + f"<p>Виміряний baseline: {esc(budgets['baseline_mtime'])}</p><pre>{esc(json.dumps(budgets['baseline'], indent=2, ensure_ascii=False))}</pre>"
    else:
        budgets_html = budget_lines_html + "<p class='empty'>немає виміряного baseline (tools/qa/baselines/perf_baseline.json відсутній) — пороги показані, порівняння немає</p>"

    return f"""<!doctype html>
<html><head><meta charset="utf-8"><title>monkey_dust — md_ctl dash</title>
<style>
body {{ font-family: system-ui, sans-serif; max-width: 1000px; margin: 2rem auto; padding: 0 1rem;
       background: #0d1117; color: #c9d1d9; }}
h1 {{ font-size: 1.4rem; }}
h2 {{ font-size: 1.1rem; border-bottom: 1px solid #30363d; padding-bottom: .3rem; margin-top: 2rem; }}
table {{ border-collapse: collapse; width: 100%; margin: .5rem 0; }}
td, th {{ border: 1px solid #30363d; padding: .3rem .6rem; text-align: left; font-size: .9rem; }}
th {{ background: #161b22; }}
.ok {{ color: #3fb950; }}
.bad {{ color: #f85149; }}
.empty {{ color: #8b949e; font-style: italic; }}
.stamp {{ color: #8b949e; font-size: .85rem; }}
pre {{ background: #161b22; padding: .6rem; overflow-x: auto; font-size: .8rem; }}
a {{ color: #58a6ff; }}
</style></head>
<body>
<h1>monkey_dust — стан репо</h1>
<p class="stamp">Згенеровано: {now} · дані по кожному блоку датовані окремо, дивись мітки нижче</p>

<h2>Git (3 репо)</h2>
<table><tr><th>репо</th><th>гілка</th><th>dirty</th><th>приклад</th><th>ahead/behind</th></tr>{git_html}</table>
<h3>Submodule ref drift</h3>
<table><tr><th>шлях</th><th>записано в main</th><th>реальний HEAD</th><th>статус</th></tr>{drift_html}</table>

<h2>Build</h2>
<table><tr><th>ціль</th><th>стан</th><th>час білда</th></tr>{build_html}</table>

<h2>Тести</h2>
{tests_html}

<h2>QA</h2>
{qa_html}

<h2>Doctor</h2>
{doctor_html}

<h2>Issues</h2>
{issues_html}

<h2>Бюджети (§13 CLAUDE_CONSTITUTION.md)</h2>
{budgets_html}

</body></html>
"""


def main() -> int:
    git_rows, sm_drift = gather_git()
    build_rows = gather_build()
    tests = gather_tests()
    qa = gather_qa()
    doctor = gather_doctor()
    issues = gather_issues()
    budgets = gather_budgets()

    out = render(git_rows, sm_drift, build_rows, tests, qa, doctor, issues, budgets)
    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUT_PATH.write_text(out, encoding="utf-8")
    print(f"[md_dash] wrote {OUT_PATH}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
