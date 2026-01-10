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
import os
from github import Github
from collections import defaultdict

# --- Configuration ---
TS_DIR = Path("src/translation")
TS_FILES = sorted(TS_DIR.glob("translation_*.ts"))

PLACEHOLDER_RE = re.compile(r"%\d+")
HTML_TAG_RE = re.compile(r"<[^>]+>")

GH_TOKEN = os.environ.get("GH_TOKEN")
REPO = os.environ.get("GITHUB_REPOSITORY")
PR_NUMBER = os.environ.get("PR_NUMBER")


# --- Helper: get line number of a snippet in a file ---
def get_ts_line_number(ts_file: Path, snippet: str):
    """
    Return the first line number in ts_file containing snippet.
    Returns 0 if not found.
    """
    try:
        with ts_file.open(encoding="utf-8") as f:
            for idx, line in enumerate(f, start=1):
                if snippet in line:
                    return idx
    except Exception:
        return 0
    return 0


# --- Detect warnings in a TS file ---
def detect_warnings(ts_file):
    warnings = []
    try:
        tree = ET.parse(ts_file)
        root = tree.getroot()
    except Exception as e:
        warnings.append({"file": ts_file, "line": 0, "message": f"XML parse error: {e}"})
        return warnings

    file_lang = ts_file.stem.replace("translation_", "")
    if root.attrib.get("language", "") != file_lang:
        warnings.append({"file": ts_file, "line": 0,
                         "message": f"Language header '{root.attrib.get('language','')}' does not match filename '{file_lang}'"})

    for context in root.findall("context"):
        for message in context.findall("message"):
            source = message.findtext("source", "")
            tr = message.findtext("translation", "")
            tr_type = message.find("translation").attrib.get("type", "")

            excerpt = source[:30].replace("\n", " ")

            # Empty translation
            if tr.strip() == "" and tr_type != "unfinished":
                snippet = "<translation>"  # unique enough to find the tag
                line_num = get_ts_line_number(ts_file, snippet)
                warnings.append({"file": ts_file, "line": line_num,
                                 "message": f"{file_lang}: empty translation for '{excerpt}...'"})

            # Placeholder mismatch
            if sorted(set(PLACEHOLDER_RE.findall(source))) != sorted(set(PLACEHOLDER_RE.findall(tr))):
                snippet = source[:40]  # small unique snippet from source
                line_num = get_ts_line_number(ts_file, snippet)
                warnings.append({"file": ts_file, "line": line_num,
                                 "message": f"{file_lang}: placeholder mismatch for '{excerpt}...'\nSource: {source}\nTranslation: {tr}"})

            # HTML check
            if HTML_TAG_RE.findall(source) and not HTML_TAG_RE.findall(tr) and tr_type != "unfinished":
                snippet = source[:40]
                line_num = get_ts_line_number(ts_file, snippet)
                warnings.append({"file": ts_file, "line": line_num,
                                 "message": f"{file_lang}: HTML missing for '{excerpt}...'\nSource: {source}\nTranslation: {tr}"})
    return warnings


# --- Collect all warnings ---
all_warnings = []
for ts_file in TS_FILES:
    all_warnings.extend(detect_warnings(ts_file))


# --- Group warnings by file + line ---
grouped = defaultdict(list)
for w in all_warnings:
    grouped[(w["file"], w["line"])].append(w["message"])


# --- Optional: Post inline comments on PR ---
is_pr = GH_TOKEN and REPO and PR_NUMBER
if is_pr:
    g = Github(GH_TOKEN)
    repo = g.get_repo(REPO)
    pr = repo.get_pull(int(PR_NUMBER))

for (file_path, line), messages in grouped.items():
    body = "\n\n".join(messages)
    if is_pr and line > 0:
        try:
            pr.create_review_comment(body=body,
                                     commit_id=pr.head.sha,
                                     path=str(file_path),
                                     line=line,
                                     side="RIGHT")
        except Exception as e:
            print(f"Failed to post comment {file_path}:{line}: {e}")
    else:
        for msg in messages:
            print(f"{file_path} roughly at line {line}: {msg}")


# --- Summary ---
print("\n== Summary ==")
for (file_path, line), messages in grouped.items():
    print(f"{file_path} roughly at line {line}: {len(messages)} warning(s)")
print(f"Total warnings: {sum(len(m) for m in grouped.values())}")

