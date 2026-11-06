#include "rom_browser_worker.h"
#include "rom_browser.h"
#include "box_art.h"
#include "video_preview.h"
#include "esp_timer.h"
#include "sd_card.h"
#include "pc_backup_mode.h"
#include "pwrmgr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif
static uint8_t *AllocatePixels(void) {
#ifdef ESP_PLATFORM
    /* Reserve byte-accessible DRAM for LVGL, FATFS and SDMMC DMA allocations. */
    return heap_caps_malloc(PREVIEW_BYTES, MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
#else
    return malloc(PREVIEW_BYTES);
#endif
}
static void TraceMedia(const char *Kind, bool Valid, esp_err_t Error, const char *Path) {
#ifdef ESP_PLATFORM
    printf("ROM_MEDIA kind=%s valid=%u error=%s heap8=%u largest_dma=%u path=%s\n",
        Kind,Valid,esp_err_to_name(Error),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),Path);
#else
    (void)Kind; (void)Valid; (void)Error; (void)Path;
#endif
}

/* One worker serializes catalog, artwork, and launch hand-off. Requests contain
 * owned strings; no worker keeps an LVGL object or a pointer into a freed catalog. */
typedef struct {
    RomBrowserAction_t Action;
    size_t Index;
    uint32_t Generation;
    char Name[256];
} Job_t;
static struct {
    TaskHandle_t Task;
    Job_t Job;
    uint32_t Generation, ArtToken;
    bool Pending, Done, ArtPending, ArtReady, ArtValid, Copying;
    char ArtPath[320], VideoPath[320];
    bool WantVideo, VideoPending, VideoReady, VideoValid;
    uint32_t VideoToken;
    /* Worker-task-only stream state. */
    VideoPreview_t Video;
    sdmmc_card_t *VideoCard;
    uint32_t StreamToken, StreamGeneration;
    uint64_t NextFrame;
    uint8_t *Pixels;
    RomBrowserJobResult_t Result;
} Worker;
static portMUX_TYPE Lock = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t Begin(RomBrowserAction_t Action, size_t Index)
{
    Job_t Job = {.Action = Action, .Index = Index};
    if (Action == kRomActionStart)
    {
        const char *Name = RomBrowser_GetDataSource()->NameAt(Index);
        if (!Name || strlen(Name) >= sizeof(Job.Name)) return ESP_ERR_INVALID_ARG;
        strcpy(Job.Name, Name);
    }
    portENTER_CRITICAL(&Lock);
    Job.Generation = ++Worker.Generation;
    Worker.Job = Job;
    Worker.Pending = true;
    Worker.Done = Worker.ArtPending = Worker.ArtReady = false;
    Worker.WantVideo = Worker.VideoPending = Worker.VideoReady = false;
    portEXIT_CRITICAL(&Lock);
    xTaskNotifyGive(Worker.Task);
    return ESP_OK;
}

static void Cancel(void) { (void)Begin(kRomActionClose, 0); }

static bool Poll(RomBrowserJobResult_t *Result)
{
    portENTER_CRITICAL(&Lock);
    const bool Done = Worker.Done;
    if (Done) { *Result = Worker.Result; Worker.Done = false; }
    portEXIT_CRITICAL(&Lock);
    return Done;
}

static void RequestArt(const char *Path, uint32_t Token)
{
    portENTER_CRITICAL(&Lock);
    Worker.ArtPending = Path && strlen(Path) < sizeof(Worker.ArtPath);
    Worker.ArtReady = false;
    Worker.WantVideo = Worker.VideoPending = Worker.VideoReady = false;
    Worker.ArtToken = Token;
    if (Worker.ArtPending) strcpy(Worker.ArtPath, Path);
    portEXIT_CRITICAL(&Lock);
    xTaskNotifyGive(Worker.Task);
}

static bool CopyArt(uint32_t Token, uint8_t *Pixels, bool *Valid)
{
    portENTER_CRITICAL(&Lock);
    const bool Ready = Worker.ArtReady && Worker.ArtToken == Token;
    if (Ready) { Worker.Copying = true; *Valid = Worker.ArtValid; }
    portEXIT_CRITICAL(&Lock);
    if (!Ready) return false;
    if (*Valid) Preview_CopyToBytes(Pixels, Worker.Pixels);
    portENTER_CRITICAL(&Lock);
    Worker.Copying = Worker.ArtReady = false;
    portEXIT_CRITICAL(&Lock);
    return true;
}

static void RequestVideo(const char *Path, uint32_t Token)
{
    portENTER_CRITICAL(&Lock);
    Worker.WantVideo = Path && strlen(Path) < sizeof(Worker.VideoPath);
    Worker.VideoPending = Worker.WantVideo;
    Worker.VideoReady = false;
    Worker.VideoToken = Token;
    if (Worker.WantVideo) strcpy(Worker.VideoPath, Path);
    portEXIT_CRITICAL(&Lock);
    xTaskNotifyGive(Worker.Task);
}
static bool CopyVideo(uint32_t Token, uint8_t *Pixels, bool *Valid)
{
    portENTER_CRITICAL(&Lock);
    const bool Ready = Worker.VideoReady && Worker.VideoToken == Token;
    if (Ready) { Worker.Copying = true; *Valid = Worker.VideoValid; }
    portEXIT_CRITICAL(&Lock);
    if (!Ready) return false;
    if (*Valid) Preview_CopyToBytes(Pixels, Worker.Pixels);
    portENTER_CRITICAL(&Lock);
    Worker.Copying = Worker.VideoReady = false;
    portEXIT_CRITICAL(&Lock);
    return true;
}
static void CloseVideo(void)
{
    if (Worker.Video.File) fclose(Worker.Video.File);
    Worker.Video = (VideoPreview_t){0};
    if (Worker.VideoCard) SDCard_Unmount(Worker.VideoCard);
    Worker.VideoCard = NULL;
}
/* Called with the sleep inhibitor held. A single frame is the maximum work
 * between cancellation/yield checks; no UI object is touched by this task. */
static void PumpVideo(void)
{
    char Path[320];
    portENTER_CRITICAL(&Lock);
    const bool Want = Worker.WantVideo;
    const bool Pending = Worker.VideoPending;
    const bool Busy = Worker.Copying || Worker.ArtReady || Worker.VideoReady;
    const uint32_t Token = Worker.VideoToken, Generation = Worker.Generation;
    if (Pending && !Busy) { strcpy(Path,Worker.VideoPath); Worker.VideoPending=false; }
    portEXIT_CRITICAL(&Lock);
    if (Worker.VideoCard && (!Want || Token!=Worker.StreamToken ||
        Generation!=Worker.StreamGeneration || Pending)) CloseVideo();
    if (!Want) return;
    if (Worker.VideoCard && SDCard_PreviewShouldYield()) {
        CloseVideo();
        portENTER_CRITICAL(&Lock);
        if (Worker.VideoToken==Token && Worker.Generation==Generation) {
            Worker.VideoReady=true; Worker.VideoValid=false; Worker.WantVideo=false;
        }
        portEXIT_CRITICAL(&Lock);
        return;
    }
    if (Busy) return;
    bool Failed = PCBackupMode_GetState() == kPCBackupModeState_On;
    if (Worker.VideoCard && SDCard_PreviewShouldYield()) Failed=true;
    if (Pending && !Failed) {
        if (!Worker.Pixels) Worker.Pixels=AllocatePixels();
        esp_err_t Result=Worker.Pixels ? SDCard_MountPreview(&Worker.VideoCard) : ESP_ERR_NO_MEM;
        if (Result==ESP_ERR_INVALID_STATE) {
            portENTER_CRITICAL(&Lock);
            if (Worker.VideoToken==Token && Worker.Generation==Generation && Worker.WantVideo)
                Worker.VideoPending=true;
            portEXIT_CRITICAL(&Lock);
            return;
        }
        Failed=Result!=ESP_OK;
        if (!Failed) {
            FILE *File=fopen(Path,"rb");
            Failed=!VideoPreview_Open(&Worker.Video,File);
            if (Failed && File) fclose(File);
            TraceMedia("video_open",!Failed,Failed?ESP_FAIL:ESP_OK,Path);
            Worker.StreamToken=Token; Worker.StreamGeneration=Generation; Worker.NextFrame=0;
        }
    }
    const uint64_t Now=(uint64_t)esp_timer_get_time()/1000;
    bool Frame=false;
    if (!Failed && Worker.Video.File && Now>=Worker.NextFrame) {
        Frame=VideoPreview_Read(&Worker.Video,Worker.Pixels);
        if (!Worker.NextFrame) TraceMedia("video_frame",Frame,Frame?ESP_OK:ESP_FAIL,"");
        Failed=!Frame;
        /* Never replay a backlog after slow I/O; controls retain priority. */
        Worker.NextFrame=Now+(1000u+Worker.Video.FPS-1)/Worker.Video.FPS;
    }
    if (Failed) CloseVideo();
    portENTER_CRITICAL(&Lock);
    if (Worker.VideoToken==Token && Worker.Generation==Generation && Worker.WantVideo && (Frame || Failed)) {
        Worker.VideoReady=true; Worker.VideoValid=Frame && !Failed;
        if (Failed) Worker.WantVideo=false;
    }
    portEXIT_CRITICAL(&Lock);
}

static bool Current(uint32_t Generation)
{
    portENTER_CRITICAL(&Lock);
    const bool Match = Worker.Generation == Generation;
    portEXIT_CRITICAL(&Lock);
    return Match;
}

static void RunJob(const Job_t *Job)
{
    const RomBrowserDataSource_t *S = RomBrowser_GetDataSource();
    RomBrowserJobResult_t Result = {.Action = Job->Action, .Error = ESP_OK};
    if (!Current(Job->Generation)) return;
    if (Job->Action != kRomActionClose && PCBackupMode_GetState() == kPCBackupModeState_On)
        Result.Error = ESP_ERR_INVALID_STATE;
    else switch (Job->Action)
    {
        case kRomActionOpen:
            Result.Error = S->Open(&Result.Count);
            Result.Selected = S->Restore();
            break;
        case kRomActionEnter:
            Result.Error = S->Enter(Job->Index, &Result.Count);
            Result.Selected = S->Restore();
            break;
        case kRomActionParent:
            Result.Error = S->Parent(&Result.Count, &Result.Selected);
            break;
        case kRomActionStart:
            /* The prior artwork read and unmount have completed on this task. */
            free(Worker.Pixels); Worker.Pixels = NULL;
            Result.Error = S->Start(Job->Name);
            break;
        case kRomActionClose:
            S->Close();
            free(Worker.Pixels); Worker.Pixels = NULL;
            break;
    }
    portENTER_CRITICAL(&Lock);
    if (Worker.Generation == Job->Generation)
    {
        Worker.Result = Result;
        Worker.Done = Job->Action != kRomActionClose;
    }
    portEXIT_CRITICAL(&Lock);
}

static void ProcessOne(void)
{
    Job_t Job;
    char Path[320];
    uint32_t Generation, Token;
    portENTER_CRITICAL(&Lock);
    const bool HaveJob = Worker.Pending && !Worker.Copying;
    const bool HaveArt = !Worker.Pending && Worker.ArtPending && !Worker.Copying;
    Job = Worker.Job;
    Generation = Worker.Generation; Token = Worker.ArtToken;
    if (HaveJob) Worker.Pending = false;
    if (HaveArt) { strcpy(Path, Worker.ArtPath); Worker.ArtPending = false; }
    portEXIT_CRITICAL(&Lock);
    if (HaveJob)
    {
        PwrMgr_InhibitSleep();
        CloseVideo();
        RunJob(&Job);
        PwrMgr_AllowSleep();
        return;
    }
    if (!HaveArt) {
        PwrMgr_InhibitSleep(); PumpVideo(); PwrMgr_AllowSleep();
        return;
    }

    PwrMgr_InhibitSleep();
    CloseVideo();
    bool Valid = false;
    esp_err_t Result = ESP_FAIL;
    if (PCBackupMode_GetState() != kPCBackupModeState_On && Current(Generation))
    {
        if (!Worker.Pixels) Worker.Pixels = AllocatePixels();
        if (Worker.Pixels)
        {
            sdmmc_card_t *Card = NULL;
            Result = SDCard_MountPreview(&Card);
            if (Result == ESP_OK)
            {
                FILE *File = fopen(Path, "rb");
                if (File) { Valid = PanelArt_Read(File, Worker.Pixels); if (fclose(File)) Valid = false; }
                if (SDCard_Unmount(Card) != ESP_OK) Valid = false;
            }
        }
    }
    TraceMedia("cover",Valid,Result,Path);
    portENTER_CRITICAL(&Lock);
    if (Worker.Generation == Generation && Worker.ArtToken == Token)
    {
        /* A busy foreground SD owner wins; retry without blocking the UI. */
        Worker.ArtPending = Result == ESP_ERR_INVALID_STATE;
        Worker.ArtValid = Valid;
        Worker.ArtReady = !Worker.ArtPending;
    }
    portEXIT_CRITICAL(&Lock);
    PwrMgr_AllowSleep();
}

static void Task(void *Arg)
{
    (void)Arg;
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30));
        ProcessOne();
    }
}

esp_err_t RomBrowserWorker_Init(void)
{
    if (xTaskCreate(Task, "rom_browser", 6144, NULL, 1, &Worker.Task) != pdPASS)
        return ESP_ERR_NO_MEM;
    static const RomBrowserAsync_t Backend = {
        .Begin = Begin, .Poll = Poll, .Cancel = Cancel,
        .RequestArt = RequestArt, .CopyArt = CopyArt,
        .RequestVideo = RequestVideo, .CopyVideo = CopyVideo,
    };
    RomBrowserUI_RegisterAsync(&Backend);
    return ESP_OK;
}
