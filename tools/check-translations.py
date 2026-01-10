#!/usr/bin/env python3
# 
##############################################################################
# Copyright (c) 2022-2026
#
# Author(s):
#  ChatGPT
#  ann0see
#  The Jamulus Development Team
#
##############################################################################
#
# This program is free software; you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation; either version 2 of the License, or (at your option) any later
# version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
# details.
#
# You should have received a copy of the GNU General Public License along with
# this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
#
##############################################################################

import re
import xml.etree.ElementTree as ET
from pathlib import Path

TS_DIR = Path("src/translation")
TS_FILES = sorted(TS_DIR.glob("translation_*.ts"))

placeholder_re = re.compile(r"%\d+")
html_tag_re = re.compile(r"<[^>]+>")

total_warnings = 0
file_warnings = {}


def gh_warning(file_path, line, message):
    # Output GitHub Actions annotation
    print(f"::warning file={file_path},line={line}::{message}")


print("== Qt Translation files validation (GitHub Actions annotations) ==")

for ts_file in TS_FILES:
    warnings = 0
    file_warnings[ts_file.name] = 0

    # Pre-read the file lines to approximate line numbers
    try:
        with ts_file.open(encoding="utf-8") as f:
            lines = f.readlines()
    except Exception as e:
        gh_warning(ts_file, 0, f"Could not read file: {e}")
        warnings += 1
        file_warnings[ts_file.name] = warnings
        total_warnings += warnings
        continue

    # Parse XML
    try:
        tree = ET.parse(ts_file)
        root = tree.getroot()
    except ET.ParseError as e:
        gh_warning(ts_file, 0, f"Could not parse XML: {e}")
        warnings += 1
        file_warnings[ts_file.name] = warnings
        total_warnings += warnings
        continue

    # Language header
    file_lang = ts_file.stem.replace("translation_", "")
    lang_attr = root.attrib.get("language", "")
    if lang_attr != file_lang:
        gh_warning(
            ts_file,
            0,
            f"Language header '{lang_attr}' does not match filename '{file_lang}'",
        )
        warnings += 1

    # Iterate messages
    message_index = 0
    line_idx = 0
    for context in root.findall("context"):
        for message in context.findall("message"):
            message_index += 1

            # Approximate line number by searching for <message> after last index
            approx_line = 0
            for i in range(line_idx, len(lines)):
                if "<message" in lines[i]:
                    approx_line = i + 1
                    line_idx = i + 1
                    break

            source_elem = message.find("source")
            trans_elem = message.find("translation")
            if source_elem is None or trans_elem is None:
                continue

            source_text = source_elem.text or ""
            trans_text = trans_elem.text or ""
            trans_type = trans_elem.attrib.get("type", "")

            excerpt = source_text[:30].replace("\n", " ")

            # Empty translation
            if trans_text.strip() == "" and trans_type != "unfinished":
                gh_warning(
                    ts_file,
                    approx_line,
                    f"{file_lang}: empty translation for '{excerpt}...'",
                )
                warnings += 1

            # Placeholder integrity
            src_ph = sorted(set(placeholder_re.findall(source_text)))
            tr_ph = sorted(set(placeholder_re.findall(trans_text)))
            if src_ph != tr_ph:
                gh_warning(
                    ts_file,
                    approx_line,
                    f"{file_lang}: placeholder mismatch for '{excerpt}...'\n  Source: {source_text}\n  Translation: {trans_text}",
                )
                warnings += 1

            # HTML parity
            src_html = html_tag_re.findall(source_text)
            tr_html = html_tag_re.findall(trans_text)
            if src_html and not tr_html and trans_type != "unfinished":
                gh_warning(
                    ts_file,
                    approx_line,
                    f"{file_lang}: HTML missing in translation for '{excerpt}...'\n  Source: {source_text}\n  Translation: {trans_text}",
                )
                warnings += 1

    file_warnings[ts_file.name] = warnings
    total_warnings += warnings

# Summary (normal output)
print("\n== Summary ==")
for fname, wcount in file_warnings.items():
    print(f"* {fname}: {wcount} warning(s)")
print(f"Total warnings: {total_warnings}")
