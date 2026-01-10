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

#!/usr/bin/env python3
import xml.etree.ElementTree as ET
from pathlib import Path
import re
from collections import defaultdict

# --- Configuration ---
TS_DIR = Path("src/translation")
TS_FILES = sorted(TS_DIR.glob("translation_*.ts"))

PLACEHOLDER_RE = re.compile(r"%\d+")
HTML_TAG_RE = re.compile(r"<[^>]+>")

# --- Helper: get the exact line content containing a snippet ---
def get_ts_line(ts_file: Path, snippet: str):
    """
    Return the first line from ts_file that contains the snippet.
    Returns empty string if not found.
    """
    try:
        with ts_file.open(encoding="utf-8") as f:
            for line in f:
                if snippet in line:
                    return line.strip()
    except Exception:
        return ""
    return ""


# --- Detect warnings in a TS file ---
def detect_warnings(ts_file):
    warnings = []
    try:
        tree = ET.parse(ts_file)
        root = tree.getroot()
    except Exception as e:
        warnings.append({
            "file": ts_file,
            "line_content": "",
            "message": f"XML parse error: {e}"
        })
        return warnings

    file_lang = ts_file.stem.replace("translation_", "")
    if root.attrib.get("language", "") != file_lang:
        warnings.append({
            "file": ts_file,
            "line_content": "",
            "message": f"Language header '{root.attrib.get('language','')}' does not match filename '{file_lang}'"
        })

    for context in root.findall("context"):
        for message in context.findall("message"):
            source = message.findtext("source", "")
            tr = message.findtext("translation", "")
            tr_type = message.find("translation").attrib.get("type", "")

            # --- Empty translation ---
            if tr.strip() == "" and tr_type != "unfinished":
                line_content = get_ts_line(ts_file, "<translation>")
                warnings.append({
                    "file": ts_file,
                    "line_content": line_content,
                    "message": f"{file_lang}: empty translation"
                })

            # --- Placeholder mismatch ---
            if sorted(set(PLACEHOLDER_RE.findall(source))) != sorted(set(PLACEHOLDER_RE.findall(tr))):
                snippet = source[:40]
                line_content = get_ts_line(ts_file, snippet)
                warnings.append({
                    "file": ts_file,
                    "line_content": line_content,
                    "message": f"{file_lang}: placeholder mismatch\nSource: {source}\nTranslation: {tr}"
                })

            # --- HTML missing ---
            if HTML_TAG_RE.findall(source) and not HTML_TAG_RE.findall(tr) and tr_type != "unfinished":
                snippet = source[:40]
                line_content = get_ts_line(ts_file, snippet)
                warnings.append({
                    "file": ts_file,
                    "line_content": line_content,
                    "message": f"{file_lang}: HTML missing\nSource: {source}\nTranslation: {tr}"
                })

    return warnings


# --- Collect all warnings ---
all_warnings = []
for ts_file in TS_FILES:
    all_warnings.extend(detect_warnings(ts_file))


# --- Output warnings ---
for w in all_warnings:
    print(f"{w['file']} line: {w['line_content']}")
    print(f"WARNING: {w['message']}\n")


# --- Summary ---
print("== Summary ==")
print(f"Total warnings: {len(all_warnings)}")


