#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""PROTOTYPE's keyboard and mouse layout: the game's actions, the PS4 button each one is on, and
the keys and mouse buttons that press it.

One source for the launcher's Controls page, run.py (THEORDER_KEY_MAP for the runtime,
src/runtime_pad.c) and the in-game prompts (scripts/proto_prompts.py). The actions and their
buttons come from the game's own text: its tutorials name a button symbol per action
(BJU_SYMBOL jump, BAT_SYMBOL attack, ... in the text bible).

A layout is {action id: [key, ...]}; keys are SDL key names ("Space", "Left Shift", "E") or the
mouse names below. Saved layouts only hold the actions the player changed.
"""
import json

MOUSE_KEYS = ('Mouse Left', 'Mouse Right', 'Mouse Middle', 'Mouse X1', 'Mouse X2', 'Wheel Up', 'Wheel Down')
SLOTS = 2  # keys per action on the Controls page

# (id, PS4 target in THEORDER_KEY_MAP terms, English name, Russian name, default keys, group)
ACTIONS = (
    ('move_forward', 'lstick_up', 'Move forward', 'Вперёд', ['W'], 'move'),
    ('move_back', 'lstick_down', 'Move back', 'Назад', ['S'], 'move'),
    ('move_left', 'lstick_left', 'Move left', 'Влево', ['A'], 'move'),
    ('move_right', 'lstick_right', 'Move right', 'Вправо', ['D'], 'move'),
    ('sprint', 'r2', 'Sprint, glide (hold)', 'Бег, планирование (удерживать)', ['Left Shift'], 'move'),
    ('jump', 'cross', 'Jump', 'Прыжок', ['Space'], 'move'),
    ('attack', 'square', 'Attack', 'Атака', ['Mouse Left'], 'fight'),
    ('special', 'triangle', 'Special attack, consume', 'Особая атака, поглощение', ['Mouse Right'], 'fight'),
    ('grab', 'circle', 'Grab, throw', 'Схватить, бросить', ['E'], 'fight'),
    ('target', 'l2', 'Target lock, aim (hold)', 'Прицел (удерживать)', ['Q'], 'fight'),
    ('power_wheel', 'l1', 'Power wheel (hold)', 'Колесо навыков (удерживать)', ['Tab'], 'powers'),
    ('ability_menu', 'r1', 'Disguise menu (hold), vehicle weapon', 'Меню маскировки (удерживать), оружие техники',
     ['F'], 'powers'),
    ('power_offense', 'up', 'Offensive power on/off', 'Навык атаки вкл./выкл.', ['1'], 'powers'),
    ('power_vision', 'right', 'Vision power on/off', 'Сенсорный навык вкл./выкл.', ['2'], 'powers'),
    ('power_defense', 'down', 'Defensive power on/off', 'Навык защиты вкл./выкл.', ['3'], 'powers'),
    ('disguise', 'left', 'Disguise', 'Маскировка', ['4', 'Z'], 'powers'),
    ('camera_reset', 'r3', 'Reset camera', 'Сброс камеры', ['Mouse Middle', 'V'], 'other'),
    ('l3', 'l3', 'L3 (left stick click)', 'L3 (нажатие левого стика)', ['C'], 'other'),
    ('pause', 'options', 'Pause', 'Пауза', ['Escape'], 'other'),
    # The touchpad (the 360's Back) opens the Web of Intrigue (WOI_TUT_EXP_9: "press · to enter
    # or exit the Web of Intrigue"); the PC version had it on Tab.
    ('web_of_intrigue', 'touchpad', 'Web of Intrigue', 'Цепь событий', ['J'], 'other'),
    ('menu_confirm', 'cross', 'Menu: confirm', 'Меню: выбрать', ['Return'], 'menu'),
    ('menu_back', 'circle', 'Menu: back', 'Меню: назад', ['Backspace'], 'menu'),
    ('menu_up', 'up', 'Menu: up', 'Меню: вверх', ['Up'], 'menu'),
    ('menu_down', 'down', 'Menu: down', 'Меню: вниз', ['Down'], 'menu'),
    ('menu_left', 'left', 'Menu: left', 'Меню: влево', ['Left'], 'menu'),
    ('menu_right', 'right', 'Menu: right', 'Меню: вправо', ['Right'], 'menu'),
)
ACTION_IDS = tuple(a[0] for a in ACTIONS)
GROUPS = (('move', 'Movement', 'Движение'), ('fight', 'Combat', 'Бой'), ('powers', 'Powers', 'Навыки'),
          ('other', 'Other', 'Другое'), ('menu', 'Menus', 'Меню'))

# The game's button symbols (text bible <name>_SYMBOL: the character its font draws as the
# PS4 button) and the PS4 target each one stands for.
SYMBOLS = {
    '◊': 'cross',      # BJU: jump
    '£': 'square',     # BAT: attack
    '³': 'triangle',   # BSP: special attack
    '±': 'circle',     # BGR: grab
    '¹': 'l1',         # BL1: power wheel
    '¥': 'r1',         # BR1: disguise menu
    '»': 'l2',         # BL2: target
    '¼': 'r2',         # BR2: sprint
    '§': 'r3',         # BR3: camera reset
    '·': 'touchpad',   # BSE: the Web of Intrigue
    '½': 'up', '¯': 'right', '¾': 'down', '¬': 'left',  # DPU, DPR, DPD, DPL
    '´': 'dpad',       # DPA: the four quick slots
    'µ': 'lstick',     # ALE: move
    '¶': 'rstick',     # ARI: camera (the mouse)
}

KEY_NAMES = {  # key: (English, Russian) for the prompts
    'Space': ('SPACE', 'ПРОБЕЛ'), 'Left Shift': ('SHIFT', 'SHIFT'), 'Right Shift': ('R-SHIFT', 'ПР. SHIFT'),
    'Left Ctrl': ('CTRL', 'CTRL'), 'Right Ctrl': ('R-CTRL', 'ПР. CTRL'), 'Left Alt': ('ALT', 'ALT'),
    'Right Alt': ('R-ALT', 'ПР. ALT'), 'Tab': ('TAB', 'TAB'), 'Escape': ('ESC', 'ESC'),
    'Return': ('ENTER', 'ENTER'), 'Backspace': ('BACKSPACE', 'BACKSPACE'), 'CapsLock': ('CAPS LOCK', 'CAPS LOCK'),
    'Up': ('UP', 'ВВЕРХ'), 'Down': ('DOWN', 'ВНИЗ'), 'Left': ('LEFT', 'ВЛЕВО'), 'Right': ('RIGHT', 'ВПРАВО'),
    'Mouse Left': ('LMB', 'ЛКМ'), 'Mouse Right': ('RMB', 'ПКМ'), 'Mouse Middle': ('MMB', 'СКМ'),
    'Mouse X1': ('MOUSE 4', 'КНОПКА МЫШИ 4'), 'Mouse X2': ('MOUSE 5', 'КНОПКА МЫШИ 5'),
    'Wheel Up': ('WHEEL UP', 'КОЛЕСО ВВЕРХ'), 'Wheel Down': ('WHEEL DOWN', 'КОЛЕСО ВНИЗ'),
}


def defaults():
    return {a[0]: list(a[4]) for a in ACTIONS}


def layout(saved=None):
    """The full layout: the defaults with the player's changes (`saved`, a dict or its JSON)."""
    if isinstance(saved, str):
        try:
            saved = json.loads(saved) if saved.strip() else {}
        except ValueError:
            saved = {}
    result = defaults()
    for action, keys in (saved or {}).items():
        if action in result and isinstance(keys, list):
            result[action] = [k for k in keys if isinstance(k, str) and k][:SLOTS]
    return result


def changes(full):
    """What `full` changes against the defaults (what the launcher saves)."""
    base = defaults()
    return {a: keys for a, keys in full.items() if a in base and keys != base[a]}


def conflicts(full):
    """{key: [action, ...]} for keys on more than one action of the same PS4 button group
    (a key on two different PS4 buttons presses both: almost always a mistake)."""
    targets = {a[0]: a[1] for a in ACTIONS}
    seen = {}
    for action, keys in full.items():
        for key in keys:
            seen.setdefault(key.casefold(), []).append(action)
    return {key: actions for key, actions in seen.items()
            if len({targets[a] for a in actions}) > 1}


def key_map_spec(full):
    """THEORDER_KEY_MAP for the runtime: every binding, replacing the built-in layout ("!")."""
    targets = {a[0]: a[1] for a in ACTIONS}
    entries = []
    for action in ACTION_IDS:
        for key in full.get(action, []):
            entries.append(f'{targets[action]}={key}')
    return '!' + ','.join(entries)


def keys_for(full, target, menu=False):
    """The keys on a PS4 target, gameplay actions first (their first key is the prompt's), or
    with `menu` the menu actions first."""
    found = []
    order = sorted(ACTIONS, key=lambda a: a[5] != 'menu') if menu else ACTIONS
    for a in order:
        if a[1] == target:
            found.extend(k for k in full.get(a[0], []) if k not in found)
    return found


def key_label(key, russian=False):
    names = KEY_NAMES.get(key)
    if names:
        return names[1 if russian else 0]
    return key.upper()
