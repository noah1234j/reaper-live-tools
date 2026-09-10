#!/usr/bin/env python3
"""Regenerate index.xml, the ReaPack repository index for Live Tools.

ReaPack installs the extension straight from the GitHub release assets, so this
script does not touch binaries — it only records, for each released version,
where its assets live and what changed. Run it after publishing a release:

    python tools/make_reapack_index.py
    git add index.xml && git commit && git push

Versions are discovered from the GitHub releases via `gh`, and each version's
changelog text is lifted from CHANGELOG.md.

Note on version names: GitHub tags are "vX.Y.Z-beta", but the index publishes
them as plain "X.Y.Z". ReaPack treats any version carrying a pre-release suffix
as a pre-release and hides it unless the user has opted into those in its
settings — which would make the whole repository look empty, since every Live
Tools release is a beta.
"""

import json
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from xml.dom import minidom

REPO = "noah1234j/reaper-live-tools"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Release asset name -> (ReaPack platform, install path under UserPlugins/).
# A version is published for whichever of these its release actually carries,
# so the macOS build appearing later needs no change here.
ASSETS = {
    "reaper_transitions.dll":   ("win64",  "reaper_transitions.dll"),
    "reaper_transitions.dylib": ("darwin", "reaper_transitions.dylib"),
    "reaper_transitions.so":    ("linux64", "reaper_transitions.so"),
}

DESCRIPTION = """Live Tools is a REAPER extension for live and theatre rigs.

Scenes     capture and recall mixer, FX and plugin state with timed transitions
Layers     named track views that drive MCP/TCP visibility and order
DCAs       VCA-style control groups
Mute Groups, Live Lock, monitors and a meter bridge

BETA software. Back up your projects before using it on a show."""


def sh(*args):
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout


def releases():
    """Published releases, newest first, with their asset names."""
    data = json.loads(sh("gh", "release", "list", "--repo", REPO,
                         "--limit", "200", "--json", "tagName,isDraft"))
    out = []
    for rel in data:
        if rel["isDraft"]:
            continue
        tag = rel["tagName"]
        detail = json.loads(sh("gh", "release", "view", tag, "--repo", REPO,
                               "--json", "publishedAt,assets"))
        out.append({
            "tag":    tag,
            "time":   detail["publishedAt"],
            "assets": [a["name"] for a in detail["assets"]],
        })
    return out


def version_of(tag):
    """v0.0.31-beta -> 0.0.31 (see the note in the module docstring)."""
    m = re.match(r"^v?(\d+(?:\.\d+)*)", tag)
    if not m:
        raise SystemExit("cannot read a version number out of tag %r" % tag)
    return m.group(1)


def changelogs():
    """Map version number -> that version's CHANGELOG.md section, as plain text."""
    with open(os.path.join(ROOT, "CHANGELOG.md"), encoding="utf-8") as f:
        text = f.read()

    sections = {}
    # "## [v0.0.31-beta] — 2026-09-10" up to the next "## " heading
    for m in re.finditer(r"^## \[(v[^\]]+)\][^\n]*\n(.*?)(?=^## \[|\Z)",
                         text, re.M | re.S):
        body = m.group(2).replace("---", "").strip()
        # Flatten the markdown: bullets keep their dash, bold markers go away.
        body = re.sub(r"^### ", "", body, flags=re.M)
        body = body.replace("**", "").replace("`", "")
        body = re.sub(r"\n{3,}", "\n\n", body).strip()
        sections[version_of(m.group(1))] = body
    return sections


def build():
    logs = changelogs()

    index = ET.Element("index", {"version": "1", "name": "Live Tools"})
    category = ET.SubElement(index, "category", {"name": "Extensions"})
    pkg = ET.SubElement(category, "reapack", {
        "name": "reaper_transitions.ext",
        "type": "extension",
        "desc": "Live Tools - scenes, layers, DCAs and mute groups for live REAPER rigs",
    })

    published = 0
    for rel in releases():
        sources = [(p, path, name) for name, (p, path) in ASSETS.items()
                   if name in rel["assets"]]
        if not sources:
            # Nothing to install for this tag (e.g. a source-only release).
            print("skipping %s: no installable assets" % rel["tag"], file=sys.stderr)
            continue

        ver = version_of(rel["tag"])
        v = ET.SubElement(pkg, "version", {
            "name": ver, "author": "noah1234j", "time": rel["time"],
        })
        if ver in logs:
            ET.SubElement(v, "changelog").text = logs[ver]
        for platform, install_path, asset in sources:
            src = ET.SubElement(v, "source",
                                {"platform": platform, "file": install_path})
            src.text = ("https://github.com/%s/releases/download/%s/%s"
                        % (REPO, rel["tag"], asset))
        published += 1

    meta = ET.SubElement(index, "metadata")
    ET.SubElement(meta, "description").text = DESCRIPTION
    ET.SubElement(meta, "link", {"rel": "website",
                                 "href": "https://github.com/%s" % REPO}).text = "GitHub"

    xml = minidom.parseString(ET.tostring(index, "utf-8")) \
                 .toprettyxml(indent="  ", encoding="utf-8").decode("utf-8")
    xml = "\n".join(line for line in xml.splitlines() if line.strip()) + "\n"

    out = os.path.join(ROOT, "index.xml")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(xml)
    print("wrote %s (%d versions)" % (out, published))


if __name__ == "__main__":
    build()
