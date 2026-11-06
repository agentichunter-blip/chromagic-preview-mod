import importlib.util,tempfile,subprocess,struct,sys
from pathlib import Path
from PIL import Image
spec=importlib.util.spec_from_file_location('preview',Path(__file__).resolve().parents[2]/'tools/prepare_previews.py');p=importlib.util.module_from_spec(spec);spec.loader.exec_module(p)
with tempfile.TemporaryDirectory() as d:
 d=Path(d);source=d/'sample.mp4';out=d/'sample.cmv'
 subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','color=c=magenta:s=160x144:r=12:d=11','-c:v','libx264','-pix_fmt','yuv420p',str(source)],check=True)
 assert p.convert_video(source,out)==96
 raw=out.read_bytes();assert len(raw)==24+96*p.FRAME_BYTES
 assert p.VIDEO_HEADER.unpack(raw[:24])==(b'CMVP',1,1,128,92,12,96,p.FRAME_BYTES,0)
 assert all(raw[i:i+2]!=b'\x1f\xf8' for i in range(24,len(raw),2))
 img=d/'cover.png';Image.new('RGB',(100,100),'red').save(img);art=d/'cover.cma';p.convert_cover(img,art)
 assert len(art.read_bytes())==16+p.FRAME_BYTES
 assert p.match(Path('Tetris (World) (Rev 1).gb'),[Path('Tetris (World).mp4')])[1]=='same-title variant'
 assert p.match(Path('Tetris (World) (Hack).gb'),[Path('Tetris (World).mp4')])[0] is None
 assert p.match(Path('Pokemon - Crystal Version (USA).gbc'),[Path('Pokemon - Crystal Version (USA, Europe).mp4'),Path('Pokemon - Crystal Version (USA, Europe) (Rev A).mp4')])[1]=='same-title region base revision'
 assert p.match(Path('Different.gb'),[Path('Tetris.mp4')])[0] is None
 cli=Path(__file__).resolve().parents[2]/'tools/convert_media.py'
 cli_art=d/'nested/cli.cma';cli_video=d/'nested/cli.cmv'
 subprocess.run([sys.executable,str(cli),'cover',str(img),str(cli_art)],check=True)
 subprocess.run([sys.executable,str(cli),'video',str(source),str(cli_video)],check=True)
 assert cli_art.read_bytes()==art.read_bytes()
 assert cli_video.read_bytes()==out.read_bytes()
 before=cli_art.read_bytes()
 refused=subprocess.run([sys.executable,str(cli),'cover',str(img),str(cli_art)],capture_output=True)
 assert refused.returncode!=0 and cli_art.read_bytes()==before
 same=subprocess.run([sys.executable,str(cli),'cover',str(img),str(img)],capture_output=True)
 assert same.returncode!=0 and Image.open(img).size==(100,100)
 print('PASS: conversion, matching, manifest-free CLI and overwrite protection')
