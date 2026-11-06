#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static bool FailAllocation;
static void *Allocate(size_t Size) { return FailAllocation ? NULL : malloc(Size); }
#define malloc Allocate
#include "../../main/rom_browser_worker.c"
#undef malloc

static RomBrowserAsync_t Backend;
static unsigned Opens, Closes, Starts, Mounts, Unmounts;
static bool Mounted, YieldPreview;
static int64_t ClockUs;
int64_t esp_timer_get_time(void) { return ClockUs; }
bool SDCard_PreviewShouldYield(void) { return YieldPreview; }
static unsigned SleepLocks;
void PwrMgr_InhibitSleep(void) { assert(SleepLocks++ == 0); }
void PwrMgr_AllowSleep(void) { assert(SleepLocks-- == 1); }
static char Filename[256] = "Pokemon.gbc", Started[256];
static PCBackupModeState_t Mode;
static esp_err_t MountError;
static void (*DuringMount)(void), (*DuringOpen)(void);
static esp_err_t Open(size_t *Count) { assert(SleepLocks == 1); ++Opens; *Count=3; if(DuringOpen) DuringOpen(); return ESP_OK; }
static const char *Name(size_t Index) { return Index<3?Filename:NULL; }
static void Close(void) { assert(!Mounted); ++Closes; }
static esp_err_t Start(const char *Name) { assert(!Mounted && Mounts==Unmounts); ++Starts; strcpy(Started,Name); return ESP_OK; }
static esp_err_t Enter(size_t Index,size_t *Count) { assert(Index<3); *Count=2; return ESP_OK; }
static esp_err_t Parent(size_t *Count,size_t *Selected) { *Count=3; *Selected=1; return ESP_OK; }
static size_t Restore(void) { return 2; }
const RomBrowserDataSource_t *RomBrowser_GetDataSource(void) {
    static const RomBrowserDataSource_t S={.Open=Open,.NameAt=Name,.Close=Close,.Start=Start,
        .Enter=Enter,.Parent=Parent,.Restore=Restore}; return &S;
}
void RomBrowserUI_RegisterAsync(const RomBrowserAsync_t *Value) { Backend=*Value; }
PCBackupModeState_t PCBackupMode_GetState(void) { return Mode; }
int xTaskCreate(void (*Task)(void *),const char *Name,unsigned Stack,void *Arg,unsigned Priority,TaskHandle_t *Handle) {
    *Handle=(void *)1; return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t Handle) { assert(Handle); }
uint32_t ulTaskNotifyTake(int Clear,TickType_t Wait) { return 0; }
esp_err_t SDCard_MountPreview(sdmmc_card_t **Card) {
    assert(SleepLocks == 1);
    assert(!Mounted);
    if(MountError) return MountError;
    Mounted=true; ++Mounts; *Card=(sdmmc_card_t *)&Mounted;
    if(DuringMount) DuringMount();
    return ESP_OK;
}
esp_err_t SDCard_Unmount(sdmmc_card_t *Card) { assert(Mounted); Mounted=false; ++Unmounts; return ESP_OK; }
static void CancelDuringIO(void) { Backend.Cancel(); }
static void LaunchDuringIO(void) { assert(Backend.Begin(kRomActionStart,1)==ESP_OK); }
static void RequestNewToken(void) { Backend.RequestArt(NULL,99); }
int main(void) {
    assert(RomBrowserWorker_Init()==ESP_OK);
    RomBrowserJobResult_t result;
    assert(Backend.Begin(kRomActionOpen,0)==ESP_OK && !Backend.Poll(&result));
    ProcessOne(); assert(Backend.Poll(&result) && result.Count==3 && result.Selected==2 && Opens==1);
    assert(!Backend.Poll(&result));
    assert(Backend.Begin(kRomActionEnter,1)==ESP_OK); ProcessOne();
    assert(Backend.Poll(&result) && result.Count==2);
    assert(Backend.Begin(kRomActionParent,0)==ESP_OK); ProcessOne();
    assert(Backend.Poll(&result) && result.Selected==1);
    assert(Backend.Begin(kRomActionStart,1)==ESP_OK);
    strcpy(Filename,"Changed.gbc"); ProcessOne();
    assert(Backend.Poll(&result) && Starts==1 && !strcmp(Started,"Pokemon.gbc"));
    assert(Backend.Begin(kRomActionStart,9)==ESP_ERR_INVALID_ARG);
    Backend.Begin(kRomActionStart,1); Backend.Cancel(); ProcessOne();
    assert(Starts==1 && !Backend.Poll(&result));
    DuringOpen=CancelDuringIO;
    Backend.Begin(kRomActionOpen,0); ProcessOne();
    assert(!Backend.Poll(&result)); ProcessOne(); DuringOpen=NULL;
    Mode=kPCBackupModeState_On;
    Backend.Begin(kRomActionStart,1); ProcessOne();
    assert(Backend.Poll(&result) && result.Error==ESP_ERR_INVALID_STATE && Starts==1);
    Mode=kPCBackupModeState_Off;

    char path[]="/tmp/chromagic-art-XXXXXX"; int fd=mkstemp(path); assert(fd>=0);
    FILE *file=fdopen(fd,"wb"); assert(file);
    const unsigned char header[]={'C','M','A','R',1,1,72,0,80,0,0,0,0,45,0,0};
    fwrite(header,1,16,file);
    for(unsigned i=0;i<BOX_ART_BYTES;i+=2) { fputc(0,file); fputc(0xf8,file); }
    assert(!fclose(file));
    _Alignas(4) uint8_t pixels[PREVIEW_BYTES]; bool valid=false;
    Backend.RequestArt(path,1); ProcessOne();
    assert(!Backend.CopyArt(2,pixels,&valid));
    assert(Backend.CopyArt(1,pixels,&valid) && valid && pixels[(6*PREVIEW_WIDTH+28)*2]==0 && pixels[(6*PREVIEW_WIDTH+28)*2+1]==0xf8);
    assert(!Backend.CopyArt(1,pixels,&valid));
    Backend.RequestArt("/nonexistent/box-art.cma",2); ProcessOne();
    assert(Backend.CopyArt(2,pixels,&valid) && !valid);
    MountError=ESP_ERR_INVALID_STATE;
    Backend.RequestArt(path,3); ProcessOne(); assert(!Backend.CopyArt(3,pixels,&valid));
    MountError=ESP_OK; ProcessOne(); assert(Backend.CopyArt(3,pixels,&valid) && valid);
    DuringMount=RequestNewToken;
    Backend.RequestArt(path,4); ProcessOne();
    assert(!Backend.CopyArt(4,pixels,&valid) && !Backend.CopyArt(99,pixels,&valid));
    DuringMount=CancelDuringIO;
    Backend.RequestArt(path,5); ProcessOne();
    assert(!Backend.CopyArt(5,pixels,&valid)); ProcessOne(); assert(!Worker.Pixels);
    DuringMount=LaunchDuringIO;
    Backend.RequestArt(path,6); ProcessOne();
    assert(!Mounted && Starts==1 && !Backend.CopyArt(6,pixels,&valid));
    ProcessOne(); assert(Starts==2 && Backend.Poll(&result) && !Worker.Pixels);
    DuringMount=NULL; FailAllocation=true;
    Backend.RequestArt(path,7); ProcessOne(); assert(Backend.CopyArt(7,pixels,&valid) && !valid);
    FailAllocation=false;
    Backend.RequestArt(path,8); Backend.Cancel(); ProcessOne(); assert(!Worker.Pixels);
    assert(Mounts==Unmounts && !Mounted);
    assert(SleepLocks == 0);
    /* Open once, advance frames without remounting, then yield before launch. */
    file=fopen(path,"wb"); assert(file);
    const uint8_t vh[24]={'C','M','V','P',1,1,128,0,92,0,12,0,2,0,0,0,0,92,0,0,0,0,0,0};
    fwrite(vh,1,24,file);
    for(unsigned frame=0;frame<2;++frame) for(unsigned i=0;i<PREVIEW_BYTES;++i) fputc(frame?0x11:0x22,file);
    fclose(file);
    const unsigned BeforeMounts=Mounts;
    Backend.RequestVideo(path,10); ProcessOne();
    assert(Mounted && Backend.CopyVideo(10,pixels,&valid) && valid && pixels[0]==0x22);
    ClockUs=90000; ProcessOne();
    assert(Backend.CopyVideo(10,pixels,&valid) && valid && pixels[0]==0x11 && Mounts==BeforeMounts+1);
    ClockUs=180000; ProcessOne();
    assert(Backend.CopyVideo(10,pixels,&valid) && valid && pixels[0]==0x22);
    YieldPreview=true; ProcessOne();
    assert(!Mounted && Backend.CopyVideo(10,pixels,&valid) && !valid);
    YieldPreview=false;
    Backend.RequestVideo(path,11); ProcessOne(); assert(Mounted);
    Backend.Begin(kRomActionStart,1); ProcessOne(); assert(!Mounted && !Worker.Pixels && Starts==3);
    Backend.RequestVideo(path,12); ProcessOne(); assert(Mounted);
    Backend.RequestVideo(NULL,13); ProcessOne(); assert(!Mounted && !Backend.CopyVideo(12,pixels,&valid));
    Backend.RequestVideo("/nonexistent/clip.cmv",14); ProcessOne();
    assert(Backend.CopyVideo(14,pixels,&valid) && !valid && !Mounted);
    Backend.Cancel(); ProcessOne(); assert(!Worker.Pixels && Mounts==Unmounts && SleepLocks==0);
    assert(unlink(path)==0);
    puts("PASS: asynchronous jobs, owned launch identity, cancellation, stale art, SD contention, drain before launch and allocation failure");
}
