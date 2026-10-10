"""Check portable Windows manifest paths and optionally load their packaged DLLs."""
import argparse
import json
import os
from pathlib import Path, PureWindowsPath
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MANIFESTS = (
    ("VK_LAYER_BEAMNG_vrs.json", "layer", "BeamNG_VRS.dll"),
    ("XR_APILAYER_BEAMNG_eye_probe.json", "api_layer", "BeamNG_EyeProbe.dll"),
)


def library_paths(directory):
    paths = []
    for filename, section, dll in MANIFESTS:
        manifest = json.loads((directory / filename).read_text(encoding="utf-8-sig"))
        library = manifest[section]["library_path"]
        # The Windows Vulkan loader recognizes backslashes when deciding to
        # resolve a library relative to its manifest, rather than the game CWD.
        assert "\\" in library, f"{filename}: use a Windows relative path (backslash)"
        path = PureWindowsPath(library)
        assert not path.is_absolute() and not path.drive, f"{filename}: path must be portable"
        assert path.parts == (dll,), f"{filename}: DLL must live beside its manifest"
        paths.append(library)
    return paths


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--package-dir", type=Path)
    parser.add_argument("--dll-check", type=Path)
    args = parser.parse_args()
    library_paths(ROOT / "manifests")
    if args.package_dir or args.dll_check:
        assert args.package_dir and args.dll_check, "Both package and native checker are required"
        assert os.name == "nt", "Native manifest loading requires Windows"
        checker = args.dll_check.resolve()
        package = args.package_dir.resolve()
        library_paths(package)
        with tempfile.TemporaryDirectory(prefix="BeamNG manifest check ") as temporary:
            # Keep DLLs outside the working directory; include spaces and Unicode.
            layers = Path(temporary) / "BeamNG-VRS \u00e9"
            layers.mkdir()
            for filename, _, dll in MANIFESTS:
                shutil.copy2(package / filename, layers / filename)
                shutil.copy2(package / dll, layers / dll)
            paths = [str(layers) + "\\" + library for library in library_paths(layers)]
            subprocess.run([str(checker), *paths], cwd=temporary, check=True)
        print("Manifest DLL loading passed from an unrelated working directory")
    else:
        print("Portable Windows manifest paths passed")


if __name__ == "__main__":
    main()
