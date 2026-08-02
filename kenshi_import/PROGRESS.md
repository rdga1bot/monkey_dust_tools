# PROGRESS.md — журнал ASSET_PIPELINE_MASTER_PROMPT.md

## 2026-08-02 — Фаза 0 ЗАВЕРШЕНА: середовище + плагін живо перевірені

Blender 5.2.0 LTS встановлено нативно (не flatpak — пісочниця
структурно блокує `/usr`, звідки береться `OgreXMLConverter`).
`Kenshi_IO_Continued` (codeberg.org/Kindrad) встановлено й
перевірено двома точковими сумісність-патчами (Python 3.13 / Blender
4.1+ API зміни, деталі й обґрунтування — `CONVENTIONS.md`). Критерій
Фази 0 виконано: `wall.mesh` імпортовано headless без помилок,
експортовано в GLB.

Не почато: Фаза 1 (vertical slice з реальним leveldata-об'єктом +
скріншот-парність у рушії).
