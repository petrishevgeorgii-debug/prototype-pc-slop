"""First run: the launcher starts with the game folder setup used (seed_launcher.py <game>)."""
import json
import os
import sys
from pathlib import Path

config = Path(os.environ.get('APPDATA', Path.home())) / 'prototype-port' / 'launcher.json'
game = sys.argv[1] if len(sys.argv) > 1 else ''
if game and Path(game, 'eboot.bin').is_file():
    values = {}
    try:
        values = json.loads(config.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        pass
    if not values.get('game_dir'):
        values['game_dir'] = str(Path(game))
        config.parent.mkdir(parents=True, exist_ok=True)
        config.write_text(json.dumps(values, indent=2), encoding='utf-8')
