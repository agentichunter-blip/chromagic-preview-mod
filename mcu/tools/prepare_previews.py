#!/usr/bin/env python3
"""Convert gameplay to CMVP and stage 128x92 covers/video for a ROM library.
No ROMs or saves are modified. Outputs are not compatible with art7 firmware.
"""
import argparse, array, csv, hashlib, json, re, struct, subprocess, sys, unicodedata
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from PIL import Image, ImageOps

WIDTH, HEIGHT, FPS, SECONDS = 128, 92, 12, 8
FRAME_BYTES = WIDTH * HEIGHT * 2
VIDEO_HEADER = struct.Struct('<4sBBHHHIII')
ART_HEADER = struct.pack('<4sBBHHHI', b'CMAR', 2, 1, WIDTH, HEIGHT, 0, FRAME_BYTES)


def opaque(data):
    data = bytearray(data)
    start = 0
    while True:
        i = data.find(b'\x1f\xf8', start)
        if i < 0: break
        if i % 2 == 0: data[i] = 0x1e
        start = i + 1
    return data


def convert_video(source, dest):
    if dest.exists():
        with dest.open('rb') as f: header = f.read(24)
        if len(header)==24:
            magic,ver,fmt,w,h,fps,n,size,res = VIDEO_HEADER.unpack(header)
            if (magic,ver,fmt,w,h,fps,size,res)==(b'CMVP',1,1,WIDTH,HEIGHT,FPS,FRAME_BYTES,0) and n==FPS*SECONDS and dest.stat().st_size==24+n*size:
                return n
    cmd=['ffmpeg','-nostdin','-v','error','-threads','1','-filter_threads','1','-ss','2','-i',str(source),
         '-t',str(SECONDS),'-an','-vf',f'fps={FPS},scale={WIDTH}:{HEIGHT}:force_original_aspect_ratio=decrease:flags=neighbor,pad={WIDTH}:{HEIGHT}:(ow-iw)/2:(oh-ih)/2:black',
         '-frames:v',str(FPS*SECONDS),'-pix_fmt','rgb565le','-f','rawvideo','pipe:1']
    result=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    if result.returncode: raise ValueError(result.stderr.decode(errors='replace'))
    n,rem=divmod(len(result.stdout),FRAME_BYTES)
    if rem or n!=FPS*SECONDS: raise ValueError(f'Expected {FPS*SECONDS} frames; got {n}, remainder {rem}')
    dest.parent.mkdir(parents=True,exist_ok=True)
    temp=dest.with_suffix('.tmp')
    try:
        temp.write_bytes(VIDEO_HEADER.pack(b'CMVP',1,1,WIDTH,HEIGHT,FPS,n,FRAME_BYTES,0)+opaque(result.stdout))
        temp.replace(dest)
    finally: temp.unlink(missing_ok=True)
    return n


def convert_cover(source, dest):
    if source.suffix=='.cma':
        b=source.read_bytes()
        if b[:6]!=b'CMAR\x01\x01' or len(b)!=16+72*80*2: raise ValueError('Invalid legacy cover')
        vals=array.array('H',b[16:])
        if sys.byteorder!='little': vals.byteswap()
        rgb=bytes(v for c in vals for v in (((c>>11)*255+15)//31,(((c>>5)&63)*255+31)//63,((c&31)*255+15)//31))
        image=Image.frombytes('RGB',(72,80),rgb)
        resample=Image.Resampling.NEAREST
    else:
        with Image.open(source) as opened: image=ImageOps.exif_transpose(opened).convert('RGBA')
        resample=Image.Resampling.LANCZOS
    image=ImageOps.contain(image,(WIDTH,HEIGHT),resample).convert('RGBA')
    canvas=Image.new('RGBA',(WIDTH,HEIGHT),(0,0,0,255))
    canvas.alpha_composite(image,((WIDTH-image.width)//2,(HEIGHT-image.height)//2))
    pixels=array.array('H',(((r>>3)<<11)|((g>>2)<<5)|(b>>3) for r,g,b in canvas.convert('RGB').getdata()))
    if sys.byteorder!='little': pixels.byteswap()
    dest.parent.mkdir(parents=True,exist_ok=True)
    temp=dest.with_suffix('.tmp');temp.write_bytes(ART_HEADER+opaque(pixels.tobytes()));temp.replace(dest)


def identity(stem):
    # Region/revision variants may share a preview; hacks/prototypes/translations
    # remain distinct. No fuzzy matching between different game titles.
    special=[]
    for tag in re.findall(r'\(([^)]*)\)|\[([^]]*)\]',stem):
        text=' '.join(tag)
        if re.search(r'\b(hack|proto|beta|unl|homebrew|translated)\b',text,re.I): special.append(text.casefold().strip())
    base=re.sub(r'\([^)]*\)|\[[^]]*\]','',stem)
    base=unicodedata.normalize('NFKD',base).encode('ascii','ignore').decode().casefold().replace('&','and')
    return re.sub(r'[^a-z0-9]','',base),tuple(sorted(special))


def match(rom, videos):
    exact=[v for v in videos if v.stem.casefold()==rom.stem.casefold()]
    if len(exact)==1: return exact[0],'exact'
    same=[v for v in videos if identity(v.stem)==identity(rom.stem)]
    if len(same)==1: return same[0],'same-title variant'
    for region in ['USA','Europe','World','Japan']:
        if region.casefold() in rom.stem.casefold():
            regional=[v for v in same if region.casefold() in v.stem.casefold()]
            if len(regional)==1: return regional[0],'same-title region'
            base=[v for v in regional if not re.search(r'\(Rev(?:ision)?[ .]',v.stem,re.I)]
            if len(base)==1: return base[0],'same-title region base revision'
    return None,'ambiguous' if same else 'missing'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--videos',type=Path,required=True)
    parser.add_argument('--library',type=Path,required=True)
    parser.add_argument('--originals',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--jobs',type=int,default=4)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    videos={s:sorted((args.videos/s).rglob('*.mp4')) for s in ['GB','GBC']}
    converted={};errors=[];done=0
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        jobs={pool.submit(convert_video,p,args.output/'converted'/s/(p.stem+'.cmv')):(s,p) for s,paths in videos.items() for p in paths}
        for future in as_completed(jobs):
            s,p=jobs[future]
            try: frames=future.result();converted[str(p)]=str(args.output/'converted'/s/(p.stem+'.cmv'))
            except Exception as exc: errors.append({'video':str(p),'error':str(exc)})
            done+=1
            if done%100==0: print(f'Converted {done}/{len(jobs)} clips',flush=True)
    manifest={row['sd_rom']:row for row in csv.DictReader((args.library/'reports/library-manifest.csv').open())}
    report=[]
    for rom in sorted((args.library/'CHROMAGIC/BACKUPS').rglob('*')):
        if not rom.is_file() or rom.suffix.lower() not in ('.gb','.gbc') or rom.name.startswith('._'):continue
        rel=rom.relative_to(args.library/'CHROMAGIC/BACKUPS');system=rel.parts[0]
        video,reason=match(rom,videos.get(system,[]));entry={'rom':str(rel),'video_match':reason}
        if video and str(video) in converted:
            dest=args.output/'sd-overlay/CHROMAGIC/VIDEO'/(str(rel)+'.cmv');dest.parent.mkdir(parents=True,exist_ok=True)
            source=Path(converted[str(video)])
            if dest.exists():
                if not dest.samefile(source): dest.unlink()
            if not dest.exists():dest.hardlink_to(source)
            entry.update(video=str(video),converted_video=str(dest))
        row=manifest.get('CHROMAGIC/BACKUPS/'+str(rel),{})
        image=args.originals/row.get('artwork','')
        if not image.is_file():image=args.library/'CHROMAGIC/ART'/(str(rel)+'.cma')
        try:
            convert_cover(image,args.output/'sd-overlay/CHROMAGIC/ART'/(str(rel)+'.cma'))
            entry['cover_source']=str(image)
        except Exception as exc:entry['cover_error']=str(exc);errors.append({'rom':str(rel),'error':str(exc)})
        report.append(entry)
    summary={'width':WIDTH,'height':HEIGHT,'fps':FPS,'seconds':SECONDS,'converted_clips':len(converted),'roms':len(report),
             'matched_roms':sum('converted_video' in r for r in report),'covers':sum('cover_source' in r for r in report),'errors':errors}
    (args.output/'conversion-report.json').write_text(json.dumps({'summary':summary,'roms':report},indent=2)+'\n')
    print(json.dumps(summary,indent=2),flush=True)
    return bool(errors)
if __name__=='__main__':sys.exit(main())
