#!/usr/bin/env python3
"""Renders the codelab's claat-format Markdown as a codelab web page.

    python3 codelab/build_site.py <output-dir>

Writes <output-dir>/<id>/index.html in the same structure `claat export -f html`
produces (google-codelab elements), using Google's hosted codelab scripts, plus
<output-dir>/index.html, which forwards to the codelab. Python standard library
only. It understands the Markdown subset the codelab uses: headings, paragraphs,
bullet and numbered lists (with indented content), fenced code, `aside positive`
and `aside negative` notes, and inline code, bold and links.
"""
import html
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = HERE / "qnx-timertimeout-unblock.md"
ASSETS = "https://storage.googleapis.com/claat-public"


def inline(text):
    """Escapes text and renders `code`, **bold** and [links](url)."""
    parts = re.split(r"(`[^`]+`)", text)
    out = []
    for part in parts:
        if part.startswith("`") and part.endswith("`") and len(part) > 1:
            out.append("<code>" + html.escape(part[1:-1]) + "</code>")
            continue
        s = html.escape(part, quote=False)
        s = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", s)
        out.append(s)
    s = "".join(out)
    # Links may wrap inline code, so render them after the code spans.
    return re.sub(r"\[(.+?)\]\((https?://[^)\s]+)\)", r'<a href="\2" target="_blank">\1</a>', s)


def dedent(lines, n=4):
    return [ln[n:] if ln.startswith(" " * n) else ln.lstrip() for ln in lines]


def render(lines):
    """Renders a list of Markdown lines (one step's body) to HTML."""
    out, i = [], 0
    while i < len(lines):
        line = lines[i]
        if not line.strip() or line.strip() == "---":
            i += 1
        elif line.startswith("```"):
            lang = line[3:].strip()
            j = i + 1
            while j < len(lines) and not lines[j].startswith("```"):
                j += 1
            code = html.escape("\n".join(lines[i + 1:j]))
            if lang == "console":
                # Program output: keep the syntax highlighter off it.
                out.append(f'<pre><code><span class="nocode">{code}</span></code></pre>')
            else:
                cls = f' language="language-{lang}" class="language-{lang}"' if lang else ""
                out.append(f"<pre><code{cls}>{code}</code></pre>")
            i = j + 1
        elif line.startswith("### "):
            out.append(f"<h2 is-upgraded>{inline(line[4:].strip())}</h2>")
            i += 1
        elif line.strip() in ("aside positive", "aside negative"):
            kind = "special" if line.strip().endswith("positive") else "warning"
            j = i + 1
            while j < len(lines) and lines[j].strip():
                j += 1
            out.append(f'<aside class="{kind}"><p>{inline(" ".join(l.strip() for l in lines[i + 1:j]))}</p></aside>')
            i = j
        elif re.match(r"^(\* |\d+\. )", line):
            ordered = bool(re.match(r"^\d+\. ", line))
            items = []
            while i < len(lines) and (re.match(r"^(\* |\d+\. )", lines[i]) or lines[i].startswith("    ") or not lines[i].strip()):
                if re.match(r"^(\* |\d+\. )", lines[i]):
                    items.append([re.sub(r"^(\* |\d+\. )", "", lines[i])])
                elif not lines[i].strip():
                    # A blank line ends the list unless the next line continues it.
                    nxt = lines[i + 1] if i + 1 < len(lines) else ""
                    if not (nxt.startswith("    ") or re.match(r"^(\* |\d+\. )", nxt)):
                        break
                    items[-1].append("")
                else:
                    items[-1].append(lines[i])
                i += 1
            tag = "ol" if ordered else "ul"
            attr = ' type="1"' if ordered else ""
            lis = []
            for item in items:
                first, rest = item[0], dedent(item[1:])
                body = inline(first) + (render(rest) if any(l.strip() for l in rest) else "")
                lis.append(f"<li>{body}</li>")
            out.append(f"<{tag}{attr}>\n" + "\n".join(lis) + f"\n</{tag}>")
        else:
            j = i
            while j < len(lines) and lines[j].strip() and not re.match(r"^(```|### |\* |\d+\. |aside )", lines[j]):
                j += 1
            out.append("<p>" + inline(" ".join(l.strip() for l in lines[i:j])) + "</p>")
            i = max(j, i + 1)
    return "\n".join(out)


def parse(text):
    meta, body = text.split("\n\n\n", 1)
    info = dict(line.split(": ", 1) for line in meta.splitlines() if ": " in line)
    steps = []
    for chunk in re.split(r"^## ", body, flags=re.M)[1:]:
        lines = chunk.splitlines()
        label = lines[0].strip()
        minutes = 0
        if len(lines) > 1 and lines[1].startswith("Duration:"):
            mm, ss = lines[1].split(":", 1)[1].strip().split(":")
            minutes = int(mm) + (1 if int(ss) >= 30 else 0)
            lines = lines[2:]
        else:
            lines = lines[1:]
        steps.append((label, minutes, render(lines)))
    return info, steps


def page(info, steps):
    step_html = "\n".join(
        f'      <google-codelab-step label="{html.escape(label)}" duration="{minutes}">\n{body}\n      </google-codelab-step>'
        for label, minutes, body in steps)
    title = html.escape(info["title"])
    return f"""<!doctype html>
<html lang="en">
<head>
  <meta name="viewport" content="width=device-width, minimum-scale=1.0, initial-scale=1.0, user-scalable=yes">
  <meta name="theme-color" content="#4F7DC9">
  <meta charset="UTF-8">
  <meta name="description" content="{html.escape(info['summary'])}">
  <title>{title}</title>
  <link rel="stylesheet" href="//fonts.googleapis.com/css?family=Source+Code+Pro:400|Roboto:400,300,400italic,500,700|Roboto+Mono">
  <link rel="stylesheet" href="//fonts.googleapis.com/icon?family=Material+Icons">
  <link rel="stylesheet" href="{ASSETS}/codelab-elements.css">
  <style>
    google-codelab #done {{ display: none !important; }}
  </style>
</head>
<body>
  <google-codelab id="{html.escape(info['id'])}"
                  title="{title}"
                  environment="web"
                  feedback-link="{html.escape(info['feedback_link'])}">
{step_html}
  </google-codelab>

  <script src="{ASSETS}/native-shim.js"></script>
  <script src="{ASSETS}/custom-elements.min.js"></script>
  <script src="{ASSETS}/prettify.js"></script>
  <script src="{ASSETS}/codelab-elements.js"></script>
</body>
</html>
"""


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: build_site.py <output-dir>")
    out = Path(sys.argv[1])
    info, steps = parse(SOURCE.read_text(encoding="utf-8"))
    (out / info["id"]).mkdir(parents=True, exist_ok=True)
    (out / info["id"] / "index.html").write_text(page(info, steps), encoding="utf-8")
    (out / "index.html").write_text(
        f'<!doctype html><meta charset="utf-8"><meta http-equiv="refresh" content="0; url={info["id"]}/">'
        f'<title>{html.escape(info["title"])}</title><a href="{info["id"]}/">{html.escape(info["title"])}</a>\n',
        encoding="utf-8")
    (out / ".nojekyll").write_text("", encoding="utf-8")
    print(f"{len(steps)} steps -> {out / info['id'] / 'index.html'}")


if __name__ == "__main__":
    main()
