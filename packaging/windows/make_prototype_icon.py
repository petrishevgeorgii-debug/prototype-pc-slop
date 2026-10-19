"""Prototype.exe's icon from the game's own icon0.png (the player's dump): make_prototype_icon.py <game> <out.ico>"""
import sys
from pathlib import Path
from PIL import Image

source = Path(sys.argv[1]) / 'sce_sys' / 'icon0.png'
Image.open(source).convert('RGBA').save(sys.argv[2], sizes=[(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (16, 16)])
