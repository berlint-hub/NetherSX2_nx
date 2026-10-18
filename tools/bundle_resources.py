import argparse
from pathlib import Path
import shutil

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cores-dir", type=Path, required=True)
    args = parser.parse_args()
    workspace = Path(__file__).resolve().parent.parent
    generated = workspace / "launcher" / "romfs" / "res"
    generated.mkdir(parents=True, exist_ok=True)
    # The product ships a single core. A version with no assets on disk is
    # therefore expected, not an error -- but its generated romfs directory must
    # not survive from an earlier two-core build, or the bundle would carry
    # resources for a core that is not in it.
    bundled = []
    for version in ("4248", "3668"):
        source = args.cores_dir / f"NetherSX2-v2.2n-{version}" / "assets"
        if not (source / "GameIndex.yaml").is_file():
            stale = generated / version
            if stale.exists():
                shutil.rmtree(stale)
                print(f"Removed stale {version} resources")
            print(f"Skipping {version}: no assets at {source}")
            continue
        target = generated / version

        if target.is_symlink() or target.resolve() != workspace / "launcher" / "romfs" / "res" / version:
            raise ValueError(f"Unsafe generated resource directory: {target}")
        if target.exists():
            shutil.rmtree(target)
        shutil.copytree(source, target, ignore=shutil.ignore_patterns("dexopt"))
        bundled.append(version)
        print(f"Bundled {version} resources")
    if not bundled:
        raise ValueError(f"No core assets found under {args.cores_dir}")

if __name__ == "__main__":
    main()
