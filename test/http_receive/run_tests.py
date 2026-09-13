"""Generate fixtures from current production functions, then compile host tests."""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


# Ignore braces in comments, strings, and character literals while finding the
# closing function brace. Preserve the source slice verbatim in the fixture.
TOKENS = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]')


def extract(path, name):
    source = path.read_text(encoding="utf-8")
    matches = list(re.finditer(
        rf"^(?:static[ \t]+)?(?:esp_err_t|unsigned|bool|void)[ \t]+{re.escape(name)}\s*\([^;{{}}]*\)\s*\{{",
        source, re.MULTILINE))
    if len(matches) != 1:
        raise ValueError(f"Expected one definition of {name} in {path}")
    match = matches[0]
    start = match.start()
    depth = 1
    for token in TOKENS.finditer(source, match.end()):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                line = source.count("\n", 0, start) + 1
                return f'#line {line} "{path.as_posix()}"\n' + source[start:token.end()] + "\n"
    raise ValueError(f"Unclosed function {name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    root = tests.parent.parent
    functions = {
        "http_app.c": ["http_status_display_result", "ota_post_handler"],
        "http_gallery.c": ["gallery_upload_begin", "gallery_upload_end",
                           "gallery_upload_busy", "gallery_upload_epoch",
                           "gallery_upload_post_handler", "gallery_delete_image_post_handler",
                           "gallery_delete_post_handler"],
        "http_canvas.c": ["canvas_image_upload_post", "canvas_layout_post"],
    }
    with tempfile.TemporaryDirectory(prefix="msp-http-handlers-") as output:
        output = Path(output)
        fixture = "".join(extract(root / "main" / file, name)
                          for file, names in functions.items() for name in names)
        (output / "production_handlers.inc").write_text(fixture, encoding="utf-8")
        command = shlex.split(args.cc) + [
            "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-parameter", "-Wno-sign-compare",
            f"-I{tests / 'stubs'}", f"-I{root / 'main'}", f"-I{output}",
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        for source, name, mapped_fs in [
            (root / "main/http_receive.c", "receive.o", True),
            (tests / "host_fs.c", "fs.o", False),
            (tests / "test_handlers.c", "tests.o", True),
        ]:
            options = ["-include", str(tests / "host_fs.h")] if mapped_fs else []
            subprocess.run(command + options + ["-c", str(source), "-o", str(output / name)], check=True)
        executable = output / "http_receive_tests"
        subprocess.run(command + [str(output / name) for name in ("receive.o", "fs.o", "tests.o")]
                       + ["-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
