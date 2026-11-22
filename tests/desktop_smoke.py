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
    env = dict(os.environ, G_DEBUG="fatal-criticals", XDG_DATA_HOME=str(directory / ".app-data"))
    proc = subprocess.Popen([binary, first], env=env)
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

        def at_file(filename):
            wait_for(lambda: xdo("getwindowname", window).startswith(filename),
                     f"Expected active file {filename}")

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
        key("alt+Left")
        at_file(target.name)
        type_text("!")
        # A canceled navigation must preserve unsaved text and the forward visit.
        key("alt+Right")
        time.sleep(.12)
        key("Escape")
        at_file(target.name)
        save_and_check(lambda: target.read_bytes() == b"before found! after\n")
        key("alt+Right")
        at_file(huge.name)
        # Both arrow buttons and the filename tabs are actionable in the strip.
        xdo("mousemove", "--window", window, "246", "40", "click", "1")
        at_file(target.name)
        xdo("mousemove", "--window", window, "325", "40", "click", "1")
        at_file(first.name)
        key("ctrl+Page_Down")
        at_file(target.name)
        key("ctrl+F4")
        at_file(first.name)
        key("ctrl+Page_Down")
        at_file(huge.name)
        key("ctrl+a", "Delete")
        save_and_check(lambda: huge.stat().st_size == 0)
        key("ctrl+w")
        assert proc.wait(timeout=5) == 0
        proc = subprocess.Popen([binary, directory], env=env)
        wait_for(lambda: xdo("search", "--onlyvisible", "--name", "^huge.log"), "History failed to reopen")
        window = xdo("search", "--onlyvisible", "--name", "^huge.log").splitlines()[0]
        xdo("windowfocus", "--sync", window)
        key("alt+Left")
        at_file(first.name)
        key("alt+Right")
        at_file(huge.name)
        first.unlink()
        key("alt+Left")
        at_file(target.name)
        key("ctrl+w")
        assert proc.wait(timeout=5) == 0
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()

def workspace_actions(binary, directory):
    root = directory / "actions"
    nested = root / "nested"
    nested.mkdir(parents=True)
    existing = nested / "existing.txt"
    existing.write_text("original", encoding="utf-8")
    created = nested / "new.md"
    env = dict(os.environ, G_DEBUG="fatal-criticals", XDG_DATA_HOME=str(directory / "settings"))
    proc = subprocess.Popen([binary, root], env=env)
    try:
        def visible(title):
            return xdo("search", "--onlyvisible", "--name", "^" + title + "$")
        wait_for(lambda: visible("Untitled — actions — Slate"), "No workspace window")
        window = visible("Untitled — actions — Slate").splitlines()[0]
        xdo("windowfocus", "--sync", window)
        def click(x, y):
            xdo("mousemove", "--window", window, str(x), str(y), "click", "1")
        def new_file(name):
            click(150, 48)
            wait_for(lambda: visible("New File"), "No New File dialog")
            type_text(name)
            key("Return")
            wait_for(lambda: not visible("New File"), "New File dialog did not close")
        def confirm_delete():
            click(178, 48)
            wait_for(lambda: visible("Delete File"), "No named delete confirmation")
        # The first child in this workspace is a folder. Create inside that selection.
        click(65, 171)
        click(178, 48)
        assert not visible("Delete File") and nested.is_dir()
        new_file(created.name)
        wait_for(lambda: created.is_file() and visible("new.md — actions — Slate"), "Nested file not opened")
        type_text("saved body")
        save_and_check(lambda: created.read_bytes() == b"saved body")
        new_file(existing.name)
        time.sleep(.1)
        key("Escape")
        assert existing.read_bytes() == b"original"
        assert visible("new.md — actions — Slate")

        # Type into the editor after dismissing the duplicate error. A canceled
        # deletion must retain the dirty buffer as well as the original file.
        click(360, 95)
        key("ctrl+End")
        type_text(" EDIT")
        confirm_delete()
        key("Return")  # Cancel is the default.
        wait_for(lambda: not visible("Delete File"), "Delete confirmation did not cancel")
        assert created.read_bytes() == b"saved body"
        save_and_check(lambda: created.read_bytes() == b"saved body EDIT")

        # Mutating the selected file during confirmation must protect its replacement.
        confirm_delete()
        replaced = nested / "replacement"
        replaced.write_text("new owner", encoding="utf-8")
        replaced.replace(created)
        key("alt+d")
        time.sleep(.1)
        key("Escape")
        assert created.read_bytes() == b"new owner"
        confirm_delete()
        key("alt+d")
        wait_for(lambda: not created.exists(), "Confirmed file survived deletion")
        wait_for(lambda: visible("Untitled — actions — Slate"), "Deleted active document remained open")
        assert existing.read_bytes() == b"original" and nested.is_dir()
        key("ctrl+p")
        type_text("existing.txt")
        def open_result():
            if visible("existing.txt — actions — Slate"):
                return True
            key("Return")
            return False
        wait_for(open_result, "Could not activate the file search result")
        confirm_delete()
        key("alt+d")
        wait_for(lambda: not existing.exists() and visible("Untitled — actions — Slate"),
                 "Deleting a search result did not update the workspace")
        # No stale result can be acted on again.
        click(178, 48)
        assert not visible("Delete File") and nested.is_dir()
        key("ctrl+w")
        assert proc.wait(timeout=5) == 0
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()

if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="slate-ui-") as path:
        exercise(str(Path(sys.argv[1]).resolve()), Path(path))
        workspace_actions(str(Path(sys.argv[1]).resolve()), Path(path))
