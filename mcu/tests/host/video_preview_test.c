#include "video_preview.h"
#include <assert.h>
#include <string.h>
#include <stdlib.h>
static _Alignas(4) uint8_t Pixels[PREVIEW_BYTES];
static const uint8_t Header[24]={'C','M','V','P',1,1,128,0,92,0,12,0,2,0,0,0,0,92,0,0,0,0,0,0};
static FILE *Clip(const uint8_t *H, unsigned Bytes) {
    FILE *F=tmpfile();assert(F);fwrite(H,1,24,F);
    for(unsigned i=0;i<Bytes;i++)fputc(i%2?0xf8:0x1f,F);
    rewind(F);return F;
}
int main(int argc,char **argv) {
    if(argc>1) { /* Batch validator for converted assets. */
        for(int i=1;i<argc;i++) {
            FILE *F=fopen(argv[i],"rb");assert(F);VideoPreview_t V;
            if(strstr(argv[i],".cmv")) { assert(VideoPreview_Open(&V,F)); for(unsigned n=0;n<V.Count;n++)assert(VideoPreview_Read(&V,Pixels)); }
            else assert(PanelArt_Read(F,Pixels));
            fclose(F);
        }
        return 0;
    }
    VideoPreview_t V;FILE *F=Clip(Header,PREVIEW_BYTES*2);
    assert(VideoPreview_Open(&V,F) && V.Count==2 && V.FPS==12);
    for(int i=0;i<5;i++)assert(VideoPreview_Read(&V,Pixels) && Pixels[0]==0x1e && Pixels[1]==0xf8);
    for(unsigned i=0;i<PREVIEW_BYTES;i+=2) assert(Pixels[i]==0x1e && Pixels[i+1]==0xf8);
    uint8_t Copy[PREVIEW_BYTES]; Preview_CopyToBytes(Copy,Pixels);
    assert(!memcmp(Copy,Pixels,PREVIEW_BYTES));
    fclose(F);
    for(unsigned pos=0;pos<24;pos++) {
        uint8_t H[24];memcpy(H,Header,24);H[pos]^=0x80;
        F=Clip(H,PREVIEW_BYTES*2);assert(!VideoPreview_Open(&V,F));fclose(F);
    }
    F=Clip(Header,PREVIEW_BYTES*2-1);assert(!VideoPreview_Open(&V,F));fclose(F);
    F=Clip(Header,PREVIEW_BYTES*2+1);assert(!VideoPreview_Open(&V,F));fclose(F);
    F=tmpfile();uint8_t A[16]={'C','M','A','R',2,1,128,0,92,0,0,0,0,92,0,0};fwrite(A,1,16,F);
    memset(Pixels,0x22,sizeof(Pixels));fwrite(Pixels,1,sizeof(Pixels),F);rewind(F);
    assert(PanelArt_Read(F,Pixels) && Pixels[0]==0x22 && Pixels[PREVIEW_BYTES-1]==0x22);fclose(F);
    /* Legacy pixels survive the word-wise in-place move, including every row
     * boundary and the last pixel; padding must not overwrite source rows. */
    F=tmpfile();const uint8_t Old[16]={'C','M','A','R',1,1,72,0,80,0,0,0,0,45,0,0};
    fwrite(Old,1,16,F);
    for(unsigned i=0;i<72*80;++i) { fputc(i&255,F);fputc(i>>8,F); }
    rewind(F);assert(PanelArt_Read(F,Pixels));fclose(F);
    for(unsigned y=0;y<92;++y) for(unsigned x=0;x<128;++x) {
        unsigned Expected=(y>=6 && y<86 && x>=28 && x<100)?(y-6)*72+x-28:0;
        unsigned Offset=2*(y*128+x);
        assert(Pixels[Offset]==(Expected&255) && Pixels[Offset+1]==(Expected>>8));
    }
    F=Clip(Header,PREVIEW_BYTES*2);assert(VideoPreview_Open(&V,F));
    assert(!VideoPreview_Read(&V,Pixels+1));fclose(F);
    puts("PASS: bounded headers, exact lengths, looping, color key and panel covers");
}
