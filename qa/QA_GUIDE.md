# monkey_dust QA System — Керівництво

> **ОБОВ'ЯЗКОВО читати перед будь-яким виправленням багу або доданням фічі.**
> Якщо проблема не покрита існуючими тестами — спочатку напиши тест, потім виправляй.

---

## Розташування

```
tools/qa/
├── qa_run.sh          — Головний runner: build → capture → report
├── qa_report.py       — Генератор QA звіту (аналіз frames)
├── captures/          — Записані gameplay сесії (video + frames)
│   └── YYYYMMDD_HHMMSS/
│       ├── demo.mkv   — Повний запис
│       └── frames/    — PNG фрейми (221 шт. @ 10fps)
├── reports/           — Згенеровані звіти (gitignored)
│   └── YYYYMMDD_HHMMSS/
│       ├── report.html           — Основний звіт (відкрити в браузері)
│       ├── report.md             — Markdown версія
│       ├── anomalies.json        — Машино-читаний список аномалій
│       ├── *_preview.gif         — Animated preview 4fps
│       ├── *_contact.png         — Contact sheet (всі кадри)
│       ├── *_delta.png           — Frame delta chart (full + NPC zone)
│       ├── *_tracking.png        — NPC tracking charts
│       └── annotated/            — Кадри з анотованими аномаліями
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
