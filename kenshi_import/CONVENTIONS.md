# CONVENTIONS.md — Kenshi `.mesh` → GLB конвеєр

Журнал технічних рішень для `ASSET_PIPELINE_MASTER_PROMPT.md`. Кожен факт
тут перевірений живим запуском, не здогаданий.

## Фаза 0 — середовище (ЗАВЕРШЕНО 2026-08-02)

**Blender:** 5.2.0 LTS, **нативний пакет** (`pacman -S blender`), НЕ
flatpak. Причина: flatpak-версія структурно (не через дозволи) не може
відкрити `/usr/*` — `OgreXMLConverter` (нижче) недосяжний з пісочниці
("Path '/usr' is reserved by Flatpak", підтверджено живою спробою
`flatpak override --filesystem=/usr/bin/OgreXMLConverter:ro`, яка
пройшла дозвіл, але сам flatpak відмовив монтувати).

**Плагін:** `Kenshi_IO_Continued` (codeberg.org/Kindrad,
`bl_idname` імпорту = `import_scene.mesh`, НЕ `ogre_import` — `bpy.ops`
динамічно резолвить будь-яке ім'я через `hasattr`, це НЕ підтверджує
реальну реєстрацію; єдиний надійний тест — реальний виклик).
Заявлена сумісність `bl_info["blender"] = (3, 6, 0)`; РЕАЛЬНО
підтверджено робочим на Blender 5.2 (Python 3.13) **лише після двох
точкових патчів**:

1. `__init__.py`: `import imp` → `import importlib as imp` (модуль
   `imp` видалено в Python 3.12+; використовувався лише для
   `imp.reload()` у dev-hot-reload блоці — `importlib.reload` ідентична
   пряма заміна для цієї єдиної функції).
2. `OgreImport.py:1295`: `me.use_auto_smooth = True` — обгорнуто в
   `if hasattr(me, 'use_auto_smooth')`. Властивість видалена в
   Blender 4.1+ (custom split normals застосовуються без неї відтоді);
   без цього фікса ІМПОРТ ГЕОМЕТРІЇ ВЖЕ БУВ УСПІШНИЙ до цього рядка —
   лише косметичний smoothing-крок падав, залишаючи валідний mesh-об'єкт
   у сцені (експорт GLB попри це проходив, але з непійманим
   виключенням — недопустимо для batch-конвеєра, тому фікс обов'язковий).

Патчений форк лежить поза репо (третьосторонній код Kenshi-спільноти,
не наш): `/tmp/.../scratchpad/blender_addons/Kenshi_IO_Continued/`
(сесійний scratch). **TODO перед Фазою 3:** запакувати патчений форк
у постійне, версійоване місце (напр. `tools/kenshi_import/vendor/` або
окремий git-форк з нашими двома патч-комітами) — scratch-каталог
gitignored і зникає між сесіями.

**XML-конвертер:** `/usr/bin/OgreXMLConverter` (пакет `ogre-14.5.2`,
вже документований у `CLAUDE.md` для character-mesh конвеєра) — той
самий бінарник, НЕ Wine-обгортка плагіна (`XML_1_29/OgreXMLConverter.bash`,
яка викликає `wine` і не працює без нього встановленого). Виклик
оператора: `xml_converter='custom', custom_xml_converter='/usr/bin/OgreXMLConverter'`.

**Реальний виклик оператора (недокументована деталь):**
`bpy.ops.import_scene.mesh` успадковує `ImportHelper` з
`files: CollectionProperty` (batch-режим), БЕЗ окремого `directory`.
Правильний виклик:
```python
bpy.ops.import_scene.mesh(
    filepath=os.path.dirname(mesh_path) + os.sep,  # ДИРЕКТОРІЯ, не файл
    files=[{"name": os.path.basename(mesh_path)}],
    xml_converter='custom',
    custom_xml_converter='/usr/bin/OgreXMLConverter',
)
```
Виклик з `filepath=<повний шлях до файлу>` (без `files=`) мовчки
резолвиться в директорію без жодного файлу для обробки → внутрішній
`UnboundLocalError` в аддоні (некоректна обробка порожнього списку,
не наша проблема, але треба уникати цього виклику).

## Фаза 0 — критерій ✅

`wall.mesh` (`tmp_/kenshi/data/buildings/wall.mesh`, найменший
статичний non-skeleton меш, 1343 байти) імпортовано в headless
Blender без помилок (`IMPORT_RESULT: {'FINISHED'}`) — 8 вертексів
(дедуп з 24), 10×10×1м, і успішно експортовано в GLB (`EXPORT_OK`,
gltf 2.0 exporter).

## Фаза 1 — vertical slice (ЗАВЕРШЕНО 2026-08-02)

`wall.mesh` (Blender-конвертований GLB з Фази 0) розміщено в грі через
РЕАЛЬНИЙ `FeatureScatterSystem` (той самий пайплайн, що вантажить 113
вже сконвертованих scatter-об'єктів) — тимчасовий рядок додано в
`game/data/features_scatter.txt` (gitignored/regenerable, прибрано
після перевірки), меш скопійовано в `game/data/props/features/`.
`[PropMesh] Loaded 24 verts / 36 idx (u16): .../wall.glb` — завантажено
без жодної помилки, той самий лог-шлях, що й для 113 реальних мешів.

**Знайдена й задокументована пастка (critical для будь-якого майбутнього
scenario-скрипта):** `md.set_camera_pose(x,y,z,...)` очікує ЛОКАЛЬНІ
координати (`SceneRender::tnoff_x/z`-прив'язані), НЕ абсолютні
Kenshi-метри — на відміну від `features_scatter.txt`, який зберігає
placement САМЕ в абсолютних Kenshi-метрах (`FeatureScatterSystem::Draw`
явно використовує `g_abs_cam`, коментар у `npc_render.cpp:1495-1507`
пояснює чому). Передача абсолютних координат у `set_camera_pose` мовчки
телепортує камеру в порожнє (нестрімлене) місце — рушій рендерить
суцільний блюр/білий кадр БЕЗ жодної помилки в лозі (діагностовано
методом виключення: та сама біла картинка відтворилась і на давньому,
раніше вже перевіреному `probe_pillar_topdown.lua`, отже це не
регресія Фази 1, а існуюча пастка API). Конвертація: `md.world_to_local()`
АБО (простіше, як тут) — прочитати `md.zone_info()` (`tnoff_x/z`,
`zone_ox/oz`) і рахувати:
```
local_center   = (tnoff_x + 4*CHUNK_SIZE, tnoff_z + 4*CHUNK_SIZE)   -- камера/terrain_height
world_center   = (zone_ox*CHUNK_SIZE + 4*CHUNK_SIZE,
                   zone_oz*CHUNK_SIZE + 4*CHUNK_SIZE)                -- features_scatter.txt
```
(CHUNK_SIZE=460.8, TNKN=9 → half-window=4*CHUNK_SIZE; формула — той
самий "найрівніше місце біля spawn" підхід, що вже існує в
`tests/scenarios/probe_clutter_diag.lua`, лише без пошуку найрівнішої
точки — тут узято сам центр вікна).

**Результат** (`phase1_verify.png`, той самий каталог): об'єкт
рендериться як плаский коричневий планшет 10×10×1м, СТОЇТЬ на своїй
геометричній основі (z=0 low в mesh-просторі — не перевернутий, не
дзеркальний), масштаб правдоподібний — постать гравця (spawn
опинився поруч) видно поруч для порівняння, зростом суттєво менше
за 10-метровий планшет, як і очікується. Реальні гірські схили
Kenshi-терейну навколо рендеряться коректно (RE-фактор: локальна
точка виявилась біля крутого перевалу, не рівнина).

**Не перевірено в Фазі 1 (свідомо, за планом):** текстура/матеріал —
аддон створює заглушку-матеріал без текстур; це Фаза 2.

**ВИПРАВЛЕННЯ заднім числом (знайдено у Фазі 2, важливо):** Фаза 1
експортувала `wall.mesh` БЕЗ масштабування — `phase1_verify.png`
показує об'єкт у 10 разів БІЛЬШИЙ, ніж насправді. Реальна конвенція,
вже перевірена й задокументована в `tools/md_convert_features.py`
(коментар: "All meshes use --scale 0.1 ... verified against two real
samples ... same as the terrain heightmap's ... convention"): **1
Kenshi world unit = 0.1м**. Я цього не застосував у Фазі 1, бо не
знав про існування цієї вже встановленої конвенції в іншому скрипті
цього ж репо. Виправлено в Фазі 2 (`blender_convert.py`'s
`WORLD_SCALE = 0.1`, застосовується через `obj.scale` +
`transform_apply` ПЕРЕД експортом, щоб GLB вже містив правильні
абсолютні розміри без потреби в runtime-множнику). `phase2_verify.png`
підтверджує коректний результат: той самий `wall.mesh`, тепер 1×1×0.1м
замість 10×10×1м, поруч з постаттю гравця — правдоподібний масштаб
підлогової плитки/дверного порогу.

## Фаза 2 — матеріали (ЗАВЕРШЕНО 2026-08-02)

**Реальна структура Kenshi-матеріалів (перевірено на живих даних, НЕ
здогад):** кожен статичний `.mesh`-сабмеш посилається на матеріал
`"StaticObject"` НАПРЯМУ (перевірено через OgreXMLConverter XML-вивід
на `wall.mesh` і `moor01-door.mesh` — обидва: `material="StaticObject"`).
`StaticObject` (`tmp_/kenshi/data/materials/deferred/objects.material`)
— базовий deferred-шейдерний клас з 3 іменованими texture_unit:
`diffuseMap`, `normalMap`, `metalnessMap` — але з ЗАГЛУШКОВИМИ
текстурами (`black.dds`/`flat.dds`/`black.dds`). Жоден `.material`
скрипт НЕ перевизначає ці текстури per-instance — реальне визначення
"який меш яку текстуру отримує" відбувається десь у Kenshi engine
(FCS/рушій), недоступному з цих даних.

**Робоче рішення — folder heuristic** (`find_textures.py`,
задокументовано в docstring файлу): шукає `*_DIF.dds`/`*_NML.dds` (або
bare+`_n.dds`) файли в ТІЙ САМІЙ директорії, що й меш; при кількох
кандидатах — обирає найближчий за `difflib`-схожістю імені до
basename меша; поріг `MIN_SIMILARITY=0.25` — нижче нього НЕ вгадує
(краще без текстури, ніж хибна: `Split-Rail_Fence01.mesh` без порогу
мовчки хапав НЕспоріднений `watertower.dds`, лише тому що це єдиний
diffuse-файл у тій директорії).

**Виміряний результат на 5 типах мешів** (`blender_convert.py`,
живий запуск): building(`wall.mesh`)✓, door(`moor01-door.mesh`)✓,
rock(`Rock_MenhirType_01.mesh`)✓, tree(`Big4Seed_Tree1.mesh`)✓,
decor(`Street_Lamp01.mesh`)✓ — усі знайшли коректну diffuse+normal
пару. fence(`Split-Rail_Fence01.mesh`) — 0 кандидатів у власній
директорії (`buildings/misc/` містить лише `market`/`watertower`
текстури, стосунку до паркану не мають) — чесно залишено без текстури
(StaticObject-заглушка), не вгадано. **5/6 = 83% успіх на цій вибірці**
— реальна метрика для повного batch-прогону (Фаза 3) буде іншою
(різні директорії/типи мешів), вимірювати окремо, не екстраполювати
сліпо.

`assign_textures()` (`blender_convert.py`) створює Principled BSDF
матеріал: diffuse → Base Color, normal (colorspace Non-Color) → Normal
Map node → BSDF Normal. Живий скріншот (`phase2_verify.png`)
підтверджує: `wall.mesh` рендериться з РЕАЛЬНОЮ ATLAS1.dds текстурою
(кам'яний/цегляний візерунок), не сірою заглушкою.

## Наступний крок (Фаза 3)

Batch-конвертер для одного цілого міста: маніфест з leveldata/features
для цього міста, per-file error log (не падати на одному поганому
меші), summary report (converted/skipped/errors, розмір GLB vs .mesh,
час конвертації). Критерій: ≥95% статичних мешів міста конвертуються
без помилки.
