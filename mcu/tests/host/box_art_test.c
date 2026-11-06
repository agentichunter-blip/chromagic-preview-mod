#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "box_art.h"
static unsigned char FileBytes[16 + BOX_ART_BYTES + 1], Pixels[BOX_ART_BYTES];
static bool Read(size_t Length) {
    FILE *f=tmpfile(); assert(f); assert(fwrite(FileBytes,1,Length,f)==Length); rewind(f);
    bool ok=BoxArt_Read(f,Pixels); fclose(f); return ok;
}
int main(void) {
    const unsigned char header[]={ 'C','M','A','R',1,1,72,0,80,0,0,0,0,45,0,0 };
    memcpy(FileBytes,header,16);
    FileBytes[16]=0x1f; FileBytes[17]=0xf8; /* forbidden transparent magenta */
    FileBytes[18]=0x00; FileBytes[19]=0xf8; /* red, little endian */
    assert(Read(16+BOX_ART_BYTES));
    assert(Pixels[0]==0x1e && Pixels[1]==0xf8 && Pixels[2]==0 && Pixels[3]==0xf8);
    for (unsigned i=0;i<16;i++) { FileBytes[i]^=1; assert(!Read(16+BOX_ART_BYTES)); FileBytes[i]^=1; }
    assert(!Read(0) && !Read(15) && !Read(16+BOX_ART_BYTES-1) && !Read(sizeof(FileBytes)));
    assert(!BoxArt_Read(NULL,Pixels));
    puts("PASS: exact asset contract, invalid headers/lengths and opaque RGB565 pixels");
}
