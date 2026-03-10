#!/usr/bin/env python3

import subprocess
import sys
import re
from pathlib import Path
import tempfile

def clean_vtt(path):
    text = []

    with open(path, "r", encoding="utf8") as f:
        for line in f:
            line = line.strip()

            if not line:
                continue
            if line.startswith("WEBVTT"):
                continue
            if "-->" in line:
                continue

            # remove timing tags like <00:11:48.680>
            line = re.sub(r"<\d\d:\d\d:\d\d\.\d+>", "", line)

            # remove html tags like <c>
            line = re.sub(r"<[^>]+>", "", line)

            line = line.strip()

            if line:
                text.append(line)

    # remove duplicate fragments (youtube repeats cues)
    cleaned = []
    prev = None
    for t in text:
        if t != prev:
            cleaned.append(t)
        prev = t

    return " ".join(cleaned)


def main():
    if len(sys.argv) < 2:
        print("usage: yt_transcript.py <youtube_url>")
        sys.exit(1)

    url = sys.argv[1]

    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp) / "sub"

        cmd = [
            "yt-dlp",
            "--write-auto-sub",
            "--sub-lang", "en",
            "--sub-format", "vtt",
            "--skip-download",
            url,
            "-o", str(base)
        ]

        subprocess.run(cmd, check=True)

        vtt = list(Path(tmp).glob("*.vtt"))
        if not vtt:
            print("no subtitles found", file=sys.stderr)
            sys.exit(1)

        transcript = clean_vtt(vtt[0])
        print(transcript)


if __name__ == "__main__":
    main()














