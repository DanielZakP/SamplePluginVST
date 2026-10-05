#!/usr/bin/env python3
"""Regenerates THIRD_PARTY_NOTICES.md from the license files of the libraries compiled into
ChopLab. Run it after upgrading JUCE, whisper.cpp, demucs.cpp, Eigen or Signalsmith (with their
sources in libs/)."""

import pathlib
import re

root = pathlib.Path(__file__).resolve().parent.parent
libs = root / "libs"
juce = libs / "JUCE" / "modules"


def read(path):
    return (path).read_text(encoding="utf-8", errors="replace").strip("\n")


def section(text, start, end):
    m = re.search(re.escape(start) + r"\n=+\n(.*?)\n" + re.escape(end) + r"\n=+", text, re.S)
    return m.group(1).strip("\n") if m else text


zlib_readme = read(juce / "juce_core/zip/zlib/README")
zlib = zlib_readme[zlib_readme.index("(C) 1995"):zlib_readme.index("If you use the zlib library")].rstrip()

components = [
    ("JUCE", "Plugin framework (all of ChopLab's audio, plugin and UI plumbing)", "AGPLv3, see LICENSE",
     "Copyright (c) Raw Material Software Limited. Used under the GNU Affero General Public License v3; the full text is in LICENSE."),
    ("Steinberg VST3 SDK", "VST3 plugin interface (bundled with JUCE)", "MIT",
     read(juce / "juce_audio_processors_headless/format_types/VST3_SDK/LICENSE.txt")),
    ("whisper.cpp and ggml", "Speech-to-text for lyrics", "MIT", read(libs / "whisper.cpp/LICENSE")),
    ("Whisper models", "Speech model files, downloaded by the plugin on first use (not included in the plugin)", "MIT",
     "Copyright (c) 2022 OpenAI. Released under the MIT License (https://github.com/openai/whisper/blob/main/LICENSE). "
     "ChopLab downloads the ggml conversions published by the whisper.cpp project."),
    ("demucs.cpp", "Stem separation (Demucs v4 in plain C++)", "MIT", read(libs / "demucs.cpp/LICENSE")),
    ("Eigen", "Maths used by demucs.cpp", "MPL 2.0",
     "Eigen is used unmodified (version 3.4.1). Its source code is available at "
     "https://gitlab.com/libeigen/eigen/-/tree/3.4.1\n\n" + read(libs / "eigen/COPYING.MPL2")),
    ("Demucs models", "Stem separation model file, downloaded by the plugin on first use (not included in the plugin)", "MIT",
     "Copyright (c) Meta Platforms, Inc. and affiliates. Hybrid Transformer Demucs (htdemucs) by Alexandre Defossez and "
     "Simon Rouard, released under the MIT License (https://github.com/facebookresearch/demucs/blob/main/LICENSE). "
     "ChopLab downloads the conversion for demucs.cpp published at https://huggingface.co/datasets/Retrobear/demucs.cpp."),
    ("Signalsmith Stretch", "Pitch shifting and time stretching", "MIT", read(libs / "signalsmith-stretch/LICENSE.txt")),
    ("Signalsmith Linear", "FFT and maths used by Signalsmith Stretch", "MIT", read(libs / "signalsmith-linear/LICENSE.txt")),
    ("FLAC", "Reading FLAC files and storing samples inside projects (bundled with JUCE)", "BSD",
     read(juce / "juce_audio_formats/codecs/flac/Flac Licence.txt")),
    ("Ogg Vorbis", "Reading OGG files (bundled with JUCE)", "BSD",
     read(juce / "juce_audio_formats/codecs/oggvorbis/Ogg Vorbis Licence.txt")),
    ("zlib", "Compression (bundled with JUCE)", "zlib", zlib),
    ("libpng", "PNG images (bundled with JUCE)", "libpng", read(juce / "juce_graphics/image_formats/pnglib/LICENSE")),
    ("Independent JPEG Group libjpeg", "JPEG images (bundled with JUCE)", "IJG",
     "This software is based in part on the work of the Independent JPEG Group.\n\n"
     + section(read(juce / "juce_graphics/image_formats/jpglib/README"), "LEGAL ISSUES", "REFERENCES")),
    ("HarfBuzz", "Text shaping (bundled with JUCE)", "Old MIT", read(juce / "juce_graphics/fonts/harfbuzz/COPYING")),
    ("SheenBidi", "Bidirectional text (bundled with JUCE)", "Apache 2.0", read(juce / "juce_graphics/unicode/sheenbidi/LICENSE")),
]

out = ["# Third-party notices", "",
       "ChopLab is free software under the GNU Affero General Public License v3 (see LICENSE).",
       "It is built with the following third-party software, whose licenses require these notices",
       "to be included with every copy.", "",
       "| Component | Used for | License |", "|---|---|---|"]
out += [f"| {name} | {use} | {lic} |" for name, use, lic, _ in components]
for name, _, lic, text in components:
    out += ["", "---", "", f"## {name} ({lic})", "", "```", text, "```"]
(root / "THIRD_PARTY_NOTICES.md").write_text("\n".join(out) + "\n", encoding="utf-8")
print("wrote", root / "THIRD_PARTY_NOTICES.md")
