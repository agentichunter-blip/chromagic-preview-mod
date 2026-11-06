"""Regression tests for conservative ROM-copy repairs."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from repair_library import LOGO, global_sum, repair_rom, title_card
from prepare_box_art import convert


class LibraryRepairTest(unittest.TestCase):
    def setUp(self):
        data = bytearray(32768)
        data[0x104:0x134] = LOGO
        data[0x14d] = (-sum(data[0x134:0x14d])-25) & 255
        data[0x14e:0x150] = global_sum(data).to_bytes(2, 'big')
        self.data = bytes(data)

    def test_verified_padding_and_repetition(self):
        for suffix in (b'\0'*32768, b'\xff'*32768, self.data):
            fixed, action = repair_rom(self.data+suffix, 'file_size_mismatch')
            self.assertEqual(fixed, self.data)
            self.assertTrue(action)

    def test_preserves_unique_data_and_truncated_roms(self):
        for data in (self.data+b'\x31'*32768, self.data[:12000]):
            self.assertEqual(repair_rom(data, 'file_size_mismatch'), (data, ''))

    def test_requires_original_global_checksum_for_trimming(self):
        data = bytearray(self.data); data[1000] ^= 1
        data = bytes(data)+bytes(32768)
        self.assertEqual(repair_rom(data, 'file_size_mismatch'), (data, ''))

    def test_repairs_only_header_checksum_byte(self):
        data = bytearray(self.data); data[0x14d] ^= 1
        fixed, action = repair_rom(bytes(data), 'invalid_header_checksum')
        self.assertEqual(fixed, self.data)
        self.assertTrue(action)
        data[0x104] ^= 1
        self.assertEqual(repair_rom(bytes(data), 'invalid_header_checksum'), (bytes(data), ''))

    def test_card_converts_to_exact_device_size(self):
        with tempfile.TemporaryDirectory() as tmp:
            png, cma = Path(tmp)/'card.png', Path(tmp)/'card.cma'
            title_card('A very long title (World).gb', 'GB', png)
            convert(png, cma)
            self.assertEqual(cma.stat().st_size, 11536)
            self.assertEqual(cma.read_bytes()[:4], b'CMAR')


if __name__ == '__main__':
    unittest.main()
