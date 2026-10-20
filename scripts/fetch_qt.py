#!/usr/bin/env python3
"""Downloads a Qt 6 binary build from download.qt.io without the online installer (no Qt
account needed): the base package archives you ask for plus add-on modules, each verified
against the .sha1 published next to it, extracted to <out>/<version>/<arch>.

aqtinstall 3.3 builds wrong repository paths for Qt >= 6.11, which is why this script exists.
Extracts with py7zr (pip install py7zr) or, with --extract-with tar, with bsdtar (macOS's tar
reads 7z and keeps the frameworks' symlinks). Used for Windows / msvc2022_64 and, in CI, for macOS:
    python scripts/fetch_qt.py --host mac_x64 --arch clang_64 --extract-with tar --out ~/Qt

    python scripts/fetch_qt.py --out C:/Qt
    python scripts/fetch_qt.py --version 6.12.0 --arch msvc2022_64 --modules qtshadertools --out C:/Qt
"""
import argparse
import hashlib
import subprocess
import sys
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

BASE_ARCHIVES = ("qtbase", "qttools", "qttranslations", "qtsvg", "d3dcompiler_47", "opengl32sw")


def fetch(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=300) as r:
        return r.read()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="6.12.0")
    ap.add_argument("--host", default="windows_x86")
    ap.add_argument("--arch", default="msvc2022_64")
    ap.add_argument("--modules", nargs="*", default=["qtshadertools"])
    ap.add_argument("--out", required=True)
    ap.add_argument("--cache", default=None, help="download folder (default: <out>/_downloads)")
    ap.add_argument("--dry-run", action="store_true", help="only list the archives that would be installed")
    ap.add_argument("--extract-with", choices=("py7zr", "tar"), default="py7zr")
    args = ap.parse_args()

    tag = args.version.replace(".", "")
    target_arch = ("win64_" if args.host.startswith("windows") else "") + args.arch
    # Windows has one repository per toolchain (qt6_6120/qt6_6120_msvc2022_64); macOS one universal build
    # (qt6_6120/qt6_6120).
    flavour = "" if args.host.startswith("mac") else f"_{args.arch}"
    repo = f"https://download.qt.io/online/qtsdkrepository/{args.host}/desktop/qt6_{tag}/qt6_{tag}{flavour}"
    out_dir = Path(args.out) / args.version / args.arch
    cache = Path(args.cache) if args.cache else Path(args.out) / "_downloads"
    if not args.dry_run:
        cache.mkdir(parents=True, exist_ok=True)
        out_dir.mkdir(parents=True, exist_ok=True)

    xml = fetch(f"{repo}/Updates.xml")
    expected = fetch(f"{repo}/Updates.xml.sha256").decode().split()[0]
    if hashlib.sha256(xml).hexdigest() != expected:
        sys.exit("Updates.xml checksum mismatch")

    base_pkg = f"qt.qt6.{tag}.{target_arch}"
    wanted = {base_pkg} | {f"qt.qt6.{tag}.addons.{m}.{target_arch}" for m in args.modules}
    found = set()
    for pkg in ET.fromstring(xml).findall("PackageUpdate"):
        name = pkg.findtext("Name")
        if name not in wanted:
            continue
        found.add(name)
        version = pkg.findtext("Version")
        archives = [a.strip() for a in (pkg.findtext("DownloadableArchives") or "").split(",") if a.strip()]
        for archive in archives:
            if name == base_pkg and archive.split("-")[0] not in BASE_ARCHIVES:
                continue  # skip qtdeclarative, docs, ...
            url = f"{repo}/{name}/{version}{archive}"
            if args.dry_run:
                print("would install", url)
                continue
            local = cache / f"{version}{archive}"
            if not local.exists():
                print("downloading", archive, flush=True)
                local.write_bytes(fetch(url))
            sha1 = fetch(url + ".sha1").decode().split()[0]
            if hashlib.sha1(local.read_bytes()).hexdigest() != sha1:
                sys.exit(f"checksum mismatch: {local.name}")
            print("extracting", archive, flush=True)
            if args.extract_with == "tar":
                subprocess.run(["tar", "-xf", str(local), "-C", str(out_dir)], check=True)
            else:
                import py7zr  # only needed to extract, not for --dry-run

                with py7zr.SevenZipFile(local, "r") as z:
                    z.extractall(out_dir)

    missing = wanted - found
    if missing:
        sys.exit("not found in the repository: " + ", ".join(sorted(missing)))
    print("dry run, nothing installed" if args.dry_run else f"Qt installed in {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
