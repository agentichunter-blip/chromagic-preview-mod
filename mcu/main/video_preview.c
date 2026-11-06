#include "video_preview.h"
#include "box_art.h"
#include <string.h>
static unsigned U16(const uint8_t *P) { return P[0] | ((unsigned)P[1] << 8); }
static uint32_t U32(const uint8_t *P) { return U16(P) | ((uint32_t)U16(P+2) << 16); }
/* ESP32 instruction RAM only permits aligned 32-bit loads/stores. Keep
 * stdio's byte accesses in a small DRAM stack buffer and publish whole words.
 * Volatile prevents these loops becoming a byte-oriented memcpy/memset. */
static uint32_t OpaqueWord(uint32_t W) {
    if ((W & 0xffffu)==0xf81fu) W=(W & 0xffff0000u)|0xf81eu;
    if ((W >> 16)==0xf81fu) W=(W & 0xffffu)|0xf81e0000u;
    return W;
}
static bool ReadPixels(FILE *F, uint8_t *Pixels, unsigned Bytes) {
    if (((uintptr_t)Pixels & 3u) || (Bytes & 3u)) return false;
    uint32_t Chunk[128];
    volatile uint32_t *Out=(volatile uint32_t *)Pixels;
    for (unsigned Offset=0;Offset<Bytes;) {
        const unsigned Count=Bytes-Offset<sizeof(Chunk)?Bytes-Offset:sizeof(Chunk);
        if (fread(Chunk,1,Count,F)!=Count || ferror(F)) return false;
        for (unsigned I=0;I<Count/4;++I) *Out++=OpaqueWord(Chunk[I]);
        Offset+=Count;
    }
    return true;
}
void Preview_CopyToBytes(uint8_t *Out, const uint8_t *Pixels) {
    const volatile uint32_t *In=(const volatile uint32_t *)Pixels;
    for (unsigned I=0;I<PREVIEW_BYTES;I+=4) {
        const uint32_t W=*In++;
        Out[I]=(uint8_t)W; Out[I+1]=(uint8_t)(W>>8);
        Out[I+2]=(uint8_t)(W>>16); Out[I+3]=(uint8_t)(W>>24);
    }
}
bool VideoPreview_Open(VideoPreview_t *V, FILE *F) {
    uint8_t H[PREVIEW_HEADER_BYTES];
    *V=(VideoPreview_t){0};
    if (!F || fread(H,1,sizeof(H),F)!=sizeof(H) || memcmp(H,"CMVP",4) ||
        H[4]!=1 || H[5]!=1 || U16(H+6)!=PREVIEW_WIDTH || U16(H+8)!=PREVIEW_HEIGHT ||
        !U16(H+10) || U16(H+10)>15 || !U32(H+12) || U32(H+12)>PREVIEW_MAX_FRAMES ||
        U32(H+16)!=PREVIEW_BYTES || U32(H+20)) return false;
    const long Expected=PREVIEW_HEADER_BYTES+(long)U32(H+12)*PREVIEW_BYTES;
    if (fseek(F,0,SEEK_END) || ftell(F)!=Expected || fseek(F,PREVIEW_HEADER_BYTES,SEEK_SET)) return false;
    *V=(VideoPreview_t){.File=F,.Count=U32(H+12),.FPS=U16(H+10)};
    return true;
}
bool VideoPreview_Read(VideoPreview_t *V, uint8_t P[PREVIEW_BYTES]) {
    if (!V->File) return false;
    if (V->Index==V->Count) {
        if (fseek(V->File,PREVIEW_HEADER_BYTES,SEEK_SET)) return false;
        V->Index=0;
    }
    if (!ReadPixels(V->File,P,PREVIEW_BYTES)) return false;
    ++V->Index; return true;
}
bool PanelArt_Read(FILE *F, uint8_t P[PREVIEW_BYTES]) {
    uint8_t H[16];
    if (!F || ((uintptr_t)P & 3u) || fread(H,1,16,F)!=16 || memcmp(H,"CMAR",4) || H[5]!=1 || U16(H+10)) return false;
    if (H[4]==2 && U16(H+6)==PREVIEW_WIDTH && U16(H+8)==PREVIEW_HEIGHT && U32(H+12)==PREVIEW_BYTES) {
        return ReadPixels(F,P,PREVIEW_BYTES) && fgetc(F)==EOF && !ferror(F);
    }
    /* The legacy cover is also loaded word-wise, then centered bottom-up. */
    if (H[4]!=1 || U16(H+6)!=BOX_ART_WIDTH || U16(H+8)!=BOX_ART_HEIGHT ||
        U32(H+12)!=BOX_ART_BYTES || !ReadPixels(F,P,BOX_ART_BYTES) ||
        fgetc(F)!=EOF || ferror(F)) return false;
    enum { X=(PREVIEW_WIDTH-BOX_ART_WIDTH)/2, Y=(PREVIEW_HEIGHT-BOX_ART_HEIGHT)/2 };
    volatile uint32_t *Words=(volatile uint32_t *)P;
    for (int Row=BOX_ART_HEIGHT-1;Row>=0;--Row)
        for (int Col=BOX_ART_WIDTH/2-1;Col>=0;--Col)
            Words[(Row+Y)*(PREVIEW_WIDTH/2)+X/2+Col]=Words[Row*(BOX_ART_WIDTH/2)+Col];
    for (unsigned Row=0;Row<PREVIEW_HEIGHT;++Row)
        for (unsigned Col=0;Col<PREVIEW_WIDTH/2;++Col)
            if(Row<Y || Row>=Y+BOX_ART_HEIGHT || Col<X/2 || Col>=(X+BOX_ART_WIDTH)/2)
                Words[Row*(PREVIEW_WIDTH/2)+Col]=0;
    return true;
}
