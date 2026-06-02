#!/usr/bin/env python3
"""Generate monkey_dust_docs.html — unified project documentation.

Output: engine/monkey_dust_docs.html  (served by GitHub Pages)
        monkey_dust_docs.html          (local copy)

Usage:
    python3 tools/gen_docs.py
"""
import markdown, os, datetime, re, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DOCS = [
    ("README.md",               "Game",           "Overview, stack, build instructions, feature list"),
    ("engine/README.md",        "Engine",         "GPU HAL, AI/BT VM, ECS, physics, animation, audio"),
    ("tools/README.md",         "Tools",          "Editor, hot-reload, QA, shader pipeline"),
    ("CLAUDE_STATE.md",         "State",          "Current milestones and recent changes"),
    ("CLAUDE.md",               "Config & Rules", "Critical rules, GPU debug protocol, hardware constraints"),
    ("CLAUDE_CONSTITUTION.md",  "Constitution",   "Project constitution, QA protocol, GPU debug guide"),
]

CSS = """
:root {
  --bg:      #0d1117;
  --bg2:     #161b22;
  --bg3:     #1c2128;
  --border:  #30363d;
  --text:    #c9d1d9;
  --text2:   #8b949e;
  --blue:    #58a6ff;
  --blue2:   #79c0ff;
  --green:   #56d364;
  --orange:  #f0883e;
  --red:     #ff7b72;
  --yellow:  #e3b341;
}
* { box-sizing: border-box; margin: 0; padding: 0; }
html { scroll-behavior: smooth; }
body {
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
  background: var(--bg); color: var(--text);
  font-size: 15px; line-height: 1.6;
}
.layout { display: flex; min-height: 100vh; }

/* ── Sidebar ── */
nav {
  width: 240px; min-width: 240px;
  background: var(--bg2); border-right: 1px solid var(--border);
  padding: 20px 0; position: sticky; top: 0; height: 100vh;
  overflow-y: auto; flex-shrink: 0;
}
nav .logo {
  padding: 0 16px 16px; border-bottom: 1px solid var(--border);
  margin-bottom: 12px;
}
nav .logo h2 { color: var(--blue); font-size: 15px; }
nav .logo p  { color: var(--text2); font-size: 12px; margin-top: 3px; }
nav a {
  display: block; padding: 7px 16px; color: var(--text2);
  text-decoration: none; font-size: 13px; border-left: 3px solid transparent;
  transition: all .15s;
}
nav a:hover  { color: var(--text); background: var(--bg3); }
nav a.active { color: var(--blue); border-left-color: var(--blue); background: var(--bg3); }
nav .nav-desc { font-size: 11px; color: var(--text2); padding: 0 16px 4px; opacity: .7; }

/* ── Content ── */
main { flex: 1; max-width: 960px; padding: 32px 40px; min-width: 0; }
section { margin-bottom: 60px; padding-bottom: 40px; border-bottom: 1px solid var(--border); }
section:last-child { border-bottom: none; }

/* ── Typography ── */
h1 { font-size: 24px; color: #e6edf3; margin: 0 0 16px; padding-bottom: 8px; border-bottom: 2px solid var(--border); }
h2 { font-size: 20px; color: var(--blue); margin: 28px 0 12px; padding-bottom: 6px; border-bottom: 1px solid var(--border); }
h3 { font-size: 16px; color: var(--blue2); margin: 20px 0 8px; }
h4 { font-size: 14px; color: #adbac7; margin: 14px 0 6px; }
p  { margin-bottom: 12px; }
a  { color: var(--blue); text-decoration: none; }
a:hover { text-decoration: underline; }
ul, ol { padding-left: 24px; margin-bottom: 12px; }
li { margin-bottom: 4px; }
strong { color: #e6edf3; }
em { color: var(--text2); }

/* ── Code ── */
code {
  font-family: "JetBrains Mono", "Fira Code", "Consolas", monospace;
  font-size: 13px; background: var(--bg2);
  padding: 2px 6px; border-radius: 4px; color: var(--orange);
}
pre {
  background: var(--bg2); border: 1px solid var(--border);
  border-radius: 6px; padding: 16px; overflow-x: auto;
  margin: 12px 0; line-height: 1.5;
}
pre code {
  background: none; padding: 0; color: #e6edf3; font-size: 13px;
}

/* ── Tables ── */
table { border-collapse: collapse; width: 100%; margin: 12px 0; font-size: 13px; }
th { background: var(--bg3); color: var(--blue2); font-weight: 600;
     border: 1px solid var(--border); padding: 8px 12px; text-align: left; }
td { border: 1px solid var(--border); padding: 7px 12px; }
tr:nth-child(even) td { background: #0a0e14; }

/* ── Blockquote ── */
blockquote {
  border-left: 3px solid var(--blue); margin: 12px 0;
  padding: 8px 16px; background: var(--bg3); border-radius: 0 4px 4px 0;
  color: var(--text2);
}

/* ── Header card ── */
.header-card {
  background: var(--bg2); border: 1px solid var(--border);
  border-radius: 8px; padding: 20px 24px; margin-bottom: 32px;
}
.header-card h1 { border: none; margin: 0 0 8px; font-size: 22px; }
.badges { display: flex; flex-wrap: wrap; gap: 6px; margin: 10px 0; }
.badge {
  background: #1f6feb33; border: 1px solid #1f6feb66;
  color: var(--blue); padding: 3px 10px; border-radius: 20px; font-size: 12px;
}
.meta { color: var(--text2); font-size: 12px; margin-top: 8px; }
.section-tag {
  display: inline-block; background: var(--bg3); border: 1px solid var(--border);
  color: var(--text2); font-size: 11px; padding: 2px 8px; border-radius: 4px;
  margin-bottom: 8px; vertical-align: middle; margin-left: 8px;
}

/* ── Scroll spy active ── */
@media (max-width: 768px) {
  nav { display: none; }
  main { padding: 20px; }
}
"""

SCROLL_SPY_JS = """
<script>
const secs = document.querySelectorAll('section[id]');
const links = document.querySelectorAll('nav a[href^="#"]');
const obs = new IntersectionObserver(entries => {
  entries.forEach(e => {
    if (e.isIntersecting) {
      links.forEach(l => l.classList.remove('active'));
      const a = document.querySelector('nav a[href="#' + e.target.id + '"]');
      if (a) a.classList.add('active');
    }
  });
}, { rootMargin: '-20% 0px -70% 0px' });
secs.forEach(s => obs.observe(s));
</script>
"""

def anchor(fname):
    return re.sub(r'[/.]', '-', fname)

def nav_html(docs, now):
    lines = ['<nav>',
             '<div class="logo">',
             '<h2>monkey_dust</h2>',
             '<p>Technical Documentation</p>',
             '<p style="font-size:11px;margin-top:6px;color:#58a6ff">v25.0 &middot; ' + now + '</p>',
             '</div>']
    for fname, title, desc in docs:
        lines.append('<span class="nav-desc">{}</span>'.format(desc[:36] + '…' if len(desc) > 36 else desc))
        lines.append('<a href="#{}">{}</a>'.format(anchor(fname), title))
    lines.append('</nav>')
    return '\n'.join(lines)

def header_html(now):
    badges = ['C++17', 'SDL3+SDL_GPU', 'Vulkan 1.1', 'EnTT ECS',
              'Intel HD 520', '60 FPS', '1557 tests', 'RenderDoc']
    badge_html = ''.join('<span class="badge">{}</span>'.format(b) for b in badges)
    return (
        '<div class="header-card">'
        '<h1>monkey_dust — Technical Documentation</h1>'
        '<div class="badges">{}</div>'
        '<p class="meta">Generated {} &middot; FLARE-inspired open-world RPG sandbox &middot; Solo project</p>'
        '</div>'
    ).format(badge_html, now)

def build():
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")

    md_ext = ['tables', 'fenced_code']
    sections = []
    for fname, title, desc in DOCS:
        path = os.path.join(REPO, fname)
        if not os.path.exists(path):
            print('  SKIP:', fname)
            continue
        with open(path, encoding='utf-8') as f:
            raw = f.read()
        body = markdown.markdown(raw, extensions=md_ext)
        sections.append(
            '<section id="{}">'
            '<h2>{}<span class="section-tag">{}</span></h2>'
            '{}'
            '</section>'.format(anchor(fname), title, fname, body)
        )

    html = (
        '<!DOCTYPE html>\n<html lang="en">\n<head>\n'
        '<meta charset="UTF-8">\n'
        '<meta name="viewport" content="width=device-width, initial-scale=1">\n'
        '<title>monkey_dust — Technical Documentation v25.0</title>\n'
        '<style>{}</style>\n'
        '</head>\n<body>\n'
        '<div class="layout">\n'
        '{}\n'               # nav
        '<main>\n'
        '{}\n'               # header
        '{}\n'               # sections
        '</main>\n'
        '</div>\n'
        '{}\n'               # scroll spy
        '</body>\n</html>'
    ).format(
        CSS,
        nav_html(DOCS, now),
        header_html(now),
        '\n'.join(sections),
        SCROLL_SPY_JS,
    )
    return html

if __name__ == '__main__':
    html = build()
    # Primary: engine/ (served by GitHub Pages)
    engine_out = os.path.join(REPO, 'engine', 'monkey_dust_docs.html')
    with open(engine_out, 'w', encoding='utf-8') as f:
        f.write(html)
    # Secondary: repo root (local preview)
    local_out = os.path.join(REPO, 'monkey_dust_docs.html')
    with open(local_out, 'w', encoding='utf-8') as f:
        f.write(html)
    size = len(html) // 1024
    print('engine/ → {} KB'.format(size))
    print('root/   → {} KB'.format(size))
    print('Open:  xdg-open {}'.format(local_out))
