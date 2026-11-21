"""Capture the four graphics QA shots with deterministic camera, animation and lighting."""

import argparse
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build-windows" / "lighting-replay")
    parser.add_argument("--graphics", choices=("fast", "balanced", "high"), default="high")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for name, frame in (
        ("still", 48),
        ("pan", 116),
        ("turn", 163),
        ("flashlight", 258),
    ):
        target = args.output.resolve() / f"{name}.tga"
        command = [
            str(ROOT / "build-windows" / "game.exe"),
            "--lighting-replay",
            "--check-visuals",
            "--frames",
            "300",
            "--w",
            "1920",
            "--h",
            "1080",
            "--fps-cap",
            "0",
            "--graphics",
            args.graphics,
            "--shot-frame",
            str(frame),
            "--screenshot",
            str(target),
        ]
        run = subprocess.run(
            command,
            check=False,
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        target.with_suffix(".log").write_bytes(run.stdout)
        run.check_returncode()
        if target.stat().st_size != 18 + 1920 * 1080 * 3:
            raise ValueError(f"Incomplete capture: {target}")
        print(target)


if __name__ == "__main__":
    main()
