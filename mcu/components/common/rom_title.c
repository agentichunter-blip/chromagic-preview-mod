#include "rom_title.h"
#include <string.h>

void RomTitle_Make(const char *Name, bool Directory, char *Output, size_t Size)
{
    if (!Size) return;
    if (!Name) Name = "";
    const char *End = Directory ? NULL : strrchr(Name, '.');
    if (!End) End = Name + strlen(Name);
    while (Name < End && ((unsigned char)*Name <= ' ' || *Name == '_')) ++Name;
    size_t Used = 0;
    while (Name < End && Used + 1 < Size)
    {
        unsigned char C = (unsigned char)*Name++;
        if (C >= 0x80)
        {
            while (Name < End && ((unsigned char)*Name & 0xc0) == 0x80) ++Name;
            C = '?'; /* The menu font is ASCII; preserve the original path. */
        }
        if (C >= 'a' && C <= 'z') C -= 'a' - 'A';
        if (C == '_' || C < ' ') C = ' ';
        Output[Used++] = (char)C;
    }
    while (Used && Output[Used - 1] == ' ') --Used;
    Output[Used] = '\0';
}

int RomTitle_Compare(const char *Left, const char *Right, bool Directory)
{
    char A[256], B[256];
    RomTitle_Make(Left, Directory, A, sizeof(A));
    RomTitle_Make(Right, Directory, B, sizeof(B));
    const int Order = strcmp(A, B);
    return Order ? Order : strcmp(Left, Right);
}

const char *RomTitle_Group(const char *Title, bool Directory, char Letter[2])
{
    if (Directory) return "DIR";
    if (*Title >= '0' && *Title <= '9') return "0-9";
    Letter[0] = *Title >= 'A' && *Title <= 'Z' ? *Title : '#';
    Letter[1] = '\0';
    return Letter;
}
