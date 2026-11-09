#!/usr/bin/env python3
"""Upload a published GitHub release to the Nexus Mods page as a new file version.

Run by .github/workflows/nexus.yml when a release is published (or by hand from
the Actions page for a retry). Standard library only.

  python tools/nexus_release.py --tag v0.94 --assets rel/ --notes notes.md [--dry-run]

- `--assets` holds the release's setup.exe and SHA256SUMS.txt, downloaded
  unchanged from the release. Their hashes are checked against SHA256SUMS.txt
  before anything is uploaded, and the two files are zipped as they are.
- The zip becomes a new version of the existing Nexus file (the old version
  moves to "Old files") and the page's version number follows it (the API's
  update_mod_version, not a page edit). The per-file note gets the release's first line and a
  link to the release (SHA-256, VirusTotal, build provenance).
- The changelog entries come from the release notes: the first line, then each
  section heading and bullet, with Markdown stripped, except the "Upgrading..."
  and "Verification" sections. One changelog call per line, as before.
- The mod page's own text (summary, description) is never touched.
- Re-running is safe: a version already on Nexus is not uploaded again, and a
  version that already has changelog entries gets no more.

The API key comes from NEXUS_API_KEY and is never printed.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

V3 = "https://api.nexusmods.com/v3"
V1 = "https://api.nexusmods.com/v1"
GAME = "skyrimspecialedition"
MOD = "197761"                      # URL id of the Nexus page
FILE_GROUP = "8293728"              # the "Plugin Shell Extension" file on that page
FILE_NAME = "Plugin Shell Extension"
REPO_URL = "https://github.com/cecell/plugin-shell-extension"
UA = "plugin-shell-extension-release/1.0"
SKIP_SECTIONS = ("upgrading", "verification")


def key() -> str:
    k = os.environ.get("NEXUS_API_KEY", "").strip()
    if not k:
        raise SystemExit("NEXUS_API_KEY is not set")
    return k


def call(method: str, url: str, body: dict | None = None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method, headers={
        "apikey": key(), "Accept": "application/json", "User-Agent": UA,
        **({"Content-Type": "application/json"} if data is not None else {})})
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            raw = r.read()
            return json.loads(raw) if raw else None
    except urllib.error.HTTPError as e:
        raise SystemExit(f"{method} {url} -> HTTP {e.code}: {e.read().decode('utf-8', 'replace')[:1500]}")


def put_file(url: str, data: bytes, filename: str, md5_raw: bytes) -> None:
    # The presigned URL signs all three headers; urllib's default Content-Type breaks it.
    req = urllib.request.Request(url, data=data, method="PUT", headers={
        "Content-Disposition": f'attachment; filename="{filename}"',
        "Content-MD5": base64.b64encode(md5_raw).decode(),
        "Content-Type": "application/octet-stream",
        "Content-Length": str(len(data))})
    for attempt in range(1, 6):
        try:
            with urllib.request.urlopen(req, timeout=600):
                return
        except urllib.error.HTTPError as e:
            raise SystemExit(f"PUT presigned -> HTTP {e.code}: {e.read().decode('utf-8', 'replace')[:800]}")
        except urllib.error.URLError as e:      # the storage host sometimes drops the connection mid-send
            if attempt == 5:
                raise
            print(f"PUT attempt {attempt} failed ({e.reason}); retrying", flush=True)
            time.sleep(5 * attempt)


def plain(md: str) -> str:
    """Markdown inline -> plain text: [text](url) -> text, drop ** and backticks."""
    md = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", md)
    return md.replace("**", "").replace("`", "").strip()


def parse_notes(body: str) -> tuple[str, list[str]]:
    """First paragraph = summary; changelog = summary + kept sections' headings and bullets."""
    lines = body.replace("\r\n", "\n").split("\n")
    summary = ""
    for ln in lines:
        if ln.strip() and not ln.startswith("#"):
            summary = plain(ln)
            break
    if not summary:
        raise SystemExit("release notes have no summary line")
    out, keep = [summary], False
    for ln in lines:
        s = ln.strip()
        if s.startswith("## "):
            heading = plain(s[3:])
            keep = not heading.lower().startswith(SKIP_SECTIONS)
            if keep:
                out.append(heading)
        elif keep and s.startswith(("- ", "* ")):
            out.append(plain(s[2:]))
        elif keep and s and out and not s.startswith("#"):
            # a wrapped bullet or paragraph inside a kept section
            out.append(plain(s))
    return summary, out


def check_and_zip(assets: Path, version: str) -> tuple[Path, bytes]:
    sums = {}
    for ln in (assets / "SHA256SUMS.txt").read_text(encoding="utf-8").splitlines():
        if ln.strip():
            digest, name = ln.split(None, 1)
            sums[name.strip().lstrip("*")] = digest.lower()
    setup = assets / f"PluginShellExtension-{version}-setup.exe"
    if not setup.is_file():
        raise SystemExit(f"{setup.name} is not among the release assets")
    actual = hashlib.sha256(setup.read_bytes()).hexdigest()
    if sums.get(setup.name) != actual:
        raise SystemExit(f"{setup.name}: SHA-256 {actual} does not match SHA256SUMS.txt")
    out = assets.parent / f"PluginShellExtension-{version}.zip"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(setup, setup.name)
        z.write(assets / "SHA256SUMS.txt", "SHA256SUMS.txt")
    return out, out.read_bytes()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tag", required=True, help="release tag, e.g. v0.94")
    ap.add_argument("--assets", required=True, type=Path, help="folder with the release's setup.exe + SHA256SUMS.txt")
    ap.add_argument("--notes", required=True, type=Path, help="the release notes (Markdown)")
    ap.add_argument("--dry-run", action="store_true", help="read only: show what would be uploaded")
    a = ap.parse_args()

    version = a.tag[1:] if a.tag.startswith("v") else a.tag
    summary, changelog = parse_notes(a.notes.read_text(encoding="utf-8"))
    note = f"{summary} Release notes, SHA-256 and VirusTotal: {REPO_URL}/releases/tag/{a.tag}"
    archive, data = check_and_zip(a.assets, version)

    mod_id = call("GET", f"{V3}/games/{GAME}/mods/{MOD}")["data"]["id"]
    versions = call("GET", f"{V3}/mod-files/{FILE_GROUP}/versions")["data"]["versions"]
    if not versions or versions[0]["file"]["name"] != FILE_NAME:
        raise SystemExit(f"file group {FILE_GROUP} is not {FILE_NAME!r} any more; update FILE_GROUP")
    on_nexus = any(v["version"] == version for v in versions)
    logged = call("GET", f"{V1}/games/{GAME}/mods/{MOD}/changelogs.json") or {}
    has_log = bool(logged.get(version))

    print(f"Nexus mod {MOD} (v3 {mod_id}), file group {FILE_GROUP}, version {version}")
    print(f"archive: {archive.name} ({len(data):,} bytes)")
    print(f"file note: {note}")
    print("changelog:" + "".join(f"\n  {ln}" for ln in changelog))
    summ = os.environ.get("GITHUB_STEP_SUMMARY")

    def report(text: str) -> None:
        print(text)
        if summ:
            with open(summ, "a", encoding="utf-8") as f:
                f.write(text + "\n")

    if a.dry_run:
        print(f"dry run: upload {'skipped (already on Nexus)' if on_nexus else 'would happen'}, "
              f"changelog {'skipped (already there)' if has_log else 'would be added'}")
        return 0

    report("## Nexus Mods")
    # The site doesn't show entries added after a version's last page change until the
    # page next changes, so they go in first and the upload that follows is that change.
    # Each is added once: entries can't be replaced, only appended.
    added = len(changelog) if has_log else 0

    def add_changelog() -> str:
        nonlocal added
        try:
            while added < len(changelog):
                call("POST", f"{V3}/mods/{mod_id}/changelogs", {"version": version, "changelog": changelog[added]})
                added += 1
        except SystemExit as e:
            return str(e)
        return ""

    early_error = add_changelog()
    if early_error:
        print(f"The changelog wasn't accepted before the upload; trying again after it: {early_error}")
    if on_nexus:
        report(f"- {version} is already on Nexus; not uploaded again.")
    else:
        md5 = hashlib.md5(data)
        up = call("POST", f"{V3}/uploads", {"size_bytes": len(data), "filename": archive.name, "md5": md5.hexdigest()})
        uid = up["data"]["id"]
        put_file(up["data"]["presigned_url"], data, archive.name, md5.digest())
        call("POST", f"{V3}/uploads/{uid}/finalise")
        for _ in range(90):
            if call("GET", f"{V3}/uploads/{uid}")["data"]["state"] == "available":
                break
            time.sleep(2)
        else:
            raise SystemExit(f"upload {uid} never became available")
        call("POST", f"{V3}/mod-files/{FILE_GROUP}/versions", {
            "upload_id": uid, "name": FILE_NAME, "version": version, "description": note,
            "file_category": "main", "show_requirements_pop_up": False,
            "allow_mod_manager_download": True, "archive_existing_file": True, "update_mod_version": True})
        report(f"- Uploaded {archive.name} as version {version}; the previous version moved to Old files; "
               "the page's version follows it.")
    late_error = add_changelog()
    if late_error:
        raise SystemExit(late_error)
    if has_log:
        report(f"- The changelog already has {version}; nothing added.")
    else:
        report(f"- Changelog: {len(changelog)} lines added for {version}.")
    report(f"- Page: https://www.nexusmods.com/{GAME}/mods/{MOD}?tab=files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
