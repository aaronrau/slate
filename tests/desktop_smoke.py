"""Linux X11 end-to-end test. Run using xvfb-run -a python3 tests/desktop_smoke.py BUILD/slate."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

def xdo(*args):
    return subprocess.run(["xdotool", *args], text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL, check=False).stdout.strip()

def wait_for(condition, description):
    end = time.monotonic() + 8
    while time.monotonic() < end:
        if condition():
            return
        time.sleep(.04)
    raise AssertionError(description)

def key(*keys):
    xdo("key", "--clearmodifiers", *keys)

def type_text(text):
    xdo("type", "--clearmodifiers", "--delay", "2", text)

def save_and_check(condition):
    key("ctrl+s")
    wait_for(condition, "Unexpected file content after save")
    # Saving finishes on the main loop after the atomic rename.
    time.sleep(.08)

def exercise(binary, directory):
    first = directory / "start.md"
    source = "# First\n\nA **bold** word and `code`.\n"
    first.write_text(source, encoding="utf-8")
    target = directory / "target.md"
    target.write_text("before MARKER after\n", encoding="utf-8")
    huge = directory / "huge.log"
    with huge.open("wb") as stream:
        stream.seek(5 * 1024**3)
        stream.write(b"\nTAIL\n")
    proc = subprocess.Popen([binary, first], env=dict(os.environ, G_DEBUG="fatal-criticals"))
    try:
        wait_for(lambda: xdo("search", "--onlyvisible", "--name", "^start.md"), "No window")
        window = xdo("search", "--onlyvisible", "--name", "^start.md").splitlines()[0]
        xdo("windowfocus", "--sync", window)
        # A normal Markdown open focuses the formatted view. Copy its visible
        # text back into the source editor to verify the window's focus routing.
        time.sleep(.2)
        key("alt+s", "a", "alt+e", "c", "alt+v", "p", "ctrl+a", "ctrl+v")
        time.sleep(.1)
        def pasted_preview():
            text = first.read_text(encoding="utf-8")
            return "First" in text and "A bold word and code." in text and "#" not in text
        save_and_check(pasted_preview)
        rendered = first.read_bytes()
        key("ctrl+a")
        type_text("Hello")
        # Switching modes previews unsaved edits and returns to source without saving.
        key("ctrl+shift+m")
        time.sleep(.1)
        key("ctrl+shift+m")
        key("ctrl+w")
        time.sleep(.1)
        key("Escape")
        assert proc.poll() is None and first.read_bytes() == rendered
        key("ctrl+a", "ctrl+c", "End", "ctrl+v")
        time.sleep(.12)
        save_and_check(lambda: first.read_bytes() == b"HelloHello")
        key("ctrl+z")
        save_and_check(lambda: first.read_bytes() == b"Hello")
        key("ctrl+shift+z")
        save_and_check(lambda: first.read_bytes() == b"HelloHello")

        def search(shortcut, query, filename):
            key(shortcut)
            type_text(query)
            # A query is debounced and results arrive from the workspace worker.
            def activate_result():
                if xdo("getwindowname", window).startswith(filename):
                    return True
                key("Return")
                return False
            wait_for(activate_result, f"Could not open search result for {query}")

        search("ctrl+shift+f", "MARKER", target.name)
        type_text("found")
        save_and_check(lambda: target.read_bytes() == b"before found after\n")
        search("ctrl+p", "huge.log", huge.name)
        key("ctrl+End")
        type_text("last")
        def tail_saved():
            with huge.open("rb") as stream:
                stream.seek(-4, 2)
                return stream.read() == b"last"
        save_and_check(tail_saved)
        key("ctrl+a", "Delete")
        save_and_check(lambda: huge.stat().st_size == 0)
        key("ctrl+w")
        assert proc.wait(timeout=5) == 0
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()

if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="slate-ui-") as path:
        exercise(str(Path(sys.argv[1]).resolve()), Path(path))
