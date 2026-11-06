#include "box_art.h"
#include <string.h>

bool BoxArt_Read(FILE *File, uint8_t Pixels[BOX_ART_BYTES])
{
    static const uint8_t Header[16] = {
        'C', 'M', 'A', 'R', 1, 1, 72, 0, 80, 0, 0, 0, 0, 45, 0, 0
    };
    uint8_t Actual[16];
    if (!File || !Pixels || fread(Actual, 1, sizeof(Actual), File) != sizeof(Actual) ||
        memcmp(Actual, Header, sizeof(Header)) != 0 ||
        fread(Pixels, 1, BOX_ART_BYTES, File) != BOX_ART_BYTES ||
        fgetc(File) != EOF || ferror(File)) return false;
    /* Also protect against incorrectly prepared assets producing OSD holes. */
    for (unsigned I = 0; I < BOX_ART_BYTES; I += 2)
        if (Pixels[I] == 0x1f && Pixels[I + 1] == 0xf8) Pixels[I] = 0x1e;
    return true;
}
