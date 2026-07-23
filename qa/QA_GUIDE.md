# monkey_dust QA System — Керівництво

> **ОБОВ'ЯЗКОВО читати перед будь-яким виправленням багу або доданням фічі.**
> Якщо проблема не покрита існуючими тестами — спочатку напиши тест, потім виправляй.

---

## Розташування

```
tools/qa/
├── qa_run.sh          — Головний runner: build → capture (MD_QA_STATE) → report → BDD
├── qa_report.py       — Генератор QA звіту (аналіз frames + JSONL)
├── qa_regression.py   — Visual regression: baseline / compare / HTML scrubber
├── qa_bdd.py          — BDD Gherkin runner (7 built-in steps)
├── qa_perf_baseline.py — Perf regression: parse FrameStats [PERF] lines / baseline / compare
├── qa_validation_check.py — Vulkan validation-layer log scan (VUID-*/ERROR)
├── features/          — .feature files
│   ├── npc_behavior.feature
│   ├── rendering.feature
│   └── player.feature
├── captures/          — Записані gameplay сесії (gitignored)
│   └── YYYYMMDD_HHMMSS/
│       ├── frames/           — PNG фрейми (@ 10fps)
│       └── qa_state.jsonl    — JSONL log: NPC pos/vel per logic tick
├── baselines/         — Еталонні кадри для regression
│   └── YYYYMMDD_HHMMSS/     — скопійовані frames + meta.json
├── reports/           — Згенеровані звіти (gitignored)
│   └── YYYYMMDD_HHMMSS/
│       ├── report.html           — Основний звіт (відкрити в браузері)
│       ├── report.md             — Markdown версія
│       ├── anomalies.json        — Машино-читаний список аномалій
│       ├── *_preview.gif         — Animated preview 4fps
│       ├── *_contact.png         — Contact sheet (всі кадри)
│       ├── *_delta.png           — Frame delta chart (full + NPC zone)
│       ├── *_tracking.png        — NPC tracking charts
│       ├── annotated/            — Кадри з анотованими аномаліями
│       └── regression/           — Visual regression дiffs + report.html
└── QA_GUIDE.md        — Цей файл
```

Unit tests: `tests/` (1557+ тестів, C++ googletest)

---

## Як запустити

### Повний QA цикл
```bash
bash tools/qa/qa_run.sh              # build + run game + report
bash tools/qa/qa_run.sh --no-build   # пропустити збірку
bash tools/qa/qa_run.sh --open       # відкрити HTML у браузері
```

### Тільки аналіз існуючого capture
```bash
python3 tools/qa/qa_report.py                          # latest capture
python3 tools/qa/qa_report.py --capture 20260531_030631
python3 tools/qa/qa_report.py --no-tests               # без unit tests
python3 tools/qa/qa_report.py --open                   # відкрити HTML
```

### BDD перевірки
```bash
python3 tools/qa/qa_bdd.py tools/qa/features/             # всі .feature файли, latest capture
python3 tools/qa/qa_bdd.py tools/qa/features/ --capture 20260531_030631
python3 tools/qa/qa_bdd.py tools/qa/features/npc_behavior.feature --capture latest
```

### Visual Regression
```bash
# 1. Зберегти еталон з «хорошого» capture:
python3 tools/qa/qa_regression.py --baseline 20260531_030631

# 2. Порівняти новий capture проти еталону:
python3 tools/qa/qa_regression.py --compare 20260601_120000

# 3. Вказати конкретний baseline:
python3 tools/qa/qa_regression.py --compare 20260601_120000 --baseline-id 20260531_030631

# 4. Переглянути всі baselines і captures:
python3 tools/qa/qa_regression.py --list
```

HTML звіт: `tools/qa/reports/TIMESTAMP/regression/report.html` — drag-to-reveal scrubber.

### Perf regression

`FrameStats` (engine/include/monkey_dust/platform/frame_stats.h) друкує
`[PERF] N FPS | NPCs=N | Name=avgms(maxms) ...` кожні 5с у stderr —
`qa_run.sh` тепер завжди зберігає повний stdout+stderr гри в
`<capture>/game_stdout.log` (раніше губилось: headless-гілка обрізала
до `tail -5`, wmctrl-гілка не редіректила взагалі).

```bash
# Порівняти capture проти збереженого бейзліну:
python3 tools/qa/qa_perf_baseline.py --check                       # latest capture
python3 tools/qa/qa_perf_baseline.py --check --capture 20260722_235959
python3 tools/qa/qa_perf_baseline.py --check --threshold 15         # свій поріг (default 10%)

# Зберегти capture як новий бейзлін (після підтвердження що продуктивність ОК):
python3 tools/qa/qa_perf_baseline.py --update-baseline
```

Бейзлін: `tools/qa/baselines/perf_baseline.json` (усереднення по всіх
5-секундних інтервалах capture, крім першого — warmup: компіляція
шейдерів/pipeline ще триває в перші секунди після старту).
Наразі підключено в `qa_run.sh` як **non-blocking** (не валить весь QA
прогін) — поки бейзлін і поріг не підтверджені кількома реальними
прогонами.

### Vulkan validation layers

`MD_GPU_VALIDATION` (engine/CMakeLists.txt) вмикається автоматично лише
для `CMAKE_BUILD_TYPE=Debug` — реальний per-pipeline compile overhead
(задокументовано: ~4.5с стопор на `terrain_forward` окремо), тому це
**НЕ** частина дефолтного швидкого `qa_run.sh`.

```bash
bash tools/qa/qa_run.sh --validation   # Debug build у temp_/build_qa_validation,
                                        # окремо від звичайного build/
```

Це збирає в **окрему** директорію (`temp_/build_qa_validation` —
конвенція проєкту: `temp_/` для тимчасових/експериментальних збірок),
не займаючи основний `build/`. Після прогону — автоматичний скан
`<capture>/game_stdout.log` через `qa_validation_check.py`: шукає
`VUID-*`/`Validation Error` (ERROR-рівень — валить прогін), WARNING —
тільки логується.

```bash
# Вручну, проти вже наявного validation-capture:
python3 tools/qa/qa_validation_check.py --capture 20260722_235959
```

### Unit tests
```bash
ninja -C build md_tests && ./build/tests/md_tests
ninja -C build md_behavior_tests && ./build/tests/md_behavior_tests
```

---

## Що аналізує QA report

### Per-frame аномалії (з 221 фреймів)

| Тип | Код | Опис | Поріг |
|-----|-----|------|-------|
| ❄️ Freeze | `freeze` | Вся сцена заморожена між кадрами | delta < 0.15% |
| ⚡ Jump | `jump` | Різкий стрибок камери/NPC | delta > 30% |
| 🫨 Twitch | `twitch` | Тремтіння (3+ кадри oscillation) | 2-30% |
| 🌑 Sky missing | `sky_missing` | Небо зникло (blue channel < 10%) | top 25% of frame |
| 👻 NPC transparent | `npc_transparent` | NPC невидимий/прозорий | brightness < 20% |
| 🎭 NPC freeze | `npc_freeze` | NPC не рухається, камера рухається | NPC delta < 0.3% |
| 🧊 Pose freeze | `pose_freeze` | Поза NPC не змінюється N+ кадрів | Bhattacharyya < 0.03 |

### Артефакти звіту

- **GIF preview** — переглянути motion без відео
- **Delta chart** — 2 панелі: full frame + NPC zone окремо
- **Tracking chart** — позиція NPC у часі + 2D trajectory + displacement
- **Annotated frames** — PNG для кожної аномалії з bbox (синій) та NPC centroid (зелений)
- **Contact sheet** — всі 221 кадрів в одній PNG

---

## Правило: Тест перед фіксом

**ЗАВЖДИ перед виправленням багу або доданням фічі:**

1. **Визнач яку аномалію детектить (або має детектити) QA система**
2. **Якщо аномалія вже детектується** → запусти `qa_report.py` → переконайся що вона в `anomalies.json` → виправляй → перезапусти → переконайся що аномалія зникла
3. **Якщо аномалія НЕ детектується** → спочатку додай detection до `qa_report.py` → потім виправляй

### Приклади

| Проблема | Де тест | Що перевіряти |
|----------|---------|---------------|
| NPC прозорий | `npc_transparent` аномалія | `anomalies.json` не містить npc_transparent |
| Анімація тремтить | `pose_freeze` + `twitch` | pose_freeze events = 0 |
| Небо зникає | `sky_missing` | sky_missing_frames = 0 |
| NPC не рухається | `npc_freeze` + tracking chart | displacement chart має ненульові значення |
| Camera jump | `jump` | jump_pairs = 0 |

### Для C++ unit tests (`tests/`)

Якщо баг у логіці (AI, physics, animation blend):
```cpp
// Новий тест у відповідному файлі tests/test_md_*.cpp
TEST(AnimBlend, IdleDoesNotTwitch) {
    // ... відтворити умову яка викликала тремтіння
    // ... перевірити що bt залишається <= 0.01 в idle
}
```

---

## Capture workflow

Game автоматично запускає `scripts/game_capture.py` при старті:
1. Чекає вікно "monkey_dust" (через wmctrl)
2. Записує через ffmpeg x11grab → `captures/TIMESTAMP/demo.mkv`
3. Нарізає фрейми → `captures/TIMESTAMP/frames/NNNN.png` (10fps)
4. При виході гри — зупиняє запис

QA camera (в грі) робить orbit навколо гравця:
- Горизонтальний 360° orbit
- Вертикальне качання (overhead → low angle)
- Тривалість: ~22 секунди (221 frames @ 10fps)

---

## Log-based QA (qa_state.jsonl)

Гра автоматично пише JSONL коли встановлено `MD_QA_STATE`:

```
{"tick":1,"t":0.100,"px":0.0,"pz":5.0,"npcs":[{"s":1,"x":10.5,"z":20.3,"vx":1.5,"vz":0.0,"mv":1},...]}
```

- **tick** — лічильник logic ticks (10 TPS)
- **t** — game time (секунди)
- **px/pz** — позиція гравця
- **npcs** — всі NPC в радіусі 150m: slot, x, z, vx (desired_vel_x), vz, mv (is_moving)

`qa_run.sh` автоматично встановлює `MD_QA_STATE=captures/TIMESTAMP/qa_state.jsonl`.
BDD steps читають цей файл для точних перевірок (без image heuristics).

---

## BDD — Gherkin тести

Feature файли в `tools/qa/features/*.feature`. Синтаксис:

```gherkin
Feature: NPC behavior stability

  Background:
    Given capture "latest"

  Scenario: NPCs must not freeze
    Then no NPC freezes for more than 10 ticks

  Scenario: No teleportation
    Then no NPC teleports more than 15m in one tick
```

### Вбудовані steps

| Step | Тип | Джерело |
|------|-----|---------|
| `capture "ID\|latest"` | setup | — |
| `no NPC freezes for more than N ticks` | assert | JSONL |
| `no NPC teleports more than Xm in one tick` | assert | JSONL |
| `at least N NPCs are present` | assert | JSONL |
| `player position changes at least Xm per second` | assert | JSONL |
| `sky is visible in [all\|N%] frames` | assert | PNG |
| `NPCs are visible in at least N% of frames` | assert | PNG |

Log-based steps (`JSONL`) точніші. Image steps (`PNG`) — fallback якщо JSONL недоступний.

### Додати новий step

```python
# В qa_bdd.py:
@step(r'no entity spawns outside (\d+)m radius')
def step_spawn_radius(m):
    max_r = float(m.group(1))
    if not ctx.qa_ticks:
        return Skip("no qa_state.jsonl")
    # ... перевірка
    return Pass("all spawns within radius") # або Fail("...")
```

---

## Visual Regression

Порівнює captures по пікселях. Корисно для:
- Детекції рендер-регресій після змін шейдерів
- Перевірки що фікс не зламав візуальний результат

### Workflow

```bash
# 1. Після підтвердження що capture виглядає правильно:
python3 tools/qa/qa_regression.py --baseline 20260531_030631

# 2. Після кожного наступного capture:
python3 tools/qa/qa_regression.py --compare 20260601_120000
# → PASS якщо < 2% пікселів змінилось у кожному кадрі
# → FAIL + HTML звіт якщо більше
```

### HTML звіт — drag-to-reveal scrubber

Відкрий `reports/TIMESTAMP/regression/report.html`:
- Ліворуч — таблиця кадрів з % зміни
- По центру — drag-to-reveal: тягни мишею щоб побачити before/after
- Праворуч — diff thumbnail (червоний = змінені пікселі)

### Пороги

| Параметр | Значення | Опис |
|----------|----------|------|
| `DIFF_THRESHOLD` | 8/255 | Ігнорує sub-pixel antialiasing шум |
| `FAIL_PCT` | 2.0% | Кадр вважається FAIL якщо > 2% пікселів змінено |

---

## Відомі обмеження

- NPC centroid detection калібрований для помаранчевого пустельного terrain.
  Для інших biome може потребувати коригування порогів у `npc_centroid()`.
- Pose freeze detector може давати false positives якщо NPC в idle breath стані
  де мінімальний рух (Bhattacharyya threshold = 0.03).
- Sky detection не працює коли камера дивиться вниз (overhead view) — очікувана поведінка.

---

## Розширення QA

Щоб додати новий тип аномалії:

```python
# В qa_report.py:
# 1. Додати константу порогу
MY_THRESHOLD = 5.0

# 2. Додати detection в analyze_capture()
if my_condition(frame):
    analysis.anomalies.append(FrameAnomaly(
        fnum, -1, "my_anomaly", value, "опис"
    ))

# 3. Додати іконку в severity_icon()
"my_anomaly": "🔥"

# 4. Додати рекомендацію в gen_markdown() recs
if count > threshold:
    recs.append("...")
```
