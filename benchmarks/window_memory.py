"""Measure Linux desktop RSS under X11: xvfb-run -a python3 benchmarks/window_memory.py build/slate"""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import time

GIB = 1024 ** 3


def measure(binary, path):
    started = time.perf_counter()
    app = subprocess.Popen([binary, path])
    try:
        while True:
            matching = subprocess.run(
                ["xdotool", "search", "--all", "--onlyvisible", "--pid", str(app.pid),
                 "--name", "Slate"], text=True, stdout=subprocess.PIPE).stdout.splitlines()
            if matching:
                window = matching[0]
                break
            if app.poll() is not None or time.perf_counter() - started > 10:
                raise RuntimeError("Editor window did not appear")
            time.sleep(.004)
        launch_ms = (time.perf_counter() - started) * 1000
        subprocess.run(["xdotool", "windowfocus", "--sync", window, "key", "ctrl+End"],
                       check=True, timeout=5)
        time.sleep(.3)
        status = Path(f"/proc/{app.pid}/status").read_text().splitlines()
        peak_kib = next(int(line.split()[1]) for line in status if line.startswith("VmHWM:"))
        print(f"{path.name:12}   {launch_ms:6.1f} ms to map   {peak_kib / 1024:5.1f} MiB peak RSS")
        subprocess.run(["xdotool", "key", "ctrl+w"], check=True)
        if app.wait(timeout=5):
            raise RuntimeError("Editor exited with an error")
    finally:
        if app.poll() is None:
            app.kill()
            app.wait()


def main():
    if len(sys.argv) != 2 or not sys.platform.startswith("linux") or not os.environ.get("DISPLAY"):
        raise SystemExit(__doc__)
    binary = Path(sys.argv[1]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="slate-window-bench-") as tmp:
        directory = Path(tmp)
        if shutil.disk_usage(tmp).free < 5 * GIB + 64 * 1024 ** 2:
            raise SystemExit("The benchmark requires just over 5 GiB of temporary space.")
        for i in range(50000):
            (directory / f"record-{i:05d}").touch()
        small = directory / "small.md"
        small.write_text("# Design notes\n\nA short document.\n", encoding="utf-8")
        sparse = directory / "sparse.log"
        with sparse.open("wb") as file:
            file.seek(5 * GIB)
            file.write(b"\nTAIL\n")
        dense = directory / "dense.log"
        block = (b"Log record for performance testing.".ljust(63) + b"\n") * 16384
        with dense.open("wb") as file:
            for _ in range(5120):
                file.write(block)
        print("50,000 directory entries, warm filesystem cache, one fresh process per file")
        for path in (small, sparse, dense):
            measure(binary, path)


if __name__ == "__main__":
    main()
