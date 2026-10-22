#!/usr/bin/env python3
"""Check the extension version and optionally extract release notes for a tag."""

import argparse
import json
from pathlib import Path
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--tag', help='expected Git tag, including its v prefix')
parser.add_argument('--notes-file', type=Path, help='write the matching changelog entry')
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent

control = (root / 'pg_describe.control').read_text()
match = re.search(r"^default_version\s*=\s*'([^']+)'", control, re.MULTILINE)
if match is None:
    parser.error('pg_describe.control must declare default_version')
version = match.group(1)
if re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', version) is None:
    parser.error('default_version must be MAJOR.MINOR.PATCH')
if args.tag is not None and args.tag != f'v{version}':
    parser.error(f'tag {args.tag} does not match v{version}')

meta = json.loads((root / 'META.json').read_text())
provided = meta['provides']['pg_describe']
installation = f'sql/pg_describe--{version}.sql'
if meta['version'] != version or provided['version'] != version:
    parser.error('META.json versions must match default_version')
if provided['file'] != installation or not (root / installation).is_file():
    parser.error(f'META.json must reference an existing {installation}')

changelog = (root / 'CHANGELOG.md').read_text()
entry = re.search(rf'^## {re.escape(version)}\n(.*?)(?=^## |\Z)', changelog, re.MULTILINE | re.DOTALL)
if entry is None or not entry.group(1).strip():
    parser.error(f'CHANGELOG.md must contain notes under ## {version}')
if args.notes_file is not None:
    args.notes_file.write_text(entry.group(1).strip() + '\n')
print(f'Validated pg_describe {version}')
