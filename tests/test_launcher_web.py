"""The web launcher's backend (launcher/prototype_web.py): saves, settings, keys and mods, without
a window or a game."""
import json
import struct
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import prototype_launcher as L  # noqa: E402
import prototype_web as W  # noqa: E402
import prototype_native as N  # noqa: E402


def param(title, subtitle, detail, mtime):
    data = bytearray(1328)
    for offset, text, size in ((0, title, 128), (128, subtitle, 128), (256, detail, 1024)):
        raw = text.encode('utf-8')[:size - 1]
        data[offset:offset + len(raw)] = raw
    struct.pack_into('<q', data, 1288, mtime)
    return bytes(data)


class BackendTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.patches = [mock.patch.object(L, 'PORT_DIR', root), mock.patch.object(L, 'LOG_FILE', root / 'last_run.log'),
                        mock.patch.object(L, 'CONFIG_DIR', root / 'cfg'), mock.patch.object(L, 'CONFIG_FILE', root / 'cfg' / 'l.json'),
                        mock.patch.object(threading.Thread, 'start', lambda self: None)]
        for p in self.patches:
            p.start()
        saves = root / 'user' / 'savedata' / '1' / 'CUSA06545'
        slot = saves / 'SLOTP1-C.BIN'
        slot.mkdir(parents=True)
        data = bytearray(2000)
        data[0x524:0x52a] = b'e01m01'
        (slot / 'slotP1-C.bin').write_bytes(bytes(data))
        (saves / 'SLOTP1-C.BIN.sce_sys').mkdir()
        (saves / 'SLOTP1-C.BIN.sce_sys' / 'param.bin').write_bytes(
            param('[PROTOTYPE]', 'ПАМЯТЬ В СМЕРТИ( 3 )', 'НИЗКИЙ', 1792395786))
        (saves / 'SLOTP2-B.BIN').mkdir()
        (saves / 'SLOTP2-B.BIN.sce_sys').mkdir()
        (saves / 'SLOTP2-B.BIN.sce_sys' / 'param.bin').write_bytes(
            param('[PROTOTYPE 2]', '(1)(AUTO) (+) MURDER YOUR MAKER', '100%', 1792917872))
        (root / 'mods' / 'clear_sky' / 'shaders').mkdir(parents=True)
        (root / 'mods' / 'clear_sky' / 'shaders' / 'a71ff1ce_p0.frag').write_text('void main() {}')
        self.backend = W.Backend(L)
        self.root = root

    def tearDown(self):
        for p in self.patches:
            p.stop()
        self.tmp.cleanup()

    def test_saves_as_the_games_wrote_them(self):
        saves = self.backend.saves()
        p1, = saves['p1']
        self.assertEqual((p1['slot'], p1['title'], p1['number'], p1['mission'], p1['detail']),
                         ('C', 'ПАМЯТЬ В СМЕРТИ', 3, 'e01m01', 'НИЗКИЙ'))
        p2, = saves['p2']
        self.assertEqual((p2['slot'], p2['title'], p2['number'], p2['auto']), ('B', 'MURDER YOUR MAKER (+)', 1, True))

    def test_settings_are_checked_and_saved(self):
        state = self.backend.act('set', {'key': 'game.fps', 'value': 120})
        self.assertEqual(state['perGame']['p1']['fps'], 120)
        self.assertTrue(state['perGame']['p1']['fps60'])
        self.assertEqual(json.loads(L.CONFIG_FILE.read_text())['per_game']['p1']['fps'], 120)
        with self.assertRaises(ValueError):
            self.backend.act('set', {'key': 'game_dir', 'value': 'C:/'})
        with self.assertRaises(ValueError):
            self.backend.act('set', {'key': 'game.fps', 'value': 77})

    def test_a_key_moves_to_its_new_action(self):
        self.backend.act('key', {'action': 'jump', 'slot': 1, 'key': 'W'})
        actions = {a['id']: a['keys'] for a in self.backend.controls()['actions']}
        self.assertIn('W', actions['jump'])
        self.assertNotIn('W', actions['move_forward'])

    def test_mods_switch_off_through_mods_json(self):
        self.assertTrue(self.backend.mods()['named'][0]['enabled'])
        self.backend.act('mod', {'name': 'clear_sky', 'enabled': False})
        self.assertEqual(json.loads((self.root / 'mods.json').read_text())['disabled'], ['clear_sky'])
        self.assertFalse(self.backend.mods()['named'][0]['enabled'])

    def test_unknown_actions_are_refused(self):
        with self.assertRaises(ValueError):
            self.backend.act('rm_rf', {})


    def test_the_native_window_sets_the_folder(self):
        game = self.root / 'game'
        game.mkdir()
        state = self.backend.act('set_folder', {'path': str(game)})
        self.assertEqual(state['gameDir'], str(game))
        self.assertIn('artDir', state)
        with self.assertRaises(ValueError):
            self.backend.act('set_folder', {'path': str(self.root / 'nowhere')})


class NativeTest(unittest.TestCase):
    """launcher-ui's requests (JSON lines on its stdout) and their answers (on its stdin)."""
    def test_requests_are_answered_by_id(self):
        import io

        class Backend:
            def act(self, name, args):
                if name == 'boom':
                    raise ValueError('no such thing')
                return {'echo': name, 'args': args}

        class Process:
            stdout = io.BytesIO(b'{"id": 1, "action": "poll", "args": {}}\nnot json\n'
                                b'{"id": 2, "action": "boom", "args": {}}\n{"id": 3, "action": "set", "args": {"key": "vsync"}}\n')
            stdin = io.BytesIO()
        process = Process()
        process.stdin.close = lambda: None
        with mock.patch.object(threading.Thread, 'start', lambda self: self.run()):
            self.assertEqual(N.serve(Backend(), process), 3)
        replies = {r['id']: r for r in map(json.loads, process.stdin.getvalue().decode().splitlines())}
        self.assertEqual(replies[1], {'id': 1, 'ok': True, 'result': {'echo': 'poll', 'args': {}}})
        self.assertEqual((replies[2]['ok'], replies[2]['error']), (False, 'no such thing'))
        self.assertEqual(replies[3]['result']['args'], {'key': 'vsync'})


class NewerTest(unittest.TestCase):
    def test_versions(self):
        self.assertTrue(W.Backend._newer('1.1.1', '1.1.0'))
        self.assertFalse(W.Backend._newer('1.0.5', '1.1.0'))
        self.assertFalse(W.Backend._newer('1.1.0', '1.1.0'))


if __name__ == '__main__':
    unittest.main()
