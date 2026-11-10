#!/usr/bin/env python3
"""Android bridge manifest: the description of the Android GS bridge (librrv-pcsx2-gs-bridge.so) that the product's
CMake turns into its resource package, in the shape scripts/pcsx2_bridge_manifest.py writes for Linux (schema 2).

The desktop manifests also prove the bridge's provenance (a clean RRV checkout, the PCSX2 index tree, the exact patch
stack). The Android bridge is cross-built from a patched copy (tools/patches/android) and ships inside a signed APK, so
this records the patches by hash and the library and resources by digest, and verification checks those digests.

  create --library L --pcsx2-source S --build-dir B --output M
  verify --manifest M [the desktop verifier's --expected-* options, accepted and ignored]
"""
import argparse, datetime, hashlib, json, pathlib, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
PATCHES = ["tools/patches/pcsx2-gs-bridge-target.patch", "tools/patches/pcsx2-gs-bridge-linux.patch",
           "tools/patches/android/pcsx2-android.patch", "tools/patches/android/bridge-android.diff",
           "tools/patches/mobile/bridge-touch-draw.diff"]


def sha256(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def inventory(directory: pathlib.Path) -> list:
    files = sorted(p for p in directory.rglob("*") if p.is_file())
    if not files:
        raise SystemExit(f"runtime resource directory is empty: {directory}")
    return [{"path": p.relative_to(directory).as_posix(), "size": p.stat().st_size, "sha256": sha256(p)} for p in files]


def create(args) -> int:
    library = args.library.resolve()
    resources = library.parent / "resources"
    build_dir = args.build_dir.resolve()
    now = datetime.datetime.now(datetime.timezone.utc).isoformat()
    patches = [{"path": p, "sha256": sha256(ROOT / p)} for p in PATCHES]
    library_data = {"path": str(library), "size": library.stat().st_size, "sha256": sha256(library)}
    receipt = {"schema": 1, "created_utc": now, "architecture": "aarch64", "platform": "android",
               "patches": patches, "library": library_data}
    stamp = build_dir / "RRV_BRIDGE_STAMP.json"
    stamp.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    files = inventory(resources)
    target = next(p for p in patches if p["path"].endswith("pcsx2-gs-bridge-target.patch"))
    linux = next(p for p in patches if p["path"].endswith("pcsx2-gs-bridge-linux.patch"))
    data = {
        "schema": 2, "platform": "android", "created_utc": now, "architecture": "aarch64",
        "rrv_commit": args.rrv_commit, "rrv_source_clean": False, "pcsx2_commit": args.pcsx2_commit,
        "pcsx2_source": {"path": str(args.pcsx2_source.resolve())},
        "bridge_patch": target, "linux_bridge_patch": linux, "android_patches": patches,
        "build_receipt": {"path": str(stamp), "sha256": sha256(stamp), "data": receipt},
        "runtime_resources": {"directory": str(resources), "file_count": len(files), "files": files},
        "library": library_data,
    }
    args.output.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"android bridge manifest: {args.output}")
    return 0


def verify(args) -> int:
    data = json.loads(args.manifest.read_text(encoding="utf-8"))
    if data.get("platform") != "android":
        print("not an Android bridge manifest", file=sys.stderr)
        return 1
    library = pathlib.Path(data["library"]["path"])
    if not library.is_file() or sha256(library) != data["library"]["sha256"]:
        print(f"bridge library changed since its manifest: {library}", file=sys.stderr)
        return 1
    resources = pathlib.Path(data["runtime_resources"]["directory"])
    for entry in data["runtime_resources"]["files"]:
        path = resources / entry["path"]
        if not path.is_file() or sha256(path) != entry["sha256"]:
            print(f"bridge resource changed since its manifest: {path}", file=sys.stderr)
            return 1
    print(json.dumps({"verified": True, "platform": "android"}))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    c = sub.add_parser("create")
    c.add_argument("--library", type=pathlib.Path, required=True)
    c.add_argument("--pcsx2-source", type=pathlib.Path, required=True)
    c.add_argument("--build-dir", type=pathlib.Path, required=True)
    c.add_argument("--output", type=pathlib.Path, required=True)
    c.add_argument("--rrv-commit", default="android")
    c.add_argument("--pcsx2-commit", default="fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3")
    v = sub.add_parser("verify")
    v.add_argument("--manifest", type=pathlib.Path, required=True)
    args, _ignored = parser.parse_known_args()  # the desktop verifier's --expected-* pins do not apply here
    return create(args) if args.command == "create" else verify(args)


if __name__ == "__main__":
    raise SystemExit(main())
