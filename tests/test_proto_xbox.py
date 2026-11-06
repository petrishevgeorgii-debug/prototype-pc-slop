"""scripts/proto_xbox.py: glyph shapes and the font layout survive a rewrite (no game files here)."""
import struct
import sys
import unittest

from paths import ROOT
sys.path.insert(0, str(ROOT / 'scripts'))
import proto_xbox as X  # noqa: E402


class ShapeTest(unittest.TestCase):
    RING = [['style', 3, 1, None, None, (100, -300)], ['q', 300, -300, 300, -100], ['l', 300, 0],
            ['l', -50, 0], ['l', 100, -300], ['style', 1, None, None, None, (150, -150)], ['l', 200, -150],
            ['l', 175, -100], ['l', 150, -150]]

    def test_records_round_trip(self):
        data = X.encode_shape(1, 0, self.RING)
        self.assertEqual(X.parse_shape(data), (1, 0, self.RING))

    def test_large_coordinates(self):
        recs = [['style', 3, 1, None, None, (-20000, 9000)], ['l', 21000, 9000], ['l', 21000, -17000],
                ['q', 0, -18000, -20000, 9000]]
        self.assertEqual(X.parse_shape(X.encode_shape(1, 0, recs))[2], recs)

    def test_reverse_keeps_the_points(self):
        sp = X.subpaths(self.RING)[1]
        back = X.reverse(sp)
        self.assertEqual(list(back[0][5]), sp[-1][1:3])
        self.assertAlmostEqual(X.area(back), -X.area(sp))

    def test_bold_a_is_two_contours(self):
        self.assertEqual(len(X.subpaths(X.BOLD_A)), 2)


class FontTest(unittest.TestCase):
    def font(self):
        shapes = [X.encode_shape(1, 0, ShapeTest.RING), X.encode_shape(1, 0, ShapeTest.RING[:5])]
        rects = X._encode_rect((0, 0, 0, 0)) + X._encode_rect((1, 300, -300, 0))
        rest = struct.pack('<hhh', 900, 200, 0) + struct.pack('<hh', 400, 500) + rects + struct.pack('<H', 0)
        return {'id': 3, 'flags': 0x84, 'lang': 1, 'name': b'Test\0', 'n': 2, 'shapes': shapes,
                'codes': [0xb1, 0x41], 'rest': rest}

    def test_write_and_read(self):
        f = self.font()
        body = X.write_font(f)
        back = X.read_font(body, 0, len(body))
        for key in ('id', 'flags', 'name', 'n', 'shapes', 'codes', 'rest'):
            self.assertEqual(back[key], f[key], key)

    def test_layout_advance_and_bounds(self):
        f = self.font()
        X._set_layout(f, {0: (777, (1, 2, 3, 4)), 1: (888, (5, -6, 7, 8))})
        self.assertEqual(X._advances(f), [777, 888])
        first, pos = X._rect(f['rest'], 10)
        second, end = X._rect(f['rest'], pos)
        self.assertEqual(first, (0, 0, 0, 0))  # left empty, as the exporter wrote it
        self.assertEqual(second, (5, 7, -6, 8))
        self.assertEqual(f['rest'][end:], struct.pack('<H', 0))


if __name__ == '__main__':
    unittest.main()
