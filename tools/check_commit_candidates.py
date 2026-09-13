#!/usr/bin/env python3
"""Review an explicit public-source candidate set, without staging or committing."""
import argparse
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PUBLIC_DIRS = ("main/", "web/", "test/", "tools/", "docs/", "prototypes/")
PUBLIC_NEW_ROOT = {".gitattributes", "简易使用说明书.md"}
PRIVATE_DIRS = (".codex/", "下一个AI交接/", "output/", "outputs/", "build/")
RELEASE_LIMITS = {
    "release/epaper_uploader_full_16MB.bin": 16 * 1024 * 1024,
    "release/epaper_uploader_full_16MB.zip": 16 * 1024 * 1024,
    "release/web_upgrade.bin": 3 * 1024 * 1024,
    "release/web_upgrade.zip": 3 * 1024 * 1024,
}
TEXT_EXT = {".c", ".h", ".js", ".cjs", ".json", ".html", ".md", ".py", ".ps1", ".sh", ".txt", ".toml", ".yml", ".yaml", ".csv"}
RULES = {
    "private-key": re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----"),
    "access-token": re.compile(r"\b(?:sk-|ghp_|github_pat_|hf_)[A-Za-z0-9_-]{20,}"),
    "aws-key": re.compile(r"\bAKIA[0-9A-Z]{16}\b"),
    "credential-url": re.compile(r"https?://[^\s/@:]+:[^\s/@]+@"),
    "private-user-path": re.compile(r"[A-Za-z]:[\\/]+Users[\\/]+"),
}
CREDENTIAL_LITERAL = re.compile(r'''(?i)\b(?:password|passwd|secret|api[_-]?key|access[_-]?token)\b["']?\s*[:=]\s*["']([^"'\r\n]{8,})["']''')
PLACEHOLDERS = {"YOUR_API_KEY", "YOUR_PASSWORD", "your-api-key", "your-password", "replace-me", "local-only"}


def git(*args):
    return subprocess.check_output(["git", "-c", "core.safecrlf=false", *args], cwd=ROOT)


def names(*args):
    return {p.decode("utf-8") for p in git(*args, "-z").split(b"\0") if p}


def private(path):
    return path.startswith(PRIVATE_DIRS) or path.startswith("测试固件-")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--staged", action="store_true")
    parser.add_argument("--release", action="store_true", help="Allow only the four fixed official firmware paths within their size limits")
    parser.add_argument("--write-list", help="Write a NUL-separated reviewed pathspec under output/")
    parser.add_argument("--copy-to", help="Create a fresh source-only test checkout under output/")
    args = parser.parse_args()
    tracked = names("ls-files")
    candidates = names("diff", "--cached", "--name-only") if args.staged else (
        names("diff", "--name-only") | names("ls-files", "-m") | names("ls-files", "--others", "--exclude-standard"))
    findings = []
    if args.staged:
        for path in names("ls-files", "--others", "--exclude-standard"):
            if path in PUBLIC_NEW_ROOT or path.startswith(PUBLIC_DIRS):
                findings.append({"file": path, "rule": "unstaged-public-source"})
    for path in sorted(candidates):
        file = ROOT / path
        if private(path) or not (path in tracked or path in PUBLIC_NEW_ROOT or path.startswith(PUBLIC_DIRS)):
            findings.append({"file": path, "rule": "not-public-candidate"})
            continue
        if not file.exists():
            continue
        if file.is_symlink() or not file.resolve().is_relative_to(ROOT):
            findings.append({"file": path, "rule": "external-path"})
            continue
        data = git("show", ":" + path) if args.staged else file.read_bytes()
        suffix = PurePosixPath(path).suffix.lower()
        if file.name in {".env", "id_rsa", "id_ed25519"} or suffix in {".key", ".p12", ".pfx"}:
            findings.append({"file": path, "rule": "credential-file"})
        release_limit = RELEASE_LIMITS.get(path) if args.release else None
        if release_limit is not None and (not data or len(data) > release_limit):
            findings.append({"file": path, "rule": "invalid-release-size", "bytes": len(data)})
        elif release_limit is None and (len(data) > 1024 * 1024 or suffix in {".bin", ".zip", ".elf", ".map", ".mef"}):
            findings.append({"file": path, "rule": "large-or-generated-file", "bytes": len(data)})
        if suffix in TEXT_EXT:
            for line_no, line in enumerate(data.decode("utf-8", errors="replace").splitlines(), 1):
                for rule, pattern in RULES.items():
                    if pattern.search(line):
                        findings.append({"file": path, "line": line_no, "rule": rule})
                literal = CREDENTIAL_LITERAL.search(line)
                if literal and literal.group(1) not in PLACEHOLDERS:
                    findings.append({"file": path, "line": line_no, "rule": "credential-literal"})
    license_file = ROOT / "docs/licenses/lucide-static.txt"
    if "Lucide static icons" in (ROOT / "web/index.html").read_text(encoding="utf-8") and not license_file.is_file():
        findings.append({"file": "docs/licenses/lucide-static.txt", "rule": "missing-license"})
    print(json.dumps({"scanned": len(candidates), "findings": findings}, ensure_ascii=False))
    if findings:
        return 1
    if args.write_list:
        target = (ROOT / args.write_list).resolve()
        if not target.is_relative_to(ROOT / "output"):
            raise ValueError("Candidate lists must remain under output/")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(b"\0".join(p.encode("utf-8") for p in sorted(candidates)) + b"\0")
    if args.copy_to:
        target = (ROOT / args.copy_to).resolve()
        if not target.is_relative_to(ROOT / "output") or target.exists():
            raise ValueError("Fresh checkout must be a new directory under output/")
        target.mkdir(parents=True)
        for path in sorted(tracked | candidates):
            source = ROOT / path
            if private(path) or not source.is_file():
                continue
            if source.is_symlink() or not source.resolve().is_relative_to(ROOT):
                raise ValueError("External checkout path: " + path)
            dest = target / path
            dest.parent.mkdir(parents=True, exist_ok=True)
            if args.staged:
                dest.write_bytes(git("show", ":" + path))
            else:
                shutil.copyfile(source, dest)
        print(json.dumps({"checkout": str(target), "cachesCopied": False}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
