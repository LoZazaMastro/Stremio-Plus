#!/usr/bin/env python3
"""Validate native-title identity before signing or packaging."""
import json
from pathlib import Path
import re
import sys

data = json.loads(Path(sys.argv[1]).read_text())
title = data.get('titleId', '')
if not re.fullmatch(r'PPSA\d{5}', title) or data.get('conceptId') != title[4:]:
    raise SystemExit('titleId and conceptId must describe the same PPSA title.')
content = data.get('contentId', '')
if not re.fullmatch(r'[A-Z]{2}\d{4}-PPSA\d{5}_00-[A-Z0-9]{16}', content) or title not in content:
    raise SystemExit('Invalid contentId or mismatched titleId.')
if not re.fullmatch(r'\d{2}\.\d{3}\.\d{3}', data.get('contentVersion', '')):
    raise SystemExit('Invalid contentVersion.')
if not re.fullmatch(r'\d{2}\.\d{2}', data.get('masterVersion', '')):
    raise SystemExit('Invalid masterVersion.')
if (data.get('applicationCategoryType'), data.get('contentBadgeType')) != (65536, 2):
    raise SystemExit('Stremio Plus must retain the native media-app category and badge.')
if 'gameIntent' in data:
    raise SystemExit('A media-app manifest must not declare a gameIntent.')
size = data.get('downloadDataSize')
if isinstance(size, bool) or not isinstance(size, int) or size != 0:
    raise SystemExit('downloadDataSize must be 0: Stremio Plus stores data on demand outside /download0.')
localized = data.get('localizedParameters', {})
default = localized.get('defaultLanguage', '')
if localized.get(default, {}).get('titleName') != 'Stremio Plus':
    raise SystemExit('The default application name must be Stremio Plus.')
if localized.get('en-US', {}).get('titleName') != 'Stremio Plus':
    raise SystemExit('The English application name must be Stremio Plus.')
print(f'Metadata validated: Stremio Plus {title}, {data["contentVersion"]}.')
