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
import os
import xml.etree.ElementTree as ET
from collections import defaultdict
from pathlib import Path

from github import Github

# --- Configuration ---
# Directory where the TS translation files are stored
TS_DIR = Path("src/translation")
TS_FILES = sorted(TS_DIR.glob("translation_*.ts"))

# Regex patterns for placeholders (%1, %2...) and HTML tags
PLACEHOLDER_RE = re.compile(r"%\d+")
HTML_TAG_RE = re.compile(r"<[^>]+>")

# GitHub environment variables for optional PR commenting
GH_TOKEN = os.environ.get("GH_TOKEN")
REPO = os.environ.get("GITHUB_REPOSITORY")
PR_NUMBER = os.environ.get("PR_NUMBER")


# --- Function: detect warnings in a TS file ---
def detect_warnings(ts_file):
    """
    Scans a TS file and returns a list of warning dictionaries.
    Each warning contains the file, approximate line number, and message.
    """
    warnings = []

    try:
        # Read all lines to help approximate line numbers
        lines = ts_file.read_text(encoding="utf-8").splitlines()
        tree = ET.parse(ts_file)
        root = tree.getroot()
    except Exception as e:
        warnings.append(
            {
                "file": ts_file,
                "line": 0,
                "message": f"Error reading or parsing XML: {e}",
            }
        )
        return warnings

    # Ensure language in the TS header matches the filename
    file_lang = ts_file.stem.replace("translation_", "")
    if root.attrib.get("language", "") != file_lang:
        warnings.append(
            {
                "file": ts_file,
                "line": 0,
                "message": f"Language header mismatch '{root.attrib.get('language', '')}' != '{file_lang}'",
            }
        )

    line_idx = 0
    for context in root.findall("context"):
        for message in context.findall("message"):
            # Approximate line number: first line containing <message>
            approx_line = next(
                (i + 1 for i in range(line_idx, len(lines)) if "<message" in lines[i]),
                0,
            )
            line_idx = approx_line - 1 if approx_line else line_idx

            src = message.findtext("source", "")
            tr = message.findtext("translation", "")
            tr_type = message.find("translation").attrib.get("type", "")

            excerpt = src[:30].replace("\n", " ")

            # Warn if translation is empty but not marked unfinished
            if tr.strip() == "" and tr_type != "unfinished":
                warnings.append(
                    {
                        "file": ts_file,
                        "line": approx_line,
                        "message": f"{file_lang}: empty translation for '{excerpt}...'",
                    }
                )

            # Check placeholder integrity (%1, %2, etc.)
            if sorted(set(PLACEHOLDER_RE.findall(src))) != sorted(
                set(PLACEHOLDER_RE.findall(tr))
            ):
                warnings.append(
                    {
                        "file": ts_file,
                        "line": approx_line,
                        "message": f"{file_lang}: placeholder mismatch for '{excerpt}...'\nSource: {src}\nTranslation: {tr}",
                    }
                )

            # Ensure any HTML tags in source exist in translation
            if (
                HTML_TAG_RE.findall(src)
                and not HTML_TAG_RE.findall(tr)
                and tr_type != "unfinished"
            ):
                warnings.append(
                    {
                        "file": ts_file,
                        "line": approx_line,
                        "message": f"{file_lang}: HTML missing for '{excerpt}...'\nSource: {src}\nTranslation: {tr}",
                    }
                )

    return warnings


# --- Collect warnings from all TS files ---
all_warnings = []
for ts_file in TS_FILES:
    all_warnings.extend(detect_warnings(ts_file))


# --- Group warnings by file and line for cleaner output or PR comments ---
grouped = defaultdict(list)
for w in all_warnings:
    grouped[(w["file"], w["line"])].append(w["message"])


# --- Diff-aware: collect added/modified lines in the PR ---
diff_lines = defaultdict(set)
is_pr = GH_TOKEN and REPO and PR_NUMBER
if is_pr:
    g = Github(GH_TOKEN)
    repo = g.get_repo(REPO)
    pr = repo.get_pull(int(PR_NUMBER))

    # For each file, parse the patch to collect line numbers added/modified
    for f in pr.get_files():
        if not f.filename.startswith(str(TS_DIR)):
            continue
        # Split patch into hunks
        for hunk in f.patch.split("@@"):
            lines = hunk.splitlines()
            if not lines:
                continue
            header = lines[0].strip()
            if "+" not in header:
                continue
            # Start line number in the new file
            start = int(header.split("+")[1].split(",")[0])
            line_no = start
            # For each added line (starting with '+'), store its line number
            for line in lines[1:]:
                if line.startswith("+") and not line.startswith("+++"):
                    diff_lines[f.filename].add(line_no)
                if not line.startswith("-"):
                    line_no += 1


# --- Output warnings or post PR comments ---
for (file_path, line), messages in grouped.items():
    body = "\n\n".join(messages)
    if is_pr:
        # Only comment on lines that were added/modified in this PR
        if str(file_path) in diff_lines and line in diff_lines[str(file_path)]:
            try:
                pr.create_review_comment(
                    body=body,
                    commit_id=pr.head.sha,
                    path=str(file_path),
                    line=line,
                    side="RIGHT",
                )
            except Exception as e:
                print(f"Failed to post comment {file_path}:{line}: {e}")
    else:
        # Print to stdout if not running in a PR
        for msg in messages:
            print(f"{file_path} roughly at line {line}: {msg}")


# --- Summary ---
print("\n== Summary ==")
for (file_path, line), messages in grouped.items():
    print(f"{file_path} roughly at line {line}: {len(messages)} warning(s)")
print(f"Total warnings: {sum(len(m) for m in grouped.values())}")
