#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "rom_browser.h"
#include "rom_browser_ui.h"
#include "sd_card.h"

static RomBrowserDataSource_t Source;
static bool Mounted;
static unsigned Mounts, Unmounts, Starts;
static esp_err_t MountResult;
static char StartedPath[320];

void RomBrowserUI_RegisterDataSource(const RomBrowserDataSource_t *pSource) { Source = *pSource; }
esp_err_t SDCard_Mount(sdmmc_card_t **ppCard) {
    assert(!Mounted);
    if (MountResult != ESP_OK) return MountResult;
    Mounted = true; ++Mounts; *ppCard = (sdmmc_card_t *)&Mounted;
    return ESP_OK;
}
esp_err_t SDCard_Unmount(sdmmc_card_t *pCard) {
    assert(Mounted && pCard == (sdmmc_card_t *)&Mounted);
    Mounted = false; ++Unmounts; return ESP_OK;
}
bool OSD_IsVisible(void) { return false; }
esp_err_t FPGA_Tx_PulseButtons(uint16_t Buttons) { assert(false); return ESP_FAIL; }
esp_err_t VirtualCart_Start(const char *pPath) {
    assert(!Mounted && Mounts == Unmounts);
    ++Starts; snprintf(StartedPath, sizeof(StartedPath), "%s", pPath);
    return ESP_OK;
}
static void FileAt(const char *pName) {
    FILE *pFile = fopen(pName, "wb"); assert(pFile); assert(fclose(pFile) == 0);
}
static void Row(size_t Index, const char *pName, bool Directory) {
    assert(strcmp(Source.NameAt(Index), pName) == 0);
    assert(Source.IsDirectory(Index) == Directory);
}

int main(void) {
    assert(mkdir(ROM_BROWSER_ROOT, 0700) == 0);
    assert(mkdir(ROM_BROWSER_ROOT "/Empty.folder", 0700) == 0);
    assert(mkdir(ROM_BROWSER_ROOT "/Japan.v1", 0700) == 0);
    assert(mkdir(ROM_BROWSER_ROOT "/Japan.v1/Deeper", 0700) == 0);
    FileAt(ROM_BROWSER_ROOT "/Pokemon.gbc");
    FileAt(ROM_BROWSER_ROOT "/Pokemon.sav");
    FileAt(ROM_BROWSER_ROOT "/._Pokemon.gbc");
    FileAt(ROM_BROWSER_ROOT "/.DS_Store");
    assert(mkdir(ROM_BROWSER_ROOT "/.hidden", 0700) == 0);
    FileAt(ROM_BROWSER_ROOT "/Japan.v1/Pokemon.gbc");
    FileAt(ROM_BROWSER_ROOT "/Japan.v1/Pokemon.rtc");
    FileAt(ROM_BROWSER_ROOT "/Japan.v1/Deeper/Pokemon.gbc");
    assert(RomBrowser_Init() == ESP_OK);
    size_t Count = 0, Selected = 99;
    assert(Source.AtRoot());
    assert(Source.Open(&Count) == ESP_OK && Count == 3);
    Row(0, "Empty.folder", true); Row(1, "Japan.v1", true); Row(2, "Pokemon.gbc", false);
    assert(Source.Enter(2, &Count) == ESP_ERR_INVALID_ARG);
    assert(Source.Parent(&Count, &Selected) == ESP_ERR_INVALID_ARG);
    assert(Source.Enter(0, &Count) == ESP_OK && Count == 0 && !Source.AtRoot());
    assert(Starts == 0 && !Mounted);
    assert(Source.Parent(&Count, &Selected) == ESP_OK && Count == 3 && Selected == 0);
    assert(Source.Enter(1, &Count) == ESP_OK && Count == 2);
    Row(0, "Deeper", true); Row(1, "Pokemon.gbc", false);
    Source.Remember(1);
    char Art[320];
    assert(Source.ArtPath(1, Art, sizeof(Art)));
    assert(!strcmp(Art, "/sdcard/CHROMAGIC/ART/Japan.v1/Pokemon.gbc.cma"));
    assert(!Source.ArtPath(0, Art, sizeof(Art)) && !Source.ArtPath(1, Art, 4));
    Source.Close();
    assert(!Source.AtRoot() && Source.NameAt(0) == NULL && !Mounted);
    assert(Source.Open(&Count) == ESP_OK && Count == 2);
    Row(0, "Deeper", true); Row(1, "Pokemon.gbc", false);
    assert(Source.Restore() == 1);
    assert(Source.Start(Source.NameAt(1)) == ESP_OK);
    assert(strcmp(StartedPath, ROM_BROWSER_ROOT "/Japan.v1/Pokemon.gbc") == 0);
    assert(Source.Enter(0, &Count) == ESP_OK && Count == 1);
    for (unsigned Reopen = 0; Reopen < 3; ++Reopen) {
        Source.Close();
        assert(!Source.AtRoot() && Source.NameAt(0) == NULL && !Mounted);
        assert(Source.Open(&Count) == ESP_OK && Count == 1);
        Row(0, "Pokemon.gbc", false);
    }
    assert(Source.Start(Source.NameAt(0)) == ESP_OK);
    assert(strcmp(StartedPath, ROM_BROWSER_ROOT "/Japan.v1/Deeper/Pokemon.gbc") == 0);
    assert(Source.Parent(&Count, &Selected) == ESP_OK && Count == 2 && Selected == 0);
    assert(Source.Parent(&Count, &Selected) == ESP_OK && Count == 3 && Selected == 1);
    assert(Source.AtRoot());
    assert(Source.Start(Source.NameAt(2)) == ESP_OK);
    assert(strcmp(StartedPath, ROM_BROWSER_ROOT "/Pokemon.gbc") == 0 && Starts == 3);

    MountResult = ESP_ERR_INVALID_STATE;
    assert(Source.Enter(1, &Count) == ESP_ERR_INVALID_STATE);
    assert(!Mounted && Starts == 3);
    MountResult = ESP_ERR_NOT_FOUND;
    assert(Source.Open(&Count) == ESP_ERR_NOT_FOUND);
    MountResult = ESP_OK;
    assert(Source.Open(&Count) == ESP_OK && Count == 2);
    assert(Source.Parent(&Count, &Selected) == ESP_OK && Selected == 1);
    assert(rmdir(ROM_BROWSER_ROOT "/Empty.folder") == 0);
    assert(Source.Enter(0, &Count) == ESP_FAIL);
    assert(Source.Parent(&Count, &Selected) == ESP_OK && Count == 2);
    assert(Source.Enter(0, &Count) == ESP_OK);
    Source.Close();
    assert(!Source.AtRoot() && Source.NameAt(0) == NULL);
    MountResult = ESP_ERR_INVALID_STATE;
    assert(Source.Open(&Count) == ESP_ERR_INVALID_STATE && !Source.AtRoot());
    MountResult = ESP_OK;
    assert(Source.Open(&Count) == ESP_OK && Count == 2);
    assert(Source.Parent(&Count, &Selected) == ESP_OK && Source.AtRoot());

    char Folder[512], LongRom[512], ShortRom[512];
    char Name[181]; memset(Name, 'x', 180); Name[180] = '\0';
    snprintf(Folder, sizeof(Folder), "%s/%s", ROM_BROWSER_ROOT, Name);
    assert(mkdir(Folder, 0700) == 0);
    char LongName[256];
    const size_t Stem = 315 - strlen(Folder) - 1 - strlen(".gb");
    assert(Stem > 0 && Stem + 4 < sizeof(LongName));
    memset(LongName, 'L', Stem); strcpy(LongName + Stem, ".gb");
    snprintf(LongRom, sizeof(LongRom), "%s/%s", Folder, LongName);
    assert(strlen(LongRom) == 315);
    snprintf(ShortRom, sizeof(ShortRom), "%s/a.gb", Folder);
    FileAt(LongRom); FileAt(ShortRom);
    assert(Source.Open(&Count) == ESP_OK && Count == 3);
    assert(Source.Enter(1, &Count) == ESP_OK && Count == 1);
    Row(0, "a.gb", false);
    assert(Source.Start(Source.NameAt(0)) == ESP_OK && strcmp(StartedPath, ShortRom) == 0);
    Source.Close();
    assert(unlink(LongRom) == 0 && unlink(ShortRom) == 0 && rmdir(Folder) == 0);
    assert(!Source.AtRoot());
    assert(Source.Open(&Count) == ESP_OK && Count == 2 && Source.AtRoot());
    assert(mkdir(Folder, 0700) == 0);
    assert(Source.Open(&Count) == ESP_OK && Count == 3);
    assert(Source.Enter(1, &Count) == ESP_OK && Count == 0);
    Source.Close();
    assert(rmdir(Folder) == 0);
    FileAt(Folder);
    assert(Source.Open(&Count) == ESP_OK && Count == 2 && Source.AtRoot());
    assert(unlink(Folder) == 0);
    Source.Close();
    assert(unlink(ROM_BROWSER_ROOT "/Pokemon.gbc") == 0);
    assert(unlink(ROM_BROWSER_ROOT "/Pokemon.sav") == 0);
    assert(unlink(ROM_BROWSER_ROOT "/._Pokemon.gbc") == 0);
    assert(unlink(ROM_BROWSER_ROOT "/.DS_Store") == 0);
    assert(rmdir(ROM_BROWSER_ROOT "/.hidden") == 0);
    assert(unlink(ROM_BROWSER_ROOT "/Japan.v1/Pokemon.gbc") == 0);
    assert(unlink(ROM_BROWSER_ROOT "/Japan.v1/Pokemon.rtc") == 0);
    assert(unlink(ROM_BROWSER_ROOT "/Japan.v1/Deeper/Pokemon.gbc") == 0);
    assert(rmdir(ROM_BROWSER_ROOT "/Japan.v1/Deeper") == 0);
    assert(rmdir(ROM_BROWSER_ROOT "/Japan.v1") == 0);
    for (unsigned I = 0; I < 2000; ++I) {
        char Path[320]; snprintf(Path, sizeof(Path), "%s/Game_%04u.gb", ROM_BROWSER_ROOT, I);
        FileAt(Path);
        snprintf(Path, sizeof(Path), "%s/._Game_%04u.gb", ROM_BROWSER_ROOT, I);
        FileAt(Path);
    }
    assert(Source.Open(&Count) == ESP_OK && Count == 2000);
    Row(0, "Game_0000.gb", false); Row(1999, "Game_1999.gb", false);
    Source.Remember(1955); Source.Close();
    assert(Source.Open(&Count) == ESP_OK && Count == 2000 && Source.Restore() == 1955);
    Source.Close();
    for (unsigned I = 0; I < 2000; ++I) {
        char Path[320]; snprintf(Path, sizeof(Path), "%s/Game_%04u.gb", ROM_BROWSER_ROOT, I);
        assert(unlink(Path) == 0);
        snprintf(Path, sizeof(Path), "%s/._Game_%04u.gb", ROM_BROWSER_ROOT, I);
        assert(unlink(Path) == 0);
    }
    assert(mkdir(ROM_BROWSER_ROOT "/gb", 0700) == 0);
    assert(mkdir(ROM_BROWSER_ROOT "/GBC", 0700) == 0);
    assert(mkdir(ROM_BROWSER_ROOT "/gb/0-F", 0700) == 0);
    assert(mkdir(ROM_BROWSER_ROOT "/gb/0-F/Nested", 0700) == 0);
    assert(mkdir(ROM_BROWSER_ROOT "/GBC/M-R", 0700) == 0);
    FileAt(ROM_BROWSER_ROOT "/gb/0-F/Game.gb");
    assert(Source.Open(&Count) == ESP_OK && Count == 2);
    assert(Source.FolderPlatform(0) == kRomPlatformGB);
    assert(Source.FolderPlatform(1) == kRomPlatformGBC);
    assert(Source.FolderPlatform(99) == kRomPlatformUnknown);
    assert(Source.Enter(0, &Count) == ESP_OK && Count == 1);
    assert(Source.FolderPlatform(0) == kRomPlatformGB);
    assert(Source.Enter(0, &Count) == ESP_OK && Count == 2);
    assert(Source.FolderPlatform(0) == kRomPlatformGB);
    assert(Source.FolderPlatform(1) == kRomPlatformUnknown);
    assert(Source.Parent(&Count, &Selected) == ESP_OK);
    assert(Source.Parent(&Count, &Selected) == ESP_OK);
    assert(Source.Enter(1, &Count) == ESP_OK && Count == 1);
    assert(Source.FolderPlatform(0) == kRomPlatformGBC);
    assert(Source.Parent(&Count, &Selected) == ESP_OK);
    assert(Source.FolderPlatform(0) == kRomPlatformGB);
    Source.Close();
    assert(unlink(ROM_BROWSER_ROOT "/gb/0-F/Game.gb") == 0);
    assert(rmdir(ROM_BROWSER_ROOT "/gb/0-F/Nested") == 0);
    assert(rmdir(ROM_BROWSER_ROOT "/gb/0-F") == 0);
    assert(rmdir(ROM_BROWSER_ROOT "/gb") == 0);
    assert(rmdir(ROM_BROWSER_ROOT "/GBC/M-R") == 0);
    assert(rmdir(ROM_BROWSER_ROOT "/GBC") == 0);
    /* BIOS dumps must not become game entries; the filter is only a leading
     * tag on files, so similarly named games and directories stay accessible. */
    const char *BiosFiles[] = {
        ROM_BROWSER_ROOT "/[BIOS] Nintendo Game Boy Pocket Boot ROM (World).gb",
        ROM_BROWSER_ROOT "/[bIoS] Nintendo Game Boy Color Boot ROM (World).GBC",
        ROM_BROWSER_ROOT "/[BIOS].gb",
    };
    for (unsigned I=0;I<sizeof(BiosFiles)/sizeof(BiosFiles[0]);++I) FileAt(BiosFiles[I]);
    FileAt(ROM_BROWSER_ROOT "/Bio Sensor.gb");
    FileAt(ROM_BROWSER_ROOT "/Game [BIOS] Mode.gbc");
    assert(mkdir(ROM_BROWSER_ROOT "/[BIOS] Reference",0700)==0);
    FileAt(ROM_BROWSER_ROOT "/[BIOS] Reference/Playable.gb");
    FileAt(ROM_BROWSER_ROOT "/[BIOS] Reference/[BIOS] Stub.gb");
    assert(Source.Open(&Count)==ESP_OK && Count==3);
    Row(0,"[BIOS] Reference",true); Row(1,"Bio Sensor.gb",false); Row(2,"Game [BIOS] Mode.gbc",false);
    assert(Source.Enter(0,&Count)==ESP_OK && Count==1);
    Row(0,"Playable.gb",false);
    assert(Source.Parent(&Count,&Selected)==ESP_OK && Count==3 && Selected==0);
    Source.Close();
    for (unsigned I=0;I<sizeof(BiosFiles)/sizeof(BiosFiles[0]);++I) {
        assert(access(BiosFiles[I],F_OK)==0); /* hidden, never deleted */
        assert(unlink(BiosFiles[I])==0);
    }
    assert(unlink(ROM_BROWSER_ROOT "/Bio Sensor.gb")==0);
    assert(unlink(ROM_BROWSER_ROOT "/Game [BIOS] Mode.gbc")==0);
    assert(unlink(ROM_BROWSER_ROOT "/[BIOS] Reference/Playable.gb")==0);
    assert(unlink(ROM_BROWSER_ROOT "/[BIOS] Reference/[BIOS] Stub.gb")==0);
    assert(rmdir(ROM_BROWSER_ROOT "/[BIOS] Reference")==0);
    assert(rmdir(ROM_BROWSER_ROOT) == 0);
    assert(Mounts == Unmounts && !Mounted);
    puts("PASS: real folder enumeration, full ROM paths, back selection, SD ownership and path limits");
}
