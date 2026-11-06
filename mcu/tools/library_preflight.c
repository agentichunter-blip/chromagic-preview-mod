/* Read-only collection checks using the actual firmware decoders.
 * Build with the host stubs; this program has no device transport. */
#include "../main/virtual_cart.c"
#include "../main/box_art.h"

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "rom") && strcmp(argv[1], "rom-full") && strcmp(argv[1], "art"))) return 2;
    char Path[4096];
    uint8_t Pixels[BOX_ART_BYTES];
    while (fgets(Path, sizeof(Path), stdin))
    {
        Path[strcspn(Path, "\r\n")] = '\0';
        FILE *File = fopen(Path, "rb");
        esp_err_t Result = ESP_ERR_NOT_FOUND;
        bool GlobalChecksumWarning = false;
        if (File)
        {
            if (!strcmp(argv[1], "art")) Result = BoxArt_Read(File, Pixels) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
            else
            {
                VirtualCartInfo_t Info = {0};
                Result = DecodeHeader(File, &Info);
                if (Result == ESP_OK && !strcmp(argv[1], "rom-full"))
                {
                    uint32_t Sum = 0, Offset = 0;
                    size_t Count;
                    while ((Count = fread(Pixels, 1, sizeof(Pixels), File)) > 0)
                    {
                        for (size_t i = 0; i < Count; ++i, ++Offset)
                            if (Offset != 0x14e && Offset != 0x14f) Sum += Pixels[i];
                    }
                    if (ferror(File) || Offset != Info.RomSize) Result = ESP_FAIL;
                    GlobalChecksumWarning = (uint16_t)Sum != Info.GlobalChecksum;
                }
            }
            if (fclose(File) && Result == ESP_OK) Result = ESP_FAIL;
        }
        const char *Status = Result == ESP_OK ? (GlobalChecksumWarning ? "ok_global_checksum_warning" : "ok") :
            Result == ESP_ERR_NOT_SUPPORTED ? "unsupported_mapper_or_geometry" :
            Result == ESP_ERR_INVALID_RESPONSE ? "invalid_header_or_format" :
            Result == ESP_ERR_INVALID_CRC ? "invalid_header_checksum" :
            Result == ESP_ERR_INVALID_SIZE ? "file_size_mismatch" : "read_error";
        printf("%s\t%s\n", Status, Path);
    }
    return ferror(stdin) ? 1 : 0;
}
