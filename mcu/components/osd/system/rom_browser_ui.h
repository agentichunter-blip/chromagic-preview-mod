#pragma once

#include "esp_err.h"
#include "osd_shared.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { kRomPlatformUnknown, kRomPlatformGB, kRomPlatformGBC } RomPlatform_t;

typedef struct RomBrowserDataSource {
    esp_err_t (*Open)(size_t *pCount);
    const char *(*NameAt)(size_t Index);
    void (*Close)(void);
    esp_err_t (*Start)(const char *pName);
    bool (*IsDirectory)(size_t Index);
    esp_err_t (*Enter)(size_t Index, size_t *pCount);
    esp_err_t (*Parent)(size_t *pCount, size_t *pSelected);
    bool (*AtRoot)(void);
    void (*Remember)(size_t Index);
    size_t (*Restore)(void);
    bool (*VideoPath)(size_t Index, char *Path, size_t Size);
    bool (*ArtPath)(size_t Index, char *Path, size_t Size);
    RomPlatform_t (*FolderPlatform)(size_t Index);
} RomBrowserDataSource_t;

typedef enum {
    kRomActionOpen, kRomActionEnter, kRomActionParent, kRomActionStart, kRomActionClose
} RomBrowserAction_t;
typedef struct {
    RomBrowserAction_t Action;
    esp_err_t Error;
    size_t Count, Selected;
} RomBrowserJobResult_t;
/* Optional asynchronous backend. All LVGL operations stay on the UI task. */
typedef struct {
    esp_err_t (*Begin)(RomBrowserAction_t Action, size_t Index);
    bool (*Poll)(RomBrowserJobResult_t *Result);
    void (*Cancel)(void);
    void (*RequestArt)(const char *Path, uint32_t Token);
    bool (*CopyArt)(uint32_t Token, uint8_t *Pixels, bool *Valid);
    void (*RequestVideo)(const char *Path, uint32_t Token);
    bool (*CopyVideo)(uint32_t Token, uint8_t *Pixels, bool *Valid);
} RomBrowserAsync_t;
void RomBrowserUI_RegisterAsync(const RomBrowserAsync_t *Backend);
/* Horizontal input belongs to the system picker while it has focus. */
bool RomBrowserUI_HandleHorizontal(Button_t Button, ButtonState_t State);

OSD_Result_t RomBrowserUI_Draw(void *pArg);
OSD_Result_t RomBrowserUI_OnButton(Button_t Button, ButtonState_t State,
                                   void *pArg);
OSD_Result_t RomBrowserUI_OnTransition(void *pArg);

void RomBrowserUI_RegisterDataSource(const RomBrowserDataSource_t *pSource);
void RomBrowserUI_SetProgress(uint32_t Completed, uint32_t Total);
bool RomBrowserUI_SetComplete(void);
void RomBrowserUI_SetError(esp_err_t Error);
