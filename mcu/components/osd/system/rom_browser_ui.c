#include "rom_browser_ui.h"
#include "rom_browser_scroll.h"
#include "rom_title.h"
#include "box_art_format.h"
#include "preview_format.h"
LV_FONT_DECLARE(chromatic_banner);
LV_FONT_DECLARE(chromatic_progress);
#include "pc_backup_mode.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "platform_logos.h"
#include "tab_shared.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if LV_COLOR_DEPTH != 16 || LV_COLOR_16_SWAP
#error "Box art requires native unswapped RGB565; update the converter for other formats."
#endif

enum { kCoverDwellMs = 1500,
       kListHoldMs = 350, kListFadeMs = 150, kOverlayRows = 5, kFolderRows = 4,
       kVisibleRows = 6, kRowHeight = 13, kListX = 92,
       kListWidth = 52, kCoverWidth = BOX_ART_WIDTH, kCoverHeight = BOX_ART_HEIGHT };
typedef enum { kIdle, kCatalog, kSelecting, kEmpty, kLoading, kError, kBlocked } State_t;
typedef struct {
    lv_obj_t *Rows[kVisibleRows], *Selection, *Status, *Hint, *Position, *List;
    lv_obj_t *Cover, *Image, *Placeholder, *Headline, *Title;
    lv_obj_t *Systems, *SystemTiles[2], *Arrows[kVisibleRows];
    lv_obj_t *Loading, *LoadingText, *Percent, *ProgressBar;
    lv_img_dsc_t Art;
    uint8_t *Pixels;
    RomBrowserDataSource_t Source;
    RomBrowserAsync_t Async;
    RomBrowserScroll_t Scroll;
    State_t State;
    size_t Count, Selected;
    size_t PendingSystem;
    unsigned Pressed;
    uint64_t ChangedAt, OverlayUntil, CoverAt, ListUntil;
    uint32_t ArtToken;
    bool CatalogOpen, TrackingLoad, RenderValid, RenderedBlocked;
    bool ArtRequested, ArtValid, ArtResolved, Fast, Marquee, VideoRequested, VideoPlaying;
    bool SystemFocus, SystemPending;
    uint8_t Progress, ListOpacity;
    char CurrentName[256], Error[40];
} Context_t;
static Context_t C;
static portMUX_TYPE ContextLock = portMUX_INITIALIZER_UNLOCKED;

static uint64_t Now(void) { return (uint64_t)esp_timer_get_time() / 1000u; }
static unsigned Held(void) {
    const uint16_t Bits = Button_GetHeldBits();
    return ((Bits & kButtonBits_Up) ? kScrollUp : 0) | ((Bits & kButtonBits_Down) ? kScrollDown : 0);
}
static bool AtRoot(void) { return !C.Source.AtRoot || C.Source.AtRoot(); }
static bool Directory(size_t Index) { return C.Source.IsDirectory && C.Source.IsDirectory(Index); }
static bool SystemPicker(size_t Indices[2]) {
    if (C.State != kSelecting || C.Count != 2 || !AtRoot() || !C.Source.FolderPlatform) return false;
    bool Found[2] = {false,false};
    for (size_t I=0; I<2; ++I) {
        const RomPlatform_t Platform = Directory(I) ? C.Source.FolderPlatform(I) : kRomPlatformUnknown;
        if (Platform != kRomPlatformGB && Platform != kRomPlatformGBC) return false;
        const unsigned Slot = Platform == kRomPlatformGBC;
        if (Found[Slot]) return false;
        Found[Slot]=true; Indices[Slot]=I;
    }
    return true;
}
static void Hide(lv_obj_t *Object, bool Hidden) {
    if (Hidden) lv_obj_add_flag(Object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(Object, LV_OBJ_FLAG_HIDDEN);
}
static void ResetScroll(void) {
    RomBrowserScroll_Reset(&C.Scroll, Held());
    C.Pressed = 0; C.OverlayUntil = 0; C.Fast = false;
    C.ListUntil = 0; C.ListOpacity = 0;
    C.SystemPending = false;
}
static void ForgetArt(void) {
    ++C.ArtToken;
    C.ArtRequested = C.ArtValid = C.ArtResolved = false;
    C.VideoRequested = C.VideoPlaying = false;
    C.CoverAt = 0;
    if (C.Async.RequestVideo) C.Async.RequestVideo(NULL, C.ArtToken);
    if (C.Image) Hide(C.Image, true);
    if (C.Async.RequestArt) C.Async.RequestArt(NULL, C.ArtToken);
}
static void SelectionChanged(void) {
    C.ChangedAt = Now(); C.RenderValid = false; C.Marquee = false;
    const bool KeepCover = C.ArtValid && C.Count && !Directory(C.Selected);
    ForgetArt();
    /* Leave the last pixels in place until this selection's lookup completes. */
    C.ArtValid = KeepCover;
    if (C.Source.Remember && C.Count) C.Source.Remember(C.Selected);
}
static void DisplayName(size_t Index, char Output[256]) {
    const bool Folder = Directory(Index);
    RomTitle_Make(C.Source.NameAt(Index), Folder, Output, 256);
}
static void SetCatalog(esp_err_t Error, size_t Count, size_t Selected) {
    C.CatalogOpen = true;
    C.Count = Error == ESP_OK ? Count : 0;
    C.Selected = Selected < C.Count ? Selected : 0;
    C.State = Error != ESP_OK ? kError : Count ? kSelecting : kEmpty;
    if (Error != ESP_OK) snprintf(C.Error, sizeof(C.Error), "%s",
        Error == ESP_ERR_NOT_FOUND ? "NO SD CARD INSTALLED" :
        Error == ESP_ERR_TIMEOUT ? "SD CARD NOT RESPONDING" : "COULD NOT READ SD CARD");
    ResetScroll(); C.SystemFocus = true; SelectionChanged();
}
static void Begin(RomBrowserAction_t Action) {
    ResetScroll(); ForgetArt(); C.RenderValid = false;
    if (Action == kRomActionStart) {
        lv_img_cache_invalidate_src(&C.Art);
        free(C.Pixels); C.Pixels = NULL; C.Art = (lv_img_dsc_t){0};
    }
    if (C.Async.Begin) {
        C.State = Action == kRomActionStart ? kLoading : kCatalog;
        C.CatalogOpen = true;
        const esp_err_t Error = C.Async.Begin(Action, C.Selected);
        if (Error != ESP_OK) {
            if (Action == kRomActionStart) RomBrowserUI_SetError(Error);
            else SetCatalog(Error, 0, 0);
        }
        return;
    }
    /* Synchronous fallback is used by host fixtures and simple data sources. */
    size_t Count = 0, Selected = 0;
    esp_err_t Error;
    if (Action == kRomActionStart) {
        C.State = kLoading;
        Error = C.Source.Start(C.Source.NameAt(C.Selected));
        if (Error != ESP_OK) RomBrowserUI_SetError(Error);
        return;
    }
    if (Action == kRomActionEnter) Error = C.Source.Enter(C.Selected, &Count);
    else if (Action == kRomActionParent) Error = C.Source.Parent(&Count, &Selected);
    else Error = C.Source.Open(&Count);
    if (Error == ESP_OK && Action != kRomActionParent && C.Source.Restore) Selected = C.Source.Restore();
    SetCatalog(Error, Count, Selected);
}
static void Open(void) {
    if (!C.Source.Open || !C.Source.NameAt || !C.Source.Close || !C.Source.Start) {
        strcpy(C.Error, "NOT READY"); C.State = kError; C.RenderValid = false; return;
    }
    Begin(kRomActionOpen);
}
void RomBrowserUI_RegisterDataSource(const RomBrowserDataSource_t *Source) { if (Source) C.Source = *Source; }
void RomBrowserUI_RegisterAsync(const RomBrowserAsync_t *Backend) { C.Async = Backend ? *Backend : (RomBrowserAsync_t){0}; }
void RomBrowserUI_SetProgress(uint32_t Completed, uint32_t Total) {
    portENTER_CRITICAL(&ContextLock);
    if (C.TrackingLoad && C.State == kLoading && Total) {
        const uint8_t Progress = (uint8_t)(((uint64_t)(Completed < Total ? Completed : Total) * 100u) / Total);
        if (C.Progress != Progress) { C.Progress = Progress; C.RenderValid = false; }
    }
    portEXIT_CRITICAL(&ContextLock);
}
bool RomBrowserUI_SetComplete(void) {
    portENTER_CRITICAL(&ContextLock);
    const bool Close = C.TrackingLoad;
    if (Close) { C.TrackingLoad = false; C.State = kSelecting; C.Progress = 0; C.RenderValid = false; }
    portEXIT_CRITICAL(&ContextLock);
    return Close;
}
void RomBrowserUI_SetError(esp_err_t Error) {
    portENTER_CRITICAL(&ContextLock);
    if (C.TrackingLoad) {
        snprintf(C.Error, sizeof(C.Error), "COULD NOT LOAD\n%s", esp_err_to_name(Error));
        C.State = kError; C.RenderValid = false;
    }
    portEXIT_CRITICAL(&ContextLock);
}
static lv_obj_t *Label(lv_obj_t *Parent, int X, int Y, int Width) {
    lv_obj_t *Object = lv_label_create(Parent);
    lv_obj_add_style(Object, OSD_GetStyleTextWhite(), 0);
    lv_obj_set_pos(Object, X, Y); lv_obj_set_width(Object, Width);
    return Object;
}
static void Create(lv_obj_t *Screen, lv_color_t Accent) {
    C.Headline = lv_obj_create(Screen); lv_obj_remove_style_all(C.Headline);
    lv_obj_set_pos(C.Headline,16,27); lv_obj_set_size(C.Headline,128,12);
    lv_obj_clear_flag(C.Headline,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(C.Headline,Accent,0);
    lv_obj_set_style_bg_opa(C.Headline,LV_OPA_COVER,0);
    C.Title=Label(C.Headline,4,2,120);
    lv_obj_set_height(C.Title,8);
    lv_obj_set_style_text_font(C.Title,&chromatic_banner,0);
    lv_obj_set_style_text_color(C.Title,lv_color_black(),0);
    lv_obj_set_style_anim_speed(C.Title,24,0);
    lv_label_set_long_mode(C.Title,LV_LABEL_LONG_CLIP);
    Hide(C.Headline,true);
    C.Status = Label(Screen, 18, 48, 124);
    lv_obj_set_height(C.Status, 76);
    lv_label_set_long_mode(C.Status, LV_LABEL_LONG_WRAP);
    C.Hint = Label(Screen, kListX, 128, kListWidth);
    C.Position = Label(Screen, 16, 128, kCoverWidth);
    lv_obj_set_style_text_align(C.Position, LV_TEXT_ALIGN_CENTER, 0);
    C.Selection = lv_obj_create(Screen); lv_obj_remove_style_all(C.Selection);
    lv_obj_set_size(C.Selection, kListWidth + 4, 12);
    lv_obj_set_style_bg_color(C.Selection, Accent, 0);
    lv_obj_set_style_bg_opa(C.Selection, LV_OPA_COVER, 0);
    for (size_t I = 0; I < kVisibleRows; ++I) {
        C.Rows[I] = Label(Screen, kListX, 45 + I * kRowHeight, kListWidth);
        lv_obj_set_height(C.Rows[I], 10);
        lv_label_set_long_mode(C.Rows[I], LV_LABEL_LONG_DOT);
    }
    C.Cover = lv_obj_create(Screen); lv_obj_remove_style_all(C.Cover);
    lv_obj_set_pos(C.Cover, 16, 44); lv_obj_set_size(C.Cover, kCoverWidth, kCoverHeight);
    lv_obj_clear_flag(C.Cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(C.Cover, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(C.Cover, LV_OPA_COVER, 0);
    C.Placeholder = Label(C.Cover, 0, 34, kCoverWidth);
    lv_obj_set_style_text_align(C.Placeholder, LV_TEXT_ALIGN_CENTER, 0);
    C.Image = lv_img_create(C.Cover); lv_obj_set_pos(C.Image, 0, 0); Hide(C.Image, true);
    lv_img_set_pivot(C.Image, 0, 0);
    lv_img_set_zoom(C.Image, LV_IMG_ZOOM_NONE);
    lv_img_set_antialias(C.Image, false);
    /* Reuse the folder rows in a single overlay, above the media. */
    C.List = lv_obj_create(Screen); lv_obj_remove_style_all(C.List);
    lv_obj_clear_flag(C.List, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(C.List, lv_color_black(), 0);
    lv_obj_set_parent(C.Selection, C.List);
    for (size_t I = 0; I < kVisibleRows; ++I) lv_obj_set_parent(C.Rows[I], C.List);
    for (size_t I = 0; I < kVisibleRows; ++I) {
        C.Arrows[I] = Label(C.List,116,0,8);
        lv_label_set_text_static(C.Arrows[I], ">");
    }
    Hide(C.List, true);
    C.Systems=lv_obj_create(Screen); lv_obj_remove_style_all(C.Systems);
    lv_obj_set_pos(C.Systems,16,44); lv_obj_set_size(C.Systems,128,80);
    lv_obj_clear_flag(C.Systems,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(C.Systems,lv_color_black(),0);
    lv_obj_set_style_bg_opa(C.Systems,LV_OPA_COVER,0);
    for (unsigned I=0;I<2;++I) {
        C.SystemTiles[I]=lv_obj_create(C.Systems); lv_obj_remove_style_all(C.SystemTiles[I]);
        lv_obj_set_pos(C.SystemTiles[I],I*64,4); lv_obj_set_size(C.SystemTiles[I],64,72);
        lv_obj_clear_flag(C.SystemTiles[I],LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(C.SystemTiles[I],LV_OPA_COVER,0);
        lv_obj_set_style_border_width(C.SystemTiles[I],2,0);
        lv_obj_t *Logo=lv_img_create(C.SystemTiles[I]);
        lv_img_set_src(Logo,I?&PlatformGBC:&PlatformGB); lv_obj_set_pos(Logo,0,0);
    }
    Hide(C.Systems,true);
    C.Loading = lv_obj_create(Screen); lv_obj_remove_style_all(C.Loading);
    lv_obj_set_pos(C.Loading, 16, 44); lv_obj_set_size(C.Loading, 128, 92);
    lv_obj_clear_flag(C.Loading, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(C.Loading, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(C.Loading, LV_OPA_COVER, 0);
    C.LoadingText = Label(C.Loading, 0, 10, 128);
    lv_obj_set_style_text_font(C.LoadingText, &chromatic_banner, 0);
    lv_obj_set_style_text_align(C.LoadingText, LV_TEXT_ALIGN_CENTER, 0);
    C.Percent = Label(C.Loading, 0, 31, 128);
    lv_obj_set_style_text_font(C.Percent, &chromatic_progress, 0);
    lv_obj_set_style_text_color(C.Percent, Accent, 0);
    lv_obj_set_style_text_align(C.Percent, LV_TEXT_ALIGN_CENTER, 0);
    C.ProgressBar = lv_bar_create(C.Loading); lv_obj_remove_style_all(C.ProgressBar);
    lv_obj_set_pos(C.ProgressBar, 8, 68); lv_obj_set_size(C.ProgressBar, 112, 9);
    lv_bar_set_range(C.ProgressBar, 0, 100);
    lv_obj_set_style_border_color(C.ProgressBar, Accent, LV_PART_MAIN);
    lv_obj_set_style_border_width(C.ProgressBar, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(C.ProgressBar, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(C.ProgressBar, Accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(C.ProgressBar, LV_OPA_COVER, LV_PART_INDICATOR);
    Hide(C.Loading, true);
    C.RenderValid = false;
}
static void TickSelection(uint64_t Time) {
    size_t Systems[2];
    if (SystemPicker(Systems)) {
        if (C.SystemPending) {
            const size_t Selected=C.PendingSystem; C.SystemPending=false;
            if (Selected!=C.Selected) { C.Selected=Selected; SelectionChanged(); }
        }
        return;
    }
    const unsigned Buttons = Held();
    const bool WasFast = C.Scroll.Rate >= 12;
    const RomBrowserScrollStep_t Step = RomBrowserScroll_Update(&C.Scroll, Time, Buttons, C.Pressed);
    C.Pressed = 0;
    const bool FastScrolling = C.Scroll.Rate >= 12;
    /* Invalidate queued/late slow-scroll art even at a list endpoint, where
     * the selection itself might not change when acceleration starts. */
    if (!WasFast && FastScrolling) {
        const bool KeepFrame = C.ArtValid && !Directory(C.Selected);
        ForgetArt(); C.ArtValid = KeepFrame;
    }
    if (Step.Steps || C.Scroll.Direction) C.ListUntil = Time + kListHoldMs + kListFadeMs;
    const uint64_t Remaining = C.ListUntil > Time ? C.ListUntil - Time : 0;
    const uint8_t Opacity = Remaining >= kListFadeMs ? LV_OPA_COVER :
        (uint8_t)(Remaining * LV_OPA_COVER / kListFadeMs);
    if (Opacity != C.ListOpacity) {
        /* Give the unobscured cover its full dwell after the list disappears. */
        if (!Opacity && C.ArtResolved) C.CoverAt = Time;
        C.ListOpacity = Opacity; C.RenderValid = false;
    }
    if (WasFast && !C.Scroll.Direction) C.OverlayUntil = Time + 250;
    if (C.Scroll.Direction && C.Scroll.Rate < 12) C.OverlayUntil = 0;
    if (Step.Steps && C.Count) {
        C.OverlayUntil = 0;
        size_t Selected = C.Selected;
        if (Step.Wrap && Step.Steps == -1 && !Selected) Selected = C.Count - 1;
        else if (Step.Wrap && Step.Steps == 1 && Selected + 1 == C.Count) Selected = 0;
        else if (Step.Steps < 0) Selected = (size_t)(-Step.Steps) > Selected ? 0 : Selected - (size_t)(-Step.Steps);
        else Selected = (size_t)Step.Steps >= C.Count - Selected ? C.Count - 1 : Selected + (size_t)Step.Steps;
        if (Selected != C.Selected) { C.Selected = Selected; SelectionChanged(); }
    }
    /* Queue immediately for taps, initial selection and release. The worker
     * still performs all SD access off the UI task; fast repeats request none. */
    if (!C.ArtRequested && !FastScrolling && !Directory(C.Selected)) {
        char Path[320];
        C.ArtRequested = true;
        if (!C.Pixels && C.Async.RequestArt && C.Async.CopyArt && C.Source.ArtPath)
            C.Pixels = malloc(PREVIEW_BYTES);
        if (C.Pixels && C.Async.RequestArt && C.Async.CopyArt && C.Source.ArtPath &&
            C.Source.ArtPath(C.Selected, Path, sizeof(Path))) C.Async.RequestArt(Path, C.ArtToken);
        else { C.ArtResolved = true; C.ArtValid = false; C.RenderValid = false; }
    }
    bool Valid;
    if (C.Pixels && C.Async.CopyArt && C.Async.CopyArt(C.ArtToken, C.Pixels, &Valid)) {
        lv_img_cache_invalidate_src(&C.Art);
        C.ArtValid = Valid;
        C.ArtResolved = true;
        C.CoverAt = Time;
        if (Valid) {
            /* Both ESP32 and the host fixture use unswapped little-endian RGB565. */
            C.Art = (lv_img_dsc_t){ .header = {.cf = LV_IMG_CF_TRUE_COLOR, .w = PREVIEW_WIDTH, .h = PREVIEW_HEIGHT},
                .data_size = PREVIEW_BYTES, .data = C.Pixels };
            lv_img_set_src(C.Image, &C.Art);
            /* Setting a source resets LVGL's pivot to the source center. */
            lv_img_set_pivot(C.Image, 0, 0);
        }
        /* A completed lookup (including missing art) ends the release overlay
         * in this frame. 250 ms is only the maximum linger while waiting. */
        if (!FastScrolling) C.OverlayUntil = 0;
        C.RenderValid = false;
    }
    if (!C.VideoRequested && !C.ListOpacity && !FastScrolling && !C.Scroll.Direction && C.ArtResolved &&
        Time-C.CoverAt >= kCoverDwellMs && !Directory(C.Selected) && C.Pixels &&
        C.Async.RequestVideo && C.Source.VideoPath) {
        char Path[320]; C.VideoRequested=true;
        if (C.Source.VideoPath(C.Selected,Path,sizeof(Path))) C.Async.RequestVideo(Path,C.ArtToken);
    }
    if (C.Pixels && C.Async.CopyVideo && C.Async.CopyVideo(C.ArtToken,C.Pixels,&Valid)) {
        if (Valid) {
            if (!C.VideoPlaying || !C.ArtValid) C.RenderValid=false;
            C.VideoPlaying=true; C.ArtValid=true;
            C.Art=(lv_img_dsc_t){.header={.cf=LV_IMG_CF_TRUE_COLOR,.w=PREVIEW_WIDTH,.h=PREVIEW_HEIGHT},
                .data_size=PREVIEW_BYTES,.data=C.Pixels};
            lv_img_cache_invalidate_src(&C.Art); lv_img_set_src(C.Image,&C.Art);
            lv_img_set_pivot(C.Image,0,0); lv_obj_invalidate(C.Image);
        } else if (C.VideoPlaying) {
            /* A failed stream restores the cover once; don't retry the clip. */
            C.VideoPlaying=false; C.ArtValid=false; C.RenderValid=false;
            char Path[320];
            if (C.Source.ArtPath && C.Source.ArtPath(C.Selected,Path,sizeof(Path)))
                C.Async.RequestArt(Path,C.ArtToken);
        }
    }
    const bool Fast = FastScrolling || Time < C.OverlayUntil;
    const bool Marquee = !C.Scroll.Direction && Time - C.ChangedAt >= 1000;
    if (Fast != C.Fast || Marquee != C.Marquee) C.RenderValid = false;
    C.Fast = Fast; C.Marquee = Marquee;
}
OSD_Result_t RomBrowserUI_Draw(void *Arg) {
    if (!Arg || !((Tab_DrawCtx_t *)Arg)->pScreen) return kOSD_Result_Err_NullDataPtr;
    const Tab_DrawCtx_t *Draw = Arg;
    const bool Blocked = PCBackupMode_GetState() == kPCBackupModeState_On;
    if (Blocked && !C.RenderedBlocked) {
        ResetScroll(); ForgetArt();
        if (C.Async.Cancel) C.Async.Cancel();
        else if (C.CatalogOpen) C.Source.Close();
        C.CatalogOpen = false; C.State = kIdle; C.TrackingLoad = false;
    }
    if (!Blocked && C.State == kIdle) Open();
    if (!C.Status) Create(Draw->pScreen, Draw->AccentColor);
    if (!Blocked && C.Async.Poll) {
        RomBrowserJobResult_t Job;
        if (C.Async.Poll(&Job)) {
            if (Job.Action == kRomActionStart) { if (Job.Error != ESP_OK) RomBrowserUI_SetError(Job.Error); }
            else SetCatalog(Job.Error, Job.Count, Job.Selected);
        }
    }
    if (!Blocked && C.State == kSelecting) TickSelection(Now());
    portENTER_CRITICAL(&ContextLock);
    const State_t State = Blocked ? kBlocked : C.State;
    const bool Dirty = !C.RenderValid || C.RenderedBlocked != Blocked;
    C.RenderValid = true; C.RenderedBlocked = Blocked;
    const uint8_t Progress = C.Progress;
    char Error[40]; memcpy(Error, C.Error, sizeof(Error));
    portEXIT_CRITICAL(&ContextLock);
    if (!Dirty) return kOSD_Result_Ok;
    const bool Selecting = State == kSelecting;
    const bool Game = Selecting && !Directory(C.Selected);
    size_t Systems[2];
    const bool Picker = Selecting && SystemPicker(Systems);
    const bool Folders = Selecting && !Game && !Picker;
    const bool Loading = State == kLoading;
    const bool Overlay = Game && C.ListOpacity;
    const bool GameHeading = Game || Loading || (State == kError && C.TrackingLoad);
    Hide(C.Headline,!GameHeading && !Folders && !Picker);
    Hide(C.Systems,!Picker);
    Hide(C.Loading,!Loading);
    Hide(C.Position,Game || Picker); Hide(C.Hint,Game);
    if (GameHeading || Folders || Picker) {
        lv_obj_set_style_text_letter_space(C.Title,Folders?-1:0,0);
        char Title[256];
        if (Game) DisplayName(C.Selected,Title);
        else if (Picker) strcpy(Title,"SYSTEMS");
        else if (Folders) {
            const RomPlatform_t Platform=C.Source.FolderPlatform ? C.Source.FolderPlatform(C.Selected) : kRomPlatformUnknown;
            strcpy(Title,Platform==kRomPlatformGB?"GAME BOY":Platform==kRomPlatformGBC?"GAME BOY COLOR":"FOLDERS");
        } else snprintf(Title,sizeof(Title),"%s",C.CurrentName);
        /* Replacing identical text restarts LVGL's marquee every video frame. */
        const lv_label_long_mode_t Mode=GameHeading && (C.Marquee || !Game) ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_CLIP;
        if (lv_label_get_long_mode(C.Title)!=Mode) lv_label_set_long_mode(C.Title,Mode);
        if (strcmp(lv_label_get_text(C.Title),Title)) lv_label_set_text(C.Title,Title);
    }
    lv_obj_set_size(C.Cover,Game?PREVIEW_WIDTH:kCoverWidth,Game?PREVIEW_HEIGHT:kCoverHeight);
    lv_obj_set_width(C.Placeholder,Game?PREVIEW_WIDTH:kCoverWidth);
    lv_obj_set_pos(C.Hint,Folders?50:16,128);
    lv_obj_set_width(C.Hint,Folders?94:128);
    lv_obj_set_style_text_align(C.Hint,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_width(C.Position,Folders?32:kCoverWidth);
    Hide(C.Status, Selecting || Loading);
    Hide(C.List, !Selecting || Picker || (Game && !Overlay));
    Hide(C.Selection, !Selecting || (Game && !Overlay));
    lv_obj_set_pos(C.List,16,44);
    lv_obj_set_size(C.List,128,Game?92:80);
    lv_obj_set_style_bg_opa(C.List,Game?LV_OPA_70:LV_OPA_COVER,0);
    lv_obj_set_style_opa(C.List, Game ? C.ListOpacity : LV_OPA_COVER, 0);
    lv_obj_set_size(C.Selection,Game?120:128,Game?14:18);
    Hide(C.Cover,!Game);
    if (Selecting) {
        if (Picker) {
            for(unsigned I=0;I<2;++I) {
                const bool Active=C.SystemFocus && C.Selected==Systems[I];
                lv_obj_set_style_border_color(C.SystemTiles[I],Active?Draw->AccentColor:lv_color_hex(0x444444),0);
                lv_obj_set_style_bg_color(C.SystemTiles[I],Active?Draw->AccentColor:lv_color_hex(0x202020),0);
            }
        }
        const size_t VisibleRows = Game ? kOverlayRows : kFolderRows;
        size_t First = C.Selected > 2 ? C.Selected - 2 : 0;
        if (C.Count > VisibleRows && First > C.Count - VisibleRows) First = C.Count - VisibleRows;
        if (C.Count <= VisibleRows) First = 0;
        for (size_t Row = 0; Row < kVisibleRows; ++Row) {
            const size_t Index = First + Row;
            Hide(C.Rows[Row], Picker || (Game && !Overlay) || Row >= VisibleRows || Index >= C.Count);
            Hide(C.Arrows[Row],!Folders || Row>=VisibleRows || Index>=C.Count);
            if (Picker || Row >= VisibleRows || Index >= C.Count) continue;
            lv_obj_set_pos(C.Rows[Row],Game?6:8,Game?8+Row*16:6+Row*20);
            lv_obj_set_width(C.Rows[Row],Game?116:100);
            lv_obj_set_height(C.Rows[Row],10);
            if (Folders) lv_obj_set_style_text_font(C.Rows[Row],&chromatic_banner,0);
            else lv_obj_remove_local_style_prop(C.Rows[Row],LV_STYLE_TEXT_FONT,0);
            lv_obj_set_pos(C.Arrows[Row],116,6+Row*20);
            char Name[256]; DisplayName(Index, Name);
            const bool Selected = Index == C.Selected;
            lv_label_set_long_mode(C.Rows[Row], Selected && C.Marquee && !Game
                ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_DOT);
            lv_label_set_text(C.Rows[Row], Name);
            lv_obj_set_style_text_color(C.Rows[Row], Selected ? lv_color_black() : lv_color_white(), 0);
            lv_obj_set_style_text_color(C.Arrows[Row],Selected?lv_color_black():lv_color_hex(0x888888),0);
            if (Selected) lv_obj_set_pos(C.Selection,Game?4:0,Game?6+Row*16:2+Row*20);
        }
        Hide(C.Image,!Game || !C.ArtValid);
        Hide(C.Placeholder, Overlay || C.Fast || C.ArtValid ||
            (!Directory(C.Selected) && !C.ArtResolved));
        lv_label_set_text_static(C.Placeholder, Directory(C.Selected) ? "FOLDER" : "NO COVER");
        lv_label_set_text_static(C.Hint,Picker ? (C.SystemFocus?"<>:PICK A:OPEN B:TABS":"<>:TABS A:CHOOSE") : AtRoot()?"A:OPEN":"A:OPEN B:BACK");
        lv_label_set_text_fmt(C.Position, "%u/%u", (unsigned)C.Selected + 1, (unsigned)C.Count);
    } else {
        for (size_t I = 0; I < kVisibleRows; ++I) Hide(C.Rows[I], true);
        lv_label_set_text_static(C.Position, "");
        lv_label_set_text_static(C.Hint, State == kLoading || State == kCatalog || Blocked ? "" :
            AtRoot() ? "A: REFRESH" : "A: REFRESH B: UP");
        switch (State) {
            case kCatalog: lv_label_set_text_static(C.Status, "READING LIBRARY..."); break;
            case kEmpty: lv_label_set_text_static(C.Status, AtRoot() ? "NO BACKUPS\nON SD CARD" : "EMPTY FOLDER"); break;
            case kLoading:
                lv_label_set_text_static(C.LoadingText, Progress == 100 ? "STARTING GAME" : "LOADING GAME");
                lv_label_set_text_fmt(C.Percent, "%u%%", (unsigned)Progress);
                lv_bar_set_value(C.ProgressBar, Progress, LV_ANIM_OFF);
                break;
            case kError: lv_label_set_text(C.Status, Error); break;
            case kBlocked: lv_label_set_text_static(C.Status, "DISABLE C. MAGICIAN\nIN SYSTEM SETTINGS"); break;
            default: break;
        }
    }
    return kOSD_Result_Ok;
}
bool RomBrowserUI_HandleHorizontal(Button_t Button, ButtonState_t State) {
    size_t Systems[2];
    if (!C.SystemFocus || !SystemPicker(Systems) || PCBackupMode_GetState()==kPCBackupModeState_On) return false;
    if (Button!=kButton_Left && Button!=kButton_Right) return false;
    if (State==kButtonState_Pressed) {
        C.PendingSystem=Systems[Button==kButton_Right]; C.SystemPending=true;
    }
    return true;
}
OSD_Result_t RomBrowserUI_OnButton(Button_t Button, ButtonState_t State, void *Arg) {
    (void)Arg;
    if (State != kButtonState_Pressed || PCBackupMode_GetState() == kPCBackupModeState_On) return kOSD_Result_Ok;
    size_t Systems[2];
    if (SystemPicker(Systems)) {
        if (Button==kButton_B || Button==kButton_Up) {
            ResetScroll(); C.SystemFocus=false; C.RenderValid=false; return kOSD_Result_Ok;
        }
        if (Button==kButton_Down || (Button==kButton_A && !C.SystemFocus)) {
            ResetScroll(); C.SystemFocus=true; C.RenderValid=false; return kOSD_Result_Ok;
        }
    }
    if (Button == kButton_B && C.State != kLoading && C.State != kCatalog) {
        C.TrackingLoad = false;
        if (!AtRoot() && C.Source.Parent) Begin(kRomActionParent);
        else if (C.State == kError) Open();
        return kOSD_Result_Ok;
    }
    if (C.State == kSelecting) {
        if (Button == kButton_Up || Button == kButton_Down)
            C.Pressed |= Button == kButton_Up ? kScrollUp : kScrollDown;
        else if (Button == kButton_A) {
            /* Selection advances only in Draw. A freezes the last displayed item. */
            ResetScroll();
            if (Directory(C.Selected) && C.Source.Enter) Begin(kRomActionEnter);
            else {
                RomTitle_Make(C.Source.NameAt(C.Selected), false, C.CurrentName, sizeof(C.CurrentName));
                portENTER_CRITICAL(&ContextLock);
                C.TrackingLoad = true; C.Progress = 0;
                portEXIT_CRITICAL(&ContextLock);
                Begin(kRomActionStart);
            }
        }
    } else if (C.State != kLoading && C.State != kCatalog && Button == kButton_A) {
        C.TrackingLoad = false; Open();
    }
    return kOSD_Result_Ok;
}
OSD_Result_t RomBrowserUI_OnTransition(void *Arg) {
    (void)Arg;
    ResetScroll(); ForgetArt();
    portENTER_CRITICAL(&ContextLock);
    C.TrackingLoad = false; C.State = kIdle; C.RenderValid = false;
    portEXIT_CRITICAL(&ContextLock);
    if (C.Async.Cancel) C.Async.Cancel();
    else if (C.CatalogOpen && C.Source.Close) C.Source.Close();
    C.CatalogOpen = false; C.Count = C.Selected = 0;
    lv_obj_t **Objects[] = {&C.Status, &C.Hint, &C.Position, &C.List, &C.Cover, &C.Headline, &C.Loading, &C.Systems};
    for (size_t I = 0; I < ARRAY_SIZE(Objects); ++I) {
        if (*Objects[I]) lv_obj_del(*Objects[I]);
        *Objects[I] = NULL;
    }
    C.Image = C.Placeholder = C.Title = NULL;
    for (unsigned I=0;I<2;++I) C.SystemTiles[I]=NULL;
    C.LoadingText = C.Percent = C.ProgressBar = NULL;
    C.Selection = NULL;
    for (size_t I = 0; I < kVisibleRows; ++I) C.Rows[I] = C.Arrows[I] = NULL;
    lv_img_cache_invalidate_src(&C.Art);
    free(C.Pixels); C.Pixels = NULL; C.Art = (lv_img_dsc_t){0};
    return kOSD_Result_Ok;
}
