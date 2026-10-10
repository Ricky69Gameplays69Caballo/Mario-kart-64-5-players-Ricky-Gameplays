"""Package fresh builds from explicit runtime files, excluding user game data."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import zipfile


def audit(folder):
    forbidden = {".z64", ".n64", ".v64", ".sav", ".pdb", ".log"}
    for path in folder.rglob("*"):
        if not path.is_file():
            continue
        if path.suffix.lower() in forbidden or path.name in {"spaghettify.cfg.json", "imgui.ini"}:
            raise RuntimeError(f"Personal/game data in package: {path}")
        if path.suffix.lower() == ".o2r" and path.name != "spaghetti.o2r":
            raise RuntimeError(f"Extracted game archive in package: {path}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--platform", required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    build = args.build_dir.resolve()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    name = f"Mario-Kart-64-5-Players-Ricky-Gameplays-{args.platform}"
    stage = build / "release-staging" / args.platform
    stage.mkdir(parents=True, exist_ok=True)
    if args.platform == "windows-x64":
        package = stage / name
        package.mkdir()
        shutil.copy2(build / "Release" / "Spaghettify.exe", package)
        for filename in ("spaghetti.o2r", "gamecontrollerdb.txt", "README-Release.md"):
            shutil.copy2(build / filename, package)
        shutil.copy2(root / "config.yml", package)
        for directory in ("yamls", "meta"):
            shutil.copytree(root / directory, package / directory)
        audit(package)
        artifact = out / f"{name}.zip"
        with zipfile.ZipFile(artifact, "w", zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(stage.rglob("*")):
                if path.is_file():
                    archive.write(path, path.relative_to(stage))
    elif args.platform.startswith("macos-"):
        app = build / "SpaghettiKart.app"
        resources = app / "Contents" / "Resources"
        for filename in ("gamecontrollerdb.txt", "README-Release.md"):
            shutil.copy2(build / filename, resources)
        audit(app)
        subprocess.run(["codesign", "--force", "--deep", "--sign", "-", "--options", "runtime",
                        "--entitlements", str(root / "cmake/macos/entitlements.plist"), str(app)], check=True)
        artifact = out / f"{name}.zip"
        subprocess.run(["ditto", "-c", "-k", "--keepParent", str(app), str(artifact)], check=True)
    elif args.platform.startswith("linux-"):
        images = list(build.glob("*.appimage")) + list(build.glob("*.AppImage"))
        if len(images) != 1:
            raise RuntimeError(f"Expected one complete AppImage, found {images}")
        artifact = out / f"{name}.AppImage"
        shutil.copy2(images[0], artifact)
        artifact.chmod(0o755)
    else:
        raise RuntimeError(f"Unsupported platform: {args.platform}")
    if artifact.stat().st_size < 1_000_000:
        raise RuntimeError(f"Incomplete release artifact: {artifact}")
    digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
    artifact.with_name(artifact.name + ".sha256").write_text(f"{digest}  {artifact.name}\n", encoding="utf-8")
    print(f"Packaged {artifact.name}: {artifact.stat().st_size} bytes; SHA-256 {digest}")


if __name__ == "__main__":
    main()
