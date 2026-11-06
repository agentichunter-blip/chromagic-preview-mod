#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "menu_mgr.h"
#include "rom_browser_ui.h"
#include "cart_backup_ui.h"
#include "pc_backup_mode.h"
#include "fw.h"
#include "box_art_format.h"
#include "preview_format.h"
#include "battery.h"
#include "mutex.h"

static OSD_Widget_t BatteryWidget;
MutexResult_t Mutex_Take(MutexKey_t Key) { (void)Key; return kMutexResult_Ok; }
MutexResult_t Mutex_Give(MutexKey_t Key) { (void)Key; return kMutexResult_Ok; }
static OSD_Result_t DrawScreen(OSD_Widget_t *Menu, lv_obj_t *Screen)
{
    const OSD_Result_t Result = Menu->fnDraw(Screen);
    assert(BatteryWidget.fnDraw(Screen) == kOSD_Result_Ok);
    return Result;
}

LV_IMG_DECLARE(menu_system);
static uint64_t ClockMs;
static uint16_t HeldBits;
static bool MenuVisible = true;
uint16_t Button_GetHeldBits(void) { return HeldBits; }
int64_t esp_timer_get_time(void) { return (int64_t)ClockMs * 1000; }
bool OSD_IsVisible(void) { return MenuVisible; }
static const char *Names[] = {"Bomberman.gb", "Kirby.gb", "Links_Awakening_DX_An_Extra_Long_Backup_Name.gbc", "Mario.gb", "Pokemon_Crystal.gbc", "Pokemon_Gold.gbc", "Zelda_An_Extra_Long_Backup_Filename_For_Scrolling.gbc"};
static size_t Count = 7;
static int Opens, Closes, Starts;
static esp_err_t OpenResult, StartResult;
static bool CompleteImmediately;
static bool FolderFixture, SystemFixture;
static size_t SystemIndex;
static unsigned Depth;
static PCBackupModeState_t PCMode;
PCBackupModeState_t PCBackupMode_GetState(void) { return PCMode; }
static unsigned BackupStarts;
static esp_err_t BackupStart(void) { ++BackupStarts; return ESP_OK; }
static char StartedName[80];
static esp_err_t Open(size_t *pCount) { ++Opens; *pCount = SystemFixture ? (Depth ? 4 : 2) : FolderFixture ? (Depth == 2 ? 0 : 2) : Count; return OpenResult; }
static const char *NameAt(size_t Index) {
    if (SystemFixture) {
        static const char *Ranges[]={"0-F","G-L","M-R","S-Z"};
        /* Reverse catalog order verifies that logo positions do not depend on sorting. */
        return Depth ? Ranges[Index] : Index ? "GB" : "GBC";
    }
    if (FolderFixture) {
        assert(Depth < 2 && Index < 2);
        return Depth == 0 ? (Index == 0 ? "Bomberman.gb" : "Japan.v1")
                          : (Index == 0 ? "Empty.folder" : "Pokemon.gbc");
    }
    assert(Index < Count); return Names[Index];
}
static bool IsDirectory(size_t Index) { return SystemFixture || (FolderFixture && Index == (Depth == 0 ? 1 : 0)); }
static RomPlatform_t FixturePlatform;
static RomPlatform_t FolderPlatform(size_t Index) { if (SystemFixture) return (Depth ? SystemIndex : Index) ? kRomPlatformGB : kRomPlatformGBC; return IsDirectory(Index) ? FixturePlatform : kRomPlatformUnknown; }
static bool AtRoot(void) { return Depth == 0; }
static esp_err_t Enter(size_t Index, size_t *pCount) {
    assert(IsDirectory(Index)); if (SystemFixture && !Depth) SystemIndex=Index; ++Depth; return Open(pCount);
}
static esp_err_t Parent(size_t *pCount, size_t *pSelected) {
    assert(Depth > 0); --Depth; *pSelected = SystemFixture && !Depth ? SystemIndex : Depth == 0 ? 1 : 0; return Open(pCount);
}
static void Close(void) { ++Closes; Depth = 0; }
static esp_err_t Start(const char *pName) {
    ++Starts; snprintf(StartedName, sizeof(StartedName), "%s", pName);
    if (CompleteImmediately) assert(RomBrowserUI_SetComplete());
    return StartResult;
}
static lv_obj_t *Text(lv_obj_t *pObj, const char *Value) {
    if (lv_obj_check_type(pObj, &lv_label_class) && strcmp(lv_label_get_text(pObj), Value) == 0) return pObj;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(pObj); ++i) {
        lv_obj_t *pFound = Text(lv_obj_get_child(pObj, i), Value);
        if (pFound != NULL) return pFound;
    }
    return NULL;
}
static lv_obj_t *OverlayList(lv_obj_t *Object) {
    lv_obj_update_layout(Object);
    if (lv_obj_check_type(Object, &lv_label_class) && lv_obj_get_width(Object) == 116)
        return lv_obj_get_parent(Object);
    for (uint32_t I = 0; I < lv_obj_get_child_cnt(Object); ++I) {
        lv_obj_t *Found = OverlayList(lv_obj_get_child(Object, I));
        if (Found) return Found;
    }
    return NULL;
}
/* Locate a visible system choice by its native image, not a redundant label. */
static lv_obj_t *SystemTile(lv_obj_t *Object, unsigned Slot) {
    lv_obj_update_layout(Object);
    if (lv_obj_check_type(Object,&lv_img_class) && lv_obj_is_visible(Object)) {
        const lv_img_dsc_t *Source=lv_img_get_src(Object);
        if (Source && Source->header.w==60 && Source->header.h==68) {
            lv_obj_t *Tile=lv_obj_get_parent(Object);
            if (lv_obj_get_x(Tile)==(int)Slot*64) return Tile;
        }
    }
    for (unsigned I=0;I<lv_obj_get_child_cnt(Object);++I) {
        lv_obj_t *Found=SystemTile(lv_obj_get_child(Object,I),Slot);
        if (Found) return Found;
    }
    return NULL;
}
static lv_color_t Frame[160 * 144], Buffer[160 * 144];
static void Flush(lv_disp_drv_t *pDrv, const lv_area_t *pArea, lv_color_t *pPixels) {
    for (int y = pArea->y1; y <= pArea->y2; ++y)
        for (int x = pArea->x1; x <= pArea->x2; ++x)
            Frame[y * 160 + x] = *pPixels++;
    lv_disp_flush_ready(pDrv);
}
static void Snapshot(const char *Name) {
    lv_refr_now(NULL);
    const char *Dir = getenv("CHROMATIC_TEST_RENDER_DIR");
    if (Dir == NULL) return;
    char Path[512]; snprintf(Path, sizeof(Path), "%s/%s.ppm", Dir, Name);
    FILE *pFile = fopen(Path, "wb"); assert(pFile);
    fprintf(pFile, "P6\n160 144\n255\n");
    for (size_t i = 0; i < 160 * 144; ++i) {
        lv_color32_t c = { .full = lv_color_to32(Frame[i]) };
        const unsigned char RGB[] = {c.ch.red, c.ch.green, c.ch.blue};
        fwrite(RGB, 1, 3, pFile);
    }
    fclose(pFile);
}
static bool JobReady, ArtReady, ArtRequested, ArtMissing;
static unsigned ArtRequests;
static uint32_t ArtToken;
static unsigned AsyncBegins, AsyncCancels;
static size_t AsyncIndex;
static RomBrowserAction_t AsyncAction;
static esp_err_t AsyncError;
static esp_err_t BeginAsync(RomBrowserAction_t Action, size_t Index) {
    ++AsyncBegins; AsyncAction=Action; AsyncIndex=Index; JobReady=false; return ESP_OK;
}
static bool PollAsync(RomBrowserJobResult_t *Job) {
    if (!JobReady) return false;
    JobReady=false; *Job=(RomBrowserJobResult_t){.Action=AsyncAction,.Count=7,.Selected=0,.Error=AsyncError}; return true;
}
static void CancelAsync(void) { ++AsyncCancels; JobReady=ArtReady=ArtRequested=false; }
static bool ArtPath(size_t Index,char *Path,size_t Size) {
    snprintf(Path,Size,"/sample/%s.cma",NameAt(Index)); return true;
}
static void RequestArt(const char *Path,uint32_t Token) {
    ArtRequested=Path!=NULL; ArtToken=Token; ArtReady=false;
    if (Path) ++ArtRequests;
}
static bool CopyArt(uint32_t Token,uint8_t *Pixels,bool *Valid) {
    if (!ArtReady || Token!=ArtToken) return false;
    for(unsigned Y=0;Y<PREVIEW_HEIGHT;++Y) for(unsigned X=0;X<PREVIEW_WIDTH;++X) {
        const uint16_t Color=X<24?0xf800:X<48?0x07e0:0x001f;
        const unsigned Offset=(Y*PREVIEW_WIDTH+X)*2;
        Pixels[Offset]=(uint8_t)Color; Pixels[Offset+1]=(uint8_t)(Color>>8);
    }
    *Valid=!ArtMissing; ArtReady=false; return true;
}
static bool VideoRequested, VideoReady, VideoMissing;
static unsigned VideoRequests;
static uint32_t VideoToken;
static void RequestVideo(const char *Path,uint32_t Token) {
    VideoRequested=Path!=NULL; VideoToken=Token; VideoReady=false;
    if(Path) ++VideoRequests;
}
static bool CopyVideo(uint32_t Token,uint8_t *Pixels,bool *Valid) {
    if(!VideoReady || Token!=VideoToken) return false;
    memset(Pixels,0xff,PREVIEW_BYTES); *Valid=!VideoMissing; VideoReady=false; return true;
}
static OSD_Result_t FirmwarePage(void *pArg) {
    return Firmware_Draw(((Tab_DrawCtx_t *)pArg)->pScreen);
}
static void CheckRowHeights(lv_obj_t *pObj, unsigned *pCount) {
    if (lv_obj_check_type(pObj, &lv_label_class) &&
        !lv_obj_has_flag(pObj, LV_OBJ_FLAG_HIDDEN) && lv_obj_get_width(pObj) == 52 &&
        lv_obj_get_y(pObj) >= 45 && lv_obj_get_y(pObj) <= 110) {
        assert(lv_obj_get_height(pObj) == 10);
        ++*pCount;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(pObj); ++i)
        CheckRowHeights(lv_obj_get_child(pObj, i), pCount);
}
static void CheckRows(lv_obj_t *Screen) {
    lv_obj_update_layout(Screen);
    unsigned Count = 0; CheckRowHeights(Screen, &Count); assert(Count == 0); /* ROM selections use the full panel. */
}

int main(void) {
    lv_init(); OSD_Common_Init(); Firmware_Initialize();
    assert(Battery_Init(&BatteryWidget) == kOSD_Result_Ok);
    Battery_UpdateVoltage(3.8f, kBatteryKind_AA);
    static lv_disp_draw_buf_t DrawBuffer;
    lv_disp_draw_buf_init(&DrawBuffer, Buffer, NULL, 160 * 144);
    static lv_disp_drv_t Driver; lv_disp_drv_init(&Driver);
    Driver.hor_res = 160; Driver.ver_res = 144; Driver.draw_buf = &DrawBuffer; Driver.flush_cb = Flush;
    lv_disp_drv_register(&Driver);
    lv_obj_t *Screen = lv_scr_act();
    lv_obj_set_style_bg_color(Screen, lv_color_hex(0x111111), 0);
    const RomBrowserDataSource_t Source = {
        .Open = Open, .NameAt = NameAt, .Close = Close, .Start = Start,
        .IsDirectory = IsDirectory, .Enter = Enter, .Parent = Parent, .AtRoot = AtRoot, .FolderPlatform = FolderPlatform, .ArtPath = ArtPath, .VideoPath = ArtPath,
    };
    RomBrowserUI_RegisterDataSource(&Source);
    OSD_Widget_t Menu = { .Name = "test" };
    assert(MenuMgr_Initialize(&Menu, Screen) == kOSD_Result_Ok);
    for (int i = 0; i < kNumTabIDs; ++i) {
        MenuTab_t *pTab = calloc(1, sizeof(*pTab));
        const MenuTab_t Template = { .pImageDesc = &menu_system };
        memcpy(pTab, &Template, sizeof(*pTab));
        pTab->Accent = lv_color_hex(0xff3399);
        if (i == kTabID_System) {
            pTab->Widget.fnDraw = FirmwarePage;
            pTab->Widget.fnOnTransition = Firmware_OnTransition;
            pTab->Widget.fnOnButton = Firmware_OnButton;
        }
        if (i == kTabID_Backups) {
            pTab->Accent = lv_color_hex(0x66ff66);
            pTab->Widget.fnDraw = RomBrowserUI_Draw;
            pTab->Widget.fnOnTransition = RomBrowserUI_OnTransition;
            pTab->Widget.fnOnButton = RomBrowserUI_OnButton;
        }
        assert(MenuMgr_AddTab(i, pTab) == kOSD_Result_Ok);
    }
    /* Boot into Backups, but do not touch the SD library until the menu opens. */
    MenuVisible = false;
    assert(DrawScreen(&Menu, Screen) == kOSD_Result_Ok);
    Menu.fnOnButton(kButton_A, kButtonState_Pressed, NULL);
    assert(Opens == 0 && Starts == 0);
    MenuVisible = true;
    PCMode = kPCBackupModeState_On;
    assert(DrawScreen(&Menu, Screen) == kOSD_Result_Ok);
    assert(Opens == 0 && Starts == 0);
    assert(Text(Screen, "DISABLE C. MAGICIAN\nIN SYSTEM SETTINGS"));
    Menu.fnOnButton(kButton_A, kButtonState_Pressed, NULL);
    assert(Opens == 0 && Starts == 0);
    PCMode = kPCBackupModeState_Off;
    assert(DrawScreen(&Menu, Screen) == kOSD_Result_Ok);
    assert(Opens == 1 && Starts == 0);
    assert(Text(Screen, "1/7"));
    Snapshot("backups");
    assert(Frame[11 * 160 + 127].full == lv_color_hex(0x80ff00).full);
    Battery_UpdateVoltage(2.7f, kBatteryKind_AA); DrawScreen(&Menu, Screen);
    Snapshot("backups-low-battery");
    assert(Frame[11 * 160 + 127].full == lv_color_hex(0xff0000).full);
    Battery_UpdateVoltage(3.8f, kBatteryKind_AA);
    Battery_SetChargingStatus(true); DrawScreen(&Menu, Screen);
    Snapshot("backups-charging");
    assert(Frame[15 * 160 + 132].full == lv_color_white().full);
    Battery_SetChargingStatus(false); DrawScreen(&Menu, Screen);
    lv_refr_now(NULL);
    for (int Y=42; Y<130; ++Y) assert(Frame[Y*160+11].full == lv_color_hex(0x66ff66).full);
    CheckRows(Screen);
    assert(Text(Screen, "BOMBERMAN") && Text(Screen, "MARIO"));
    assert(lv_obj_get_width(Text(Screen,"BOMBERMAN"))==120);
    assert(lv_obj_has_flag(Text(Screen,"MARIO"),LV_OBJ_FLAG_HIDDEN));
    DrawScreen(&Menu, Screen); assert(Opens == 1);
    Menu.fnOnButton(kButton_Up, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    assert(Text(Screen, "7/7"));
    ClockMs += 1000; DrawScreen(&Menu, Screen);
    lv_obj_t *Long = Text(Screen, "ZELDA AN EXTRA LONG BACKUP FILENAME FOR SCROLLING");
    assert(Long);
    assert(lv_label_get_long_mode(Long) == LV_LABEL_LONG_SCROLL_CIRCULAR);
    Snapshot("backups-scrolled");
    CheckRows(Screen);
    Menu.fnOnButton(kButton_Down, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    assert(Text(Screen, "1/7"));
    CheckRows(Screen);
    Menu.fnOnButton(kButton_Down, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    Menu.fnOnButton(kButton_A, kButtonState_Pressed, NULL);
    assert(Starts == 1 && strcmp(StartedName, "Kirby.gb") == 0);
    RomBrowserUI_SetProgress(1, 2); DrawScreen(&Menu, Screen);
    assert(Text(Screen, "LOADING GAME") && Text(Screen, "50%"));
    assert(lv_obj_is_visible(Text(Screen, "KIRBY")));
    lv_obj_t *LoadingPanel = lv_obj_get_parent(Text(Screen, "50%"));
    lv_obj_t *ProgressBar = lv_obj_get_child(LoadingPanel, 2);
    assert(lv_obj_check_type(ProgressBar, &lv_bar_class));
    assert(lv_bar_get_value(ProgressBar) == 50);
    Snapshot("backups-loading");
    RomBrowserUI_SetProgress(3, 2); DrawScreen(&Menu, Screen);
    assert(Text(Screen, "100%") && Text(Screen, "STARTING GAME"));
    assert(lv_bar_get_value(ProgressBar) == 100);
    Snapshot("backups-loading-complete");
    Menu.fnOnButton(kButton_Down, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    Menu.fnOnButton(kButton_A, kButtonState_Pressed, NULL); assert(Starts == 1);
    assert(RomBrowserUI_SetComplete()); DrawScreen(&Menu, Screen);
    assert(!lv_obj_is_visible(LoadingPanel));
    assert(Text(Screen, "2/7") && Opens == 1);
    Menu.fnOnButton(kButton_Down, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    Menu.fnOnButton(kButton_A, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    Long = Text(Screen, "LINKS AWAKENING DX AN EXTRA LONG BACKUP NAME");
    assert(Long && lv_obj_is_visible(Long));
    assert(lv_label_get_long_mode(Long) == LV_LABEL_LONG_SCROLL_CIRCULAR);
    RomBrowserUI_SetProgress(1, 4); DrawScreen(&Menu, Screen);
    assert(lv_obj_is_visible(Long) && Text(Screen, "25%"));
    Snapshot("backups-loading-long-title");
    assert(RomBrowserUI_SetComplete()); DrawScreen(&Menu, Screen);
    Menu.fnOnButton(kButton_Up, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    CompleteImmediately = true;
    Menu.fnOnButton(kButton_A, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    assert(Text(Screen, "2/7")); CompleteImmediately = false;
    Menu.fnOnButton(kButton_A, kButtonState_Pressed, NULL);
    Menu.fnOnButton(kButton_Left, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    assert(Closes == 1 && !RomBrowserUI_SetComplete());
    assert(Text(Screen, "1.0.0 (4.2)") && !Text(Screen, "SYSTEM    INFO"));
    Snapshot("firmware");
    Menu.fnOnButton(kButton_A, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    assert(Text(Screen, "MCU:") && Text(Screen, "FPGA:"));
    Menu.fnOnButton(kButton_Right, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    StartResult = ESP_FAIL;
    Menu.fnOnButton(kButton_A, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    assert(Text(Screen, "COULD NOT LOAD\nESP_FAIL"));
    StartResult = ESP_OK; Count = 0;
    Menu.fnOnButton(kButton_A, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    assert(Text(Screen, "NO BACKUPS\nON SD CARD"));
    OpenResult = ESP_FAIL;
    Menu.fnOnButton(kButton_A, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    assert(Text(Screen, "COULD NOT READ SD CARD"));
    OpenResult = ESP_ERR_NOT_FOUND;
    Menu.fnOnButton(kButton_A, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    assert(Text(Screen, "NO SD CARD INSTALLED"));
    OpenResult = ESP_ERR_TIMEOUT;
    Menu.fnOnButton(kButton_A, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    assert(Text(Screen, "SD CARD NOT RESPONDING"));
    OpenResult = ESP_OK; Count = 7;
    Menu.fnOnButton(kButton_A, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    assert(Text(Screen, "1/7"));
    PCMode = kPCBackupModeState_On;
    const int PreviousStarts = Starts;
    DrawScreen(&Menu, Screen);
    assert(Text(Screen, "DISABLE C. MAGICIAN\nIN SYSTEM SETTINGS"));
    Menu.fnOnButton(kButton_A, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    assert(Starts == PreviousStarts);
    assert(Text(Screen, "DISABLE C. MAGICIAN\nIN SYSTEM SETTINGS"));
    Snapshot("backups-disable-magician");
    PCMode = kPCBackupModeState_Off;
    DrawScreen(&Menu, Screen);
    assert(Text(Screen, "1/7"));
    Menu.fnOnTransition(NULL);
    FolderFixture = true;
    DrawScreen(&Menu, Screen);
    const int FolderStarts = Starts, FolderOpens = Opens;
    Menu.fnOnButton(kButton_B, kButtonState_Pressed, NULL);
    assert(Depth == 0 && Opens == FolderOpens);
    Menu.fnOnButton(kButton_Down, kButtonState_Pressed, NULL);
    DrawScreen(&Menu, Screen);
    Snapshot("backups-folders");
    FixturePlatform = kRomPlatformGB;
    /* Force a selection redraw, as a real directory change does. */
    Menu.fnOnButton(kButton_Up, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    Menu.fnOnButton(kButton_Down, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    Snapshot("backups-gb-folder");
    assert(Text(Screen, Depth ? "GAME BOY COLOR" : "GAME BOY"));
    FixturePlatform = kRomPlatformGBC;

    assert(Text(Screen, "JAPAN.V1"));
    Menu.fnOnButton(kButton_A, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    ClockMs += 1000; DrawScreen(&Menu, Screen);
    assert(Depth == 1 && Starts == FolderStarts && Text(Screen, "EMPTY.FOLDER"));
    Snapshot("backups-inside-folder");
    assert(Text(Screen, Depth ? "GAME BOY COLOR" : "GAME BOY"));
    FixturePlatform = kRomPlatformUnknown;
    Menu.fnOnButton(kButton_A, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    assert(Depth == 2 && Text(Screen, "EMPTY FOLDER"));
    Menu.fnOnButton(kButton_A, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    assert(Depth == 2);
    Menu.fnOnButton(kButton_B, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    assert(Depth == 1 && Text(Screen, "1/2"));
    Menu.fnOnButton(kButton_Down, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    Menu.fnOnButton(kButton_A, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    assert(Starts == FolderStarts + 1 && !strcmp(StartedName, "Pokemon.gbc"));
    Menu.fnOnButton(kButton_B, kButtonState_Pressed, NULL);
    assert(Depth == 1);
    assert(RomBrowserUI_SetComplete()); DrawScreen(&Menu, Screen);
    assert(Text(Screen, "2/2"));
    PCMode = kPCBackupModeState_On;
    Menu.fnOnButton(kButton_B, kButtonState_Pressed, NULL);
    Menu.fnOnButton(kButton_A, kButtonState_Pressed, NULL);
    assert(Depth == 1 && Starts == FolderStarts + 1);
    PCMode = kPCBackupModeState_Off;
    Menu.fnOnButton(kButton_B, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    assert(Depth == 0 && Text(Screen, "2/2"));
    OpenResult = ESP_ERR_NOT_FOUND;
    Menu.fnOnButton(kButton_A, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    assert(Depth == 1 && Text(Screen, "NO SD CARD INSTALLED"));
    OpenResult = ESP_OK;
    Menu.fnOnButton(kButton_B, kButtonState_Pressed, NULL); DrawScreen(&Menu, Screen);
    assert(Depth == 0 && Text(Screen, "2/2"));
    assert(Starts == FolderStarts + 1 && !Text(Screen, "B: BACK"));
    Menu.fnOnTransition(NULL);
    /* Two system logos own Left/Right only while the picker is focused. */
    FolderFixture=false; SystemFixture=true; HeldBits=0;
    DrawScreen(&Menu, Screen);
    assert(Text(Screen,"SYSTEMS") && SystemTile(Screen,0) && SystemTile(Screen,1));
    const int PickerStarts=Starts;
    Menu.fnOnButton(kButton_Left,kButtonState_Pressed,NULL); DrawScreen(&Menu, Screen);
    Snapshot("systems-game-boy");
    assert(lv_obj_get_style_border_color(SystemTile(Screen,0),0).full==lv_color_hex(0x66ff66).full);
    Menu.fnOnButton(kButton_A,kButtonState_Pressed,NULL); DrawScreen(&Menu, Screen);
    assert(Depth==1 && SystemIndex==1 && Text(Screen,"GAME BOY") && Text(Screen,"S-Z"));
    Snapshot("folders-game-boy");
    Menu.fnOnButton(kButton_B,kButtonState_Pressed,NULL); DrawScreen(&Menu, Screen);
    assert(Depth==0 && Text(Screen,"SYSTEMS"));
    Menu.fnOnButton(kButton_Right,kButtonState_Pressed,NULL); DrawScreen(&Menu, Screen);
    Snapshot("systems-game-boy-color");
    assert(lv_obj_get_style_border_color(SystemTile(Screen,1),0).full==lv_color_hex(0x66ff66).full);
    Menu.fnOnButton(kButton_A,kButtonState_Pressed,NULL); DrawScreen(&Menu, Screen);
    assert(Depth==1 && SystemIndex==0 && Text(Screen,"GAME BOY COLOR"));
    for(unsigned I=0;I<3;++I) { Menu.fnOnButton(kButton_Down,kButtonState_Pressed,NULL); DrawScreen(&Menu, Screen); }
    assert(Text(Screen,"4/4")); Snapshot("folders-game-boy-color");
    Menu.fnOnButton(kButton_B,kButtonState_Pressed,NULL); DrawScreen(&Menu, Screen);
    Menu.fnOnButton(kButton_B,kButtonState_Pressed,NULL); DrawScreen(&Menu, Screen);
    assert(Text(Screen,"<>:TABS A:CHOOSE"));
    Menu.fnOnButton(kButton_A,kButtonState_Pressed,NULL); DrawScreen(&Menu, Screen);
    assert(Depth==0 && Text(Screen,"<>:PICK A:OPEN B:TABS"));
    Menu.fnOnButton(kButton_Up,kButtonState_Pressed,NULL); DrawScreen(&Menu, Screen);
    Menu.fnOnButton(kButton_Down,kButtonState_Pressed,NULL); DrawScreen(&Menu, Screen);
    assert(Depth==0 && Text(Screen,"<>:PICK A:OPEN B:TABS"));
    Menu.fnOnButton(kButton_B,kButtonState_Pressed,NULL); DrawScreen(&Menu, Screen);
    Menu.fnOnButton(kButton_Left,kButtonState_Pressed,NULL); DrawScreen(&Menu, Screen);
    assert(!Text(Screen,"SYSTEMS")); /* back to the global tabs */
    Menu.fnOnButton(kButton_Right,kButtonState_Pressed,NULL); DrawScreen(&Menu, Screen);
    assert(Text(Screen,"SYSTEMS") && Text(Screen,"<>:PICK A:OPEN B:TABS") && Starts==PickerStarts);
    Menu.fnOnTransition(NULL); SystemFixture=false;
    /* Drive the production UI against delayed background jobs and art results. */
    FolderFixture=false; Count=7; HeldBits=0; ClockMs=0;
    const RomBrowserAsync_t Async={.Begin=BeginAsync,.Poll=PollAsync,.Cancel=CancelAsync,
        .RequestArt=RequestArt,.CopyArt=CopyArt,.RequestVideo=RequestVideo,.CopyVideo=CopyVideo};
    RomBrowserUI_RegisterAsync(&Async);
    DrawScreen(&Menu, Screen); assert(Text(Screen,"READING LIBRARY...") && AsyncBegins==1);
    Menu.fnOnButton(kButton_A,kButtonState_Pressed,NULL); assert(AsyncBegins==1);
    JobReady=true; DrawScreen(&Menu, Screen); assert(Text(Screen,"1/7"));
    assert(lv_obj_has_flag(Text(Screen,"NO COVER"), LV_OBJ_FLAG_HIDDEN));
    assert(ClockMs==0 && ArtRequested && ArtRequests==1); /* initial selection: no timer wait */
    ArtReady=true; DrawScreen(&Menu, Screen); Snapshot("backups-color-cover");
    assert(Frame[44*160+16].full==0xf800 && Frame[44*160+40].full==0x07e0 && Frame[44*160+64].full==0x001f);
    Menu.fnOnButton(kButton_Down,kButtonState_Pressed,NULL); DrawScreen(&Menu, Screen);
    assert(Text(Screen,"2/7") && ArtRequested && ArtRequests==2 && ClockMs==0);
    Snapshot("backups-pending-cover");
    assert(lv_obj_has_flag(Text(Screen,"NO COVER"), LV_OBJ_FLAG_HIDDEN));
    assert(Frame[44*160+16].full>0 && Frame[44*160+16].full<0xf800);
    lv_obj_t *List = OverlayList(Screen); assert(List && lv_obj_is_visible(List));
    assert(lv_obj_get_style_opa(List,0)==LV_OPA_COVER);
    unsigned Visible = 0;
    for(unsigned I=1;I<lv_obj_get_child_cnt(List);++I) Visible += lv_obj_is_visible(lv_obj_get_child(List,I));
    assert(Visible==5); /* neighboring titles, with a distinct selected row */
    assert(lv_obj_get_height(lv_obj_get_child(List,2))==10);
    assert(lv_obj_get_style_text_color(lv_obj_get_child(List,2),0).full==lv_color_black().full);
    DrawScreen(&Menu, Screen); assert(ArtRequests==2); /* don't repeat a pending request */
    Menu.fnOnButton(kButton_Up,kButtonState_Pressed,NULL); DrawScreen(&Menu, Screen);
    assert(Text(Screen,"1/7") && ArtRequests==3);
    ClockMs=160;
    HeldBits=kButtonBits_Down;
    Menu.fnOnButton(kButton_Down,kButtonState_Pressed,NULL); DrawScreen(&Menu, Screen);
    assert(Text(Screen,"2/7") && ArtRequested && ArtRequests==4);
    for(ClockMs=180;ClockMs<=1380;ClockMs+=20) DrawScreen(&Menu, Screen);
    assert(!ArtRequested); /* fast tier cancels queued slow art, including at the endpoint */
    const unsigned BeforeFast = ArtRequests;
    for(;ClockMs<=1500;ClockMs+=20) DrawScreen(&Menu, Screen);
    assert(ArtRequests==BeforeFast && !ArtRequested);
    assert(Text(Screen,"7/7"));
    assert(lv_obj_is_visible(List));
    Snapshot("backups-fast-list");
    assert(Frame[44*160+16].full>0 && Frame[44*160+16].full<0xf800);
    HeldBits=0; DrawScreen(&Menu, Screen);
    assert(ArtRequested && ArtRequests==BeforeFast+1 && lv_obj_is_visible(List));
    ArtReady=true; DrawScreen(&Menu, Screen); /* artwork must not dismiss the list early */
    assert(Text(Screen,"7/7") && lv_obj_is_visible(List));
    Menu.fnOnButton(kButton_Up,kButtonState_Pressed,NULL);
    Menu.fnOnButton(kButton_A,kButtonState_Pressed,NULL);
    assert(AsyncAction==kRomActionStart && AsyncIndex==6); /* launch what was displayed */
    AsyncError=ESP_FAIL; JobReady=true; DrawScreen(&Menu, Screen);
    assert(Text(Screen,"COULD NOT LOAD\nESP_FAIL"));
    AsyncError=ESP_OK; Menu.fnOnButton(kButton_A,kButtonState_Pressed,NULL);
    JobReady=true; DrawScreen(&Menu, Screen);
    MenuVisible=false; HeldBits=kButtonBits_Down; DrawScreen(&Menu, Screen); assert(AsyncCancels>0);
    MenuVisible=true; DrawScreen(&Menu, Screen); JobReady=true; DrawScreen(&Menu, Screen);
    ClockMs+=1000; DrawScreen(&Menu, Screen); assert(Text(Screen,"1/7")); /* held across reopen */
    HeldBits=0; DrawScreen(&Menu, Screen);
    Menu.fnOnButton(kButton_Down,kButtonState_Pressed,NULL); DrawScreen(&Menu, Screen); assert(Text(Screen,"2/7"));
    /* Release one millisecond after a fast selection change: no 150 ms delay.
     * A stalled frame keeps this small fixture away from its endpoint. */
    Menu.fnOnTransition(NULL); HeldBits=0; ClockMs=3000;
    DrawScreen(&Menu, Screen); JobReady=true; DrawScreen(&Menu, Screen);
    HeldBits=kButtonBits_Down; Menu.fnOnButton(kButton_Down,kButtonState_Pressed,NULL); DrawScreen(&Menu, Screen);
    ClockMs=4200; DrawScreen(&Menu, Screen); assert(Text(Screen,"2/7") && !ArtRequested);
    ClockMs=4280; DrawScreen(&Menu, Screen); assert(Text(Screen,"3/7") && !ArtRequested);
    const unsigned BeforeRelease=ArtRequests;
    ClockMs=4281; HeldBits=0; DrawScreen(&Menu, Screen);
    assert(ArtRequested && ArtRequests==BeforeRelease+1);
    List=OverlayList(Screen); assert(List && lv_obj_is_visible(List));
    ArtMissing=true; ArtReady=true; DrawScreen(&Menu, Screen);
    assert(lv_obj_has_flag(Text(Screen,"NO COVER"),LV_OBJ_FLAG_HIDDEN));
    ArtMissing=false;
    /* Slow/missing responses cannot hold the list open; the fade is input-driven. */
    HeldBits=kButtonBits_Down; Menu.fnOnButton(kButton_Down,kButtonState_Pressed,NULL); DrawScreen(&Menu, Screen);
    ClockMs=5481; DrawScreen(&Menu, Screen);
    ClockMs=5482; HeldBits=0; DrawScreen(&Menu, Screen);
    assert(lv_obj_is_visible(List));
    ClockMs=5831; DrawScreen(&Menu, Screen); assert(lv_obj_get_style_opa(List,0)==LV_OPA_COVER);
    ClockMs=5906; DrawScreen(&Menu, Screen);
    assert(lv_obj_get_style_opa(List,0)>0 && lv_obj_get_style_opa(List,0)<LV_OPA_COVER);
    ClockMs=5981; DrawScreen(&Menu, Screen); assert(!lv_obj_is_visible(List));
    ArtReady=true; DrawScreen(&Menu, Screen); assert(ArtRequests==BeforeRelease+3);
    /* Cover dwell, video frame updates, stream failure fallback and cancellation. */
    Menu.fnOnTransition(NULL); HeldBits=0; ClockMs=10000;
    DrawScreen(&Menu, Screen); JobReady=true; DrawScreen(&Menu, Screen);
    ArtReady=true; DrawScreen(&Menu, Screen);
    const unsigned BeforeVideo=VideoRequests;
    ClockMs=10999; DrawScreen(&Menu, Screen); assert(VideoRequests==BeforeVideo);
    ClockMs=11000; DrawScreen(&Menu, Screen); assert(VideoRequests==BeforeVideo);
    ClockMs=11499; DrawScreen(&Menu, Screen); assert(VideoRequests==BeforeVideo);
    ClockMs=11500; DrawScreen(&Menu, Screen); assert(VideoRequests==BeforeVideo+1);
    VideoReady=true; DrawScreen(&Menu, Screen); Snapshot("backups-video");
    assert(Frame[44*160+16].full==0xffff);
    VideoMissing=true; VideoReady=true; DrawScreen(&Menu, Screen); assert(ArtRequested);
    ArtReady=true; DrawScreen(&Menu, Screen); ClockMs+=2000; DrawScreen(&Menu, Screen);
    assert(VideoRequests==BeforeVideo+1); /* failed clips do not repeatedly retry */
    VideoMissing=false;
    Menu.fnOnButton(kButton_Down,kButtonState_Pressed,NULL); DrawScreen(&Menu, Screen);
    assert(!VideoRequested); /* selection cancels before another frame is consumed */
    List=OverlayList(Screen); assert(lv_obj_is_visible(List));
    ArtReady=true; DrawScreen(&Menu, Screen);
    const uint64_t StoppedAt=ClockMs;
    ClockMs=StoppedAt+500; DrawScreen(&Menu, Screen); assert(!lv_obj_is_visible(List));
    ClockMs=StoppedAt+1999; DrawScreen(&Menu, Screen); assert(!VideoRequested);
    ClockMs=StoppedAt+2000; DrawScreen(&Menu, Screen); assert(VideoRequested);
    VideoReady=true; DrawScreen(&Menu, Screen);
    /* A new tap instantly restores a list that was fading. */
    Menu.fnOnButton(kButton_Up,kButtonState_Pressed,NULL); DrawScreen(&Menu, Screen);
    Snapshot("backups-list-frozen-video");
    assert(Frame[44*160+16].full>0 && Frame[44*160+16].full<0xffff);
    ClockMs+=425; DrawScreen(&Menu, Screen); assert(lv_obj_get_style_opa(List,0)<LV_OPA_COVER);
    Menu.fnOnButton(kButton_Down,kButtonState_Pressed,NULL); DrawScreen(&Menu, Screen);
    assert(lv_obj_get_style_opa(List,0)==LV_OPA_COVER);
    Menu.fnOnTransition(NULL);
    lv_refr_now(NULL);
    lv_mem_monitor_t BeforeCycles, AfterCycles;
    lv_mem_monitor(&BeforeCycles);
    for (unsigned Cycle=0; Cycle<100; ++Cycle) {
        DrawScreen(&Menu, Screen); JobReady=true; DrawScreen(&Menu, Screen);
        ArtReady=true; DrawScreen(&Menu, Screen);
        Menu.fnOnButton(kButton_Down,kButtonState_Pressed,NULL); DrawScreen(&Menu, Screen);
        ClockMs+=425; DrawScreen(&Menu, Screen); /* close during an overlay fade */
        lv_refr_now(NULL);
        Menu.fnOnTransition(NULL);
        lv_refr_now(NULL);
    }
    lv_mem_monitor(&AfterCycles);
    assert(AfterCycles.free_size==BeforeCycles.free_size);
    RomBrowserUI_RegisterAsync(NULL);
    lv_obj_t *Background = lv_img_create(Screen);
    lv_img_set_src(Background, &menu_system);
    lv_obj_set_pos(Background, 16, 7);
    CartBackupUI_RegisterStartCallback(BackupStart);
    CartBackupUI_Draw(Screen);
    assert(Text(Screen, "BACK UP\nTO SD CARD"));
    lv_obj_t *Button = lv_obj_get_child(Screen, lv_obj_get_child_cnt(Screen) - 2);
    assert(lv_obj_get_style_radius(Button, 0) == LV_RADIUS_CIRCLE);
    assert(lv_obj_get_child_cnt(Button) == 2);
    Snapshot("cart-backup-button");
    PCMode = kPCBackupModeState_On;
    CartBackupUI_Draw(Screen);
    assert(Text(Screen, "DISABLE\nC. MAGICIAN\nIN SYSTEM\nSETTINGS"));
    CartBackupUI_OnButton(kButton_A, kButtonState_Pressed, NULL);
    CartBackupUI_Draw(Screen);
    assert(BackupStarts == 0);
    assert(Text(Screen, "DISABLE\nC. MAGICIAN\nIN SYSTEM\nSETTINGS"));
    Snapshot("cart-backup-disable-magician");
    PCMode = kPCBackupModeState_Off;
    CartBackupUI_Draw(Screen);
    assert(Text(Screen, "BACK UP\nTO SD CARD"));
    CartBackupUI_OnButton(kButton_A, kButtonState_Pressed, NULL);
    assert(BackupStarts == 1);
    CartBackupUI_OnTransition(NULL);
    lv_obj_del(Background);
    puts("PASS: menu routing, list scrolling, exact file selection, progress, immediate completion, errors, cleanup, firmware layout");
    return 0;
}
