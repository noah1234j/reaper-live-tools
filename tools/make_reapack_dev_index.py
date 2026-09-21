#!/usr/bin/env python3
"""Regenerate index-dev.xml, the ReaPack repository index for the dev channel.

Same job as make_reapack_index.py, for a separate ReaPack repository that
publishes the dev branch's builds. Run it after publishing a dev release:

    python tools/make_reapack_dev_index.py
    git add index-dev.xml && git commit && git push origin dev

Differences from the stable index:

  * Only tags carrying "-dev" are published, so the two channels never pick up
    each other's releases.
  * The index and the package are named separately ("Live Tools (dev)"), so
    ReaPack lists it as its own repository. Both packages install the same
    file, so ReaPack will refuse to have both at once — which is the point:
    the dev channel replaces the stable install rather than sitting beside it.
  * Version names keep a fourth component ("0.0.47.1" for v0.0.47-dev.1) so
    every dev build sorts above the stable release it branched from, and so
    ReaPack does not read them as pre-releases and hide the lot. See the note
    in make_reapack_index.py about why the suffix has to go.

The dev channel is imported in REAPER with:

  https://raw.githubusercontent.com/noah1234j/reaper-live-tools/dev/index-dev.xml
"""

import json
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

REPO = "noah1234j/reaper-live-tools"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Release asset name -> (ReaPack platform, install path under UserPlugins/).
ASSETS = {
    "reaper_transitions.dll":   ("win64",  "reaper_transitions.dll"),
    "reaper_transitions.dylib": ("darwin", "reaper_transitions.dylib"),
    "reaper_transitions.so":    ("linux64", "reaper_transitions.so"),
}

DESCRIPTION = """Live Tools DEV CHANNEL - unreleased builds from the dev branch.

These are work-in-progress builds. They are less tested than the stable
releases, they can change behaviour between builds, and a project saved by one
may not open the same way in another. Do not use them on a show.

Installing this replaces the stable Live Tools package: both install the same
reaper_transitions.dll, so ReaPack will not keep both. Uninstall "Live Tools"
first, and reinstall it from the stable repository to go back.

Stable repository:
https://raw.githubusercontent.com/noah1234j/reaper-live-tools/main/index.xml"""


def indent(elem, level=0, space="  "):
    """ET.indent from Python 3.9, inlined so 3.8 works too.

    Only ever touches elements that have no text of their own, so the blank
    lines separating paragraphs in the changelogs and the description survive.
    """
    pad = "\n" + space * level
    if len(elem):
        if not (elem.text or "").strip():
            elem.text = pad + space
        for child in elem:
            indent(child, level + 1, space)
        if not (elem.tail or "").strip():
            elem.tail = pad
        if not (elem[-1].tail or "").strip():
            elem[-1].tail = pad
    elif level and not (elem.tail or "").strip():
        elem.tail = pad


def sh(*args):
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout


def releases():
    """Published dev releases, newest first, with their asset names."""
    data = json.loads(sh("gh", "release", "list", "--repo", REPO,
                         "--limit", "200", "--json", "tagName,isDraft"))
    out = []
    for rel in data:
        if rel["isDraft"]:
            continue
        tag = rel["tagName"]
        if "-dev" not in tag:
            continue          # stable release: make_reapack_index.py owns it
        detail = json.loads(sh("gh", "release", "view", tag, "--repo", REPO,
                               "--json", "publishedAt,assets"))
        out.append({
            "tag":    tag,
            "time":   detail["publishedAt"],
            "assets": [a["name"] for a in detail["assets"]],
        })
    return out


def version_of(tag):
    """v0.0.47-dev.1 -> 0.0.47.1; v0.0.47-dev -> 0.0.47.0.

    The build number becomes a fourth component rather than a suffix so that
    ReaPack reads it as an ordinary version, sorted above the stable release
    the branch came from.
    """
    m = re.match(r"^v?(\d+(?:\.\d+)*)-dev(?:\.(\d+))?$", tag)
    if not m:
        raise SystemExit("cannot read a dev version out of tag %r" % tag)
    return "%s.%s" % (m.group(1), m.group(2) or "0")


def changelogs():
    """Map version number -> that version's CHANGELOG.md section, as plain text."""
    with open(os.path.join(ROOT, "CHANGELOG.md"), encoding="utf-8") as f:
        text = f.read()

    sections = {}
    # "## [v0.0.47-dev.1] — 2026-09-21  (dev branch)" up to the next "## "
    for m in re.finditer(r"^## \[(v[^\]]+)\][^\n]*\n(.*?)(?=^## \[|\Z)",
                         text, re.M | re.S):
        tag = m.group(1)
        if "-dev" not in tag:
            continue
        body = m.group(2).replace("---", "").strip()
        body = re.sub(r"^### ", "", body, flags=re.M)
        body = body.replace("**", "").replace("`", "")
        body = re.sub(r"\n{3,}", "\n\n", body).strip()
        sections[version_of(tag)] = body
    return sections


def build():
    logs = changelogs()

    index = ET.Element("index", {"version": "1", "name": "Live Tools (dev)"})
    category = ET.SubElement(index, "category", {"name": "Extensions"})
    pkg = ET.SubElement(category, "reapack", {
        "name": "reaper_transitions_dev.ext",
        "type": "extension",
        "desc": "Live Tools (dev branch) - unreleased builds, not for show use",
    })

    published = 0
    for rel in releases():
        sources = [(p, path, name) for name, (p, path) in ASSETS.items()
                   if name in rel["assets"]]
        if not sources:
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
                                 "href": "https://github.com/%s/tree/dev" % REPO}).text = "GitHub (dev branch)"

    indent(index)

    out = os.path.join(ROOT, "index-dev.xml")
    with open(out, "wb") as f:
        f.write(b'<?xml version="1.0" encoding="utf-8"?>\n')
        ET.ElementTree(index).write(f, encoding="utf-8", xml_declaration=False)
        f.write(b"\n")
    print("wrote %s (%d versions)" % (out, published))


if __name__ == "__main__":
    build()
