#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include "../../main/virtual_cart.c"

static bool Mounted, Bulk, Started;
static unsigned Completed, Errors, Closed;
static esp_err_t UnmountResult;
static sdmmc_card_t Card;
static unsigned SleepLocks;
void PwrMgr_InhibitSleep(void) { ++SleepLocks; }
void PwrMgr_AllowSleep(void) { assert(SleepLocks); --SleepLocks; }
TickType_t xTaskGetTickCount(void) { return 0; }
void vTaskDelay(TickType_t Ticks) { (void)Ticks; }
esp_err_t SDCard_Mount(sdmmc_card_t **Out) { assert(!Mounted); Mounted=true; *Out=&Card; return ESP_OK; }
esp_err_t SDCard_Unmount(sdmmc_card_t *Value) { assert(Mounted && Value==&Card); Mounted=false; return UnmountResult; }
esp_err_t CartBulk_Begin(void) { assert(Mounted && !Bulk); Bulk=true; return ESP_OK; }
void CartBulk_End(void) { assert(Bulk); Bulk=false; }
esp_err_t CartBulk_WritePSRAMBlock(uint32_t Address, const uint8_t *Data) { (void)Address; (void)Data; assert(Bulk); return ESP_OK; }
esp_err_t CartBulk_ReadVirtualBlock(uint16_t Sequence, uint8_t *Data) { (void)Sequence; (void)Data; assert(false); return ESP_FAIL; }
esp_err_t CartLink_VirtualCommand(uint8_t Command, uint16_t Config, uint8_t High, CartLinkResponse_t *Out) {
    (void)Config; (void)High;
    if(Command==kVirtualCommandPrepare || Command==kVirtualCommandStop) Started=false;
    if(Command==kVirtualCommandStart) Started=true;
    memset(Out,0,sizeof(*Out)); Out->Count=4;
    Out->Data[0]=Started ? 0xff : kVirtualStatusDisableSeen|kVirtualStatusResetAsserted;
    Out->Data[1]=Started ? kVirtualStatusPostExitFrame>>8 : 0;
    return ESP_OK;
}
void RomBrowserUI_SetProgress(uint32_t Value, uint32_t Total) { assert(Value<=Total); }
bool RomBrowserUI_SetComplete(void) { assert(!Mounted && !Bulk); ++Completed; return true; }
void RomBrowserUI_SetError(esp_err_t Error) { assert(Error!=ESP_OK && !Mounted && !Bulk); ++Errors; }
esp_err_t RomBrowser_CloseMenu(void) { assert(Completed && !Mounted && !Bulk); ++Closed; return ESP_OK; }

int main(void) {
    char Path[]="/tmp/chromagic-load-XXXXXX"; int FD=mkstemp(Path); assert(FD>=0);
    uint8_t ROM[32768]={0}; memcpy(ROM+0x104,NintendoLogo,sizeof(NintendoLogo));
    memcpy(ROM+0x134,"LOAD TEST",9);
    for(unsigned I=0x134;I<=0x14c;++I) ROM[0x14d]=(uint8_t)(ROM[0x14d]-ROM[I]-1);
    uint16_t Sum=0; for(unsigned I=0;I<sizeof(ROM);++I) Sum+=ROM[I]; ROM[0x14e]=Sum>>8; ROM[0x14f]=Sum;
    FILE *File=fdopen(FD,"wb"); assert(File); assert(fwrite(ROM,1,sizeof(ROM),File)==sizeof(ROM)); assert(!fclose(File));
    assert(RunLoad(Path)==ESP_OK);
    assert(Completed==1 && Closed==1 && Errors==0 && Active);
    SetActive(false); ClearActiveSave(); Completed=Closed=Errors=0;
    UnmountResult=ESP_FAIL;
    assert(RunLoad(Path)==ESP_FAIL);
    assert(Completed==0 && Closed==0 && Errors==1 && !Active);
    assert(!Mounted && !Bulk && !Started);
    assert(!SleepLocks);
    assert(!unlink(Path));
    puts("PASS: load success follows SD cleanup; cleanup errors retain the menu and stop the failed launch");
}
