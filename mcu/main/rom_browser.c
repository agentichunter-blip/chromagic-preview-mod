#include "rom_browser.h"

#include "rom_browser_ui.h"
#include "button.h"
#include "fpga_tx.h"
#include "osd.h"
#include "sd_card.h"
#include "virtual_cart.h"
#include "rom_title.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static void TraceCatalog(const char *Phase, esp_err_t Error)
{
    printf("ROM_BROWSER_SCAN phase=%s error=%s time_ms=%lld heap=%lu stack=%u\n",
           Phase, esp_err_to_name(Error), (long long)(esp_timer_get_time() / 1000),
           (unsigned long)esp_get_free_heap_size(),
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
}
#else
#define TraceCatalog(Phase, Error) ((void)0)
#endif

#ifndef ROM_BROWSER_ROOT
#define ROM_BROWSER_ROOT "/sdcard/CHROMAGIC/BACKUPS"
#endif
static const char *const RomDirectory = ROM_BROWSER_ROOT;
enum { kPathSize = 320, kSavePathReserve = 5 };
static char CurrentDirectory[kPathSize] = ROM_BROWSER_ROOT;

typedef struct {
    char *pName;
    bool Directory;
} RomEntry_t;

typedef struct RomCatalog {
    RomEntry_t *pEntries;
    size_t Count;
    size_t Capacity;
} RomCatalog_t;

static RomCatalog_t Catalog;
/* Bounded session-only location cache: no flash writes while browsing. */
static struct { char Directory[kPathSize], Name[256]; } Locations[8];
static unsigned NextLocation;

static bool IsRomName(const char *pName)
{
    if (pName == NULL)
    {
        return false;
    }
    /* No-Intro BIOS dumps share cartridge extensions but are system firmware,
     * not launchable games. Keep them on disk while excluding them from play. */
    if (strncasecmp(pName, "[BIOS]", 6) == 0)
    {
        return false;
    }
    const char *pDot = strrchr(pName, '.');
    return pDot != NULL &&
           (strcasecmp(pDot, ".gb") == 0 || strcasecmp(pDot, ".gbc") == 0);
}

static int CompareNames(const void *pLeft, const void *pRight)
{
    const RomEntry_t *Left = pLeft, *Right = pRight;
    if (Left->Directory != Right->Directory) return Left->Directory ? -1 : 1;
    return RomTitle_Compare(Left->pName, Right->pName, Left->Directory);
}

static void CloseCatalog(void)
{
    for (size_t i = 0; i < Catalog.Count; ++i)
    {
        free(Catalog.pEntries[i].pName);
    }
    free(Catalog.pEntries);
    Catalog.pEntries = NULL;
    Catalog.Count = 0;
    Catalog.Capacity = 0;
}

static esp_err_t AppendName(const char *pName, bool Directory)
{
    if (Catalog.Count == Catalog.Capacity)
    {
        const size_t Capacity = Catalog.Capacity ? Catalog.Capacity * 2 : 32;
        RomEntry_t *Expanded = realloc(Catalog.pEntries, Capacity * sizeof(*Expanded));
        if (!Expanded) return ESP_ERR_NO_MEM;
        Catalog.pEntries = Expanded;
        Catalog.Capacity = Capacity;
    }
    Catalog.pEntries[Catalog.Count].pName = strdup(pName);
    if (Catalog.pEntries[Catalog.Count].pName == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    Catalog.pEntries[Catalog.Count++].Directory = Directory;
    return ESP_OK;
}

static bool AtRoot(void)
{
    return strcmp(CurrentDirectory, RomDirectory) == 0;
}

static esp_err_t ReadCatalog(size_t *pCount, bool ResumeLocation)
{
    if (pCount == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    CloseCatalog();
    TraceCatalog("mount", ESP_OK);

    sdmmc_card_t *pCard = NULL;
    esp_err_t Result = SDCard_Mount(&pCard);
    if (Result != ESP_OK)
    {
        TraceCatalog("mount_failed", Result);
        return Result;
    }

    TraceCatalog("directory", ESP_OK);

    DIR *pDirectory = opendir(CurrentDirectory);
    if (pDirectory == NULL && ResumeLocation && !AtRoot() &&
        (errno == ENOENT || errno == ENOTDIR))
    {
        strcpy(CurrentDirectory, RomDirectory);
        pDirectory = opendir(CurrentDirectory);
    }
    if (pDirectory == NULL)
    {
        Result = errno == ENOENT && AtRoot() ? ESP_OK : ESP_FAIL;
        goto Cleanup;
    }

    for (;;)
    {
        errno = 0;
        struct dirent *pEntry = readdir(pDirectory);
        if (pEntry == NULL)
        {
            if (errno != 0) Result = ESP_FAIL;
            break;
        }
        /* Finder's AppleDouble files retain .gb/.gbc extensions but contain
         * metadata, not ROMs. Hidden folders are not library entries either. */
        if (pEntry->d_name[0] == '.')
        {
            continue;
        }

        char Path[kPathSize];
        const int Length = snprintf(Path, sizeof(Path), "%s/%s", CurrentDirectory,
                                    pEntry->d_name);
        if (Length < 0 || Length >= (int)sizeof(Path))
        {
            continue;
        }
        /* ESP-IDF FATFS supplies d_type from the directory entry. Calling
         * stat for every name rescans the directory and makes this quadratic. */
        bool Directory = pEntry->d_type == DT_DIR;
        bool Regular = pEntry->d_type == DT_REG;
        if (pEntry->d_type == DT_UNKNOWN)
        {
            struct stat Stat;
            if (stat(Path, &Stat) != 0) continue;
            Directory = S_ISDIR(Stat.st_mode);
            Regular = S_ISREG(Stat.st_mode);
        }
        if (!Directory && (!Regular || !IsRomName(pEntry->d_name) ||
                           Length + kSavePathReserve >= kPathSize)) continue;
        Result = AppendName(pEntry->d_name, Directory);
        if (Result != ESP_OK)
        {
            break;
        }
    }
    if (closedir(pDirectory) != 0 && Result == ESP_OK)
    {
        Result = ESP_FAIL;
    }

Cleanup:
    TraceCatalog("unmount", Result);
    {
        const esp_err_t UnmountResult = SDCard_Unmount(pCard);
        if (Result == ESP_OK && UnmountResult != ESP_OK)
        {
            Result = UnmountResult;
        }
    }
    if (Result != ESP_OK)
    {
        CloseCatalog();
        return Result;
    }

    if (Catalog.Count > 1)
    {
        TraceCatalog("sort", ESP_OK);
        qsort(Catalog.pEntries, Catalog.Count, sizeof(*Catalog.pEntries),
              CompareNames);
    }
    *pCount = Catalog.Count;
    TraceCatalog("complete", ESP_OK);
    return ESP_OK;
}

static esp_err_t OpenCatalog(size_t *pCount)
{
    return ReadCatalog(pCount, true);
}

static const char *NameAt(size_t Index)
{
    return Index < Catalog.Count ? Catalog.pEntries[Index].pName : NULL;
}

static bool IsDirectory(size_t Index)
{
    return Index < Catalog.Count && Catalog.pEntries[Index].Directory;
}

static esp_err_t EnterDirectory(size_t Index, size_t *pCount)
{
    if (!IsDirectory(Index) || pCount == NULL) return ESP_ERR_INVALID_ARG;
    char Path[kPathSize];
    const int Length = snprintf(Path, sizeof(Path), "%s/%s", CurrentDirectory,
                                NameAt(Index));
    if (Length < 0 || Length >= (int)sizeof(Path)) return ESP_ERR_INVALID_SIZE;
    memcpy(CurrentDirectory, Path, Length + 1);
    return ReadCatalog(pCount, false);
}

static esp_err_t ParentDirectory(size_t *pCount, size_t *pSelected)
{
    if (AtRoot() || pCount == NULL || pSelected == NULL) return ESP_ERR_INVALID_ARG;
    char *pSlash = strrchr(CurrentDirectory, '/');
    char Previous[kPathSize];
    strcpy(Previous, pSlash + 1);
    *pSlash = '\0';
    const esp_err_t Result = ReadCatalog(pCount, false);
    *pSelected = 0;
    for (size_t Index = 0; Result == ESP_OK && Index < Catalog.Count; ++Index)
    {
        if (IsDirectory(Index) && strcmp(NameAt(Index), Previous) == 0)
        {
            *pSelected = Index;
            break;
        }
    }
    return Result;
}

static esp_err_t StartRom(const char *pName)
{
    printf("ROM_BROWSER_LOAD name=\"%s\"\n", pName != NULL ? pName : "");

    if (pName == NULL) return ESP_ERR_INVALID_ARG;
    char Path[kPathSize];
    const int Length = snprintf(Path, sizeof(Path), "%s/%s", CurrentDirectory, pName);
    if (Length < 0 || Length + kSavePathReserve >= (int)sizeof(Path)) return ESP_ERR_INVALID_SIZE;
    return VirtualCart_Start(Path);
}

esp_err_t RomBrowser_CloseMenu(void)
{
    return OSD_IsVisible()
        ? FPGA_Tx_PulseButtons(kButtonBits_MenuEnAlt) : ESP_OK;
}

static void Remember(size_t Index)
{
    if (Index >= Catalog.Count) return;
    unsigned Slot;
    for (Slot = 0; Slot < 8; ++Slot)
        if (!strcmp(Locations[Slot].Directory, CurrentDirectory)) break;
    if (Slot == 8) Slot = NextLocation++ % 8;
    snprintf(Locations[Slot].Directory, kPathSize, "%s", CurrentDirectory);
    snprintf(Locations[Slot].Name, sizeof(Locations[Slot].Name), "%s", NameAt(Index));
}

static size_t Restore(void)
{
    for (unsigned Slot = 0; Slot < 8; ++Slot)
        if (!strcmp(Locations[Slot].Directory, CurrentDirectory))
            for (size_t I = 0; I < Catalog.Count; ++I)
                if (!strcmp(Locations[Slot].Name, NameAt(I))) return I;
    return 0;
}

static bool ArtPath(size_t Index, char *Path, size_t Size)
{
    if (Index >= Catalog.Count || IsDirectory(Index)) return false;
    const int Length = snprintf(Path, Size, "/sdcard/CHROMAGIC/ART%s/%s.cma",
        CurrentDirectory + strlen(RomDirectory), NameAt(Index));
    return Length >= 0 && (size_t)Length < Size;
}

static bool VideoPath(size_t Index, char *Path, size_t Size)
{
    if (Index >= Catalog.Count || IsDirectory(Index)) return false;
    const int Length = snprintf(Path, Size, "/sdcard/CHROMAGIC/VIDEO%s/%s.cmv",
        CurrentDirectory + strlen(RomDirectory), NameAt(Index));
    return Length >= 0 && (size_t)Length < Size;
}

static RomPlatform_t FolderPlatform(size_t Index)
{
    if (!IsDirectory(Index)) return kRomPlatformUnknown;
    /* Use the top-level system, so every range/nested folder inherits it. */
    const char *Name = AtRoot() ? NameAt(Index) : CurrentDirectory + strlen(RomDirectory) + 1;
    const size_t Length = strcspn(Name, "/");
    if (Length == 2 && strncasecmp(Name, "GB", 2) == 0) return kRomPlatformGB;
    if (Length == 3 && strncasecmp(Name, "GBC", 3) == 0) return kRomPlatformGBC;
    return kRomPlatformUnknown;
}

static const RomBrowserDataSource_t Source = {
    .Open = OpenCatalog, .NameAt = NameAt, .Close = CloseCatalog, .Start = StartRom,
    .IsDirectory = IsDirectory, .Enter = EnterDirectory, .Parent = ParentDirectory,
    .AtRoot = AtRoot, .Remember = Remember, .Restore = Restore, .ArtPath = ArtPath,
    .FolderPlatform = FolderPlatform, .VideoPath = VideoPath,
};

const RomBrowserDataSource_t *RomBrowser_GetDataSource(void) { return &Source; }

esp_err_t RomBrowser_Init(void)
{
    RomBrowserUI_RegisterDataSource(&Source);
    return ESP_OK;
}
