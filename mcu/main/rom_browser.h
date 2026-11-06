#pragma once

#include "esp_err.h"
#include "rom_browser_ui.h"

esp_err_t RomBrowser_Init(void);
esp_err_t RomBrowser_CloseMenu(void);
const RomBrowserDataSource_t *RomBrowser_GetDataSource(void);
