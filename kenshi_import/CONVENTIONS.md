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

## Наступний крок (Фаза 1, не почато)

Vertical slice: впізнаваний пропс з відомим розміщенням (будівля з
leveldata) → GLB → `PropMesh::LoadGLB` у рушії за АБСОЛЮТНИМИ
координатами → скріншот-парність з грою. Осі/масштаб з `wall.mesh`
(10×10×1м, правдоподібно для будівельного модуля) виглядають
розумно, але формально не звірені зі скріншотом гри — це і є Фаза 1.
