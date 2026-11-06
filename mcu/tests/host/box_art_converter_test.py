import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
from PIL import Image

spec = importlib.util.spec_from_file_location('prepare_box_art', Path(__file__).parents[2] / 'tools/prepare_box_art.py')
converter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(converter)

class CoverTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
    def tearDown(self):
        self.tmp.cleanup()
    def cover(self, name, size=(72,80), color=(255,0,0,255)):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        Image.new('RGBA',size,color).save(path)
        return path
    def test_header_endian_and_color_key(self):
        output = self.root / 'art.cma'
        converter.convert(self.cover('cover.png',color=(255,0,255,255)),output)
        data=output.read_bytes()
        self.assertEqual(len(data),11536)
        self.assertEqual(struct.unpack('<4sBBHHHI',data[:16]),(b'CMAR',1,1,72,80,0,11520))
        self.assertEqual(set(struct.unpack('<5760H',data[16:])),{0xf81e})
    def test_aspect_ratio_and_alpha(self):
        out=self.root/'art.cma'
        converter.convert(self.cover('wide.png',(144,80)),out)
        pixels=struct.unpack('<5760H',out.read_bytes()[16:])
        self.assertNotEqual(pixels[0],0xf800)
        self.assertEqual(pixels[40*72+36],0xf800)
        converter.convert(self.cover('alpha.png',color=(255,0,0,0)),out)
        self.assertEqual(len(set(struct.unpack('<5760H',out.read_bytes()[16:]))),1)
    def test_library_identity_missing_and_mapping(self):
        roms=self.root/'roms'; images=self.root/'images'; art=self.root/'art'
        (roms/'JP').mkdir(parents=True); images.mkdir()
        for name in ('Game.gb','Game.gbc','JP/Game.gb','Missing.gb'):
            (roms/name).touch()
        self.cover('images/Game.gb.png')
        self.cover('images/Game.gbc.png',color=(0,0,255,255))
        self.cover('images/JP/Game.png',color=(0,255,0,255))
        report=converter.library(roms,images,art,{})
        self.assertEqual([r['status'] for r in report],['converted','converted','converted','missing'])
        self.assertNotEqual((art/'Game.gb.cma').read_bytes(),(art/'Game.gbc.cma').read_bytes())
        self.assertNotEqual((art/'Game.gb.cma').read_bytes(),(art/'JP/Game.gb.cma').read_bytes())
        report=converter.library(roms,images,art,{'Missing.gb':'Game.gbc.png'})
        self.assertTrue(all(r['status']=='converted' for r in report))
    def test_ambiguous_and_invalid(self):
        roms=self.root/'roms'; images=self.root/'images'; art=self.root/'art'
        roms.mkdir(); images.mkdir(); (roms/'Game.gb').touch()
        self.cover('images/Game.png'); self.cover('images/Game.webp')
        self.assertEqual(converter.library(roms,images,art,{})[0]['status'],'ambiguous')
        (images/'bad.png').write_text('not an image')
        self.assertEqual(converter.library(roms,images,art,{'Game.gb':'bad.png'})[0]['status'],'error')
        self.assertFalse((art/'Game.gb.cma').exists())

if __name__ == '__main__':
    unittest.main()
