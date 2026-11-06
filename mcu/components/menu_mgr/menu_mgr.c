#include "menu_mgr.h"

#include "osd_shared.h"
#include "lvgl.h"
#include "esp_log.h"
#include "button.h"
#include "osd.h"
#include "battery.h"
#include "rom_browser_ui.h"

#include <stddef.h>
#include <stdint.h>

LV_IMG_DECLARE(menu_status);
LV_IMG_DECLARE(menu_backups_accent);

static const uint8_t BackupGlyphData[] = {
    0x7c, 0x82, 0xb9, 0x81, 0xbd, 0xa5, 0xbd, 0x81, 0xbd, 0x7e,
};
static const lv_img_dsc_t BackupGlyph = {
    .header.cf = LV_IMG_CF_ALPHA_1BIT,
    .header.w = 8,
    .header.h = 10,
    .data_size = sizeof(BackupGlyphData),
    .data = BackupGlyphData,
};

static const uint8_t BackupTitleData[] = {
    0xfe, 0x0f, 0x07, 0xe3, 0x9c, 0xe7, 0x3f, 0x87, 0xf0,
    0xff, 0x1f, 0x8f, 0xf3, 0xb8, 0xe7, 0x3f, 0xcf, 0xe0,
    0xe7, 0x3b, 0xce, 0x63, 0xf0, 0xe7, 0x39, 0xce, 0x00,
    0xfe, 0x39, 0xce, 0x03, 0xe0, 0xe7, 0x3f, 0x8f, 0xe0,
    0xff, 0x3f, 0xce, 0x03, 0xf0, 0xe7, 0x3f, 0x07, 0xf0,
    0xe7, 0x3f, 0xce, 0x73, 0xf8, 0xe7, 0x38, 0x00, 0x70,
    0xfe, 0x39, 0xcf, 0xe3, 0xbc, 0xfe, 0x38, 0x0f, 0xe0,
    0xfc, 0x31, 0x87, 0xc3, 0x18, 0x7c, 0x30, 0x0f, 0xc0,
};
static const lv_img_dsc_t BackupTitle = {
    .header.cf = LV_IMG_CF_ALPHA_1BIT,
    .header.w = 70,
    .header.h = 8,
    .data_size = sizeof(BackupTitleData),
    .data = BackupTitleData,
};

typedef struct MenuMgrCtx
{
    TabID_t eCurTab;
    MenuTab_t* pMenus[kNumTabIDs];
    lv_obj_t *pBackupTab;
    lv_obj_t *pBackupFrameAccent;
    lv_obj_t *pInactiveSystem;
    lv_obj_t *pBackupTitle;
} MenuMgrCtx_t;

const lv_point_t _MenuOrigin_px = {
    .x = 8,
    .y = 7,
};

static MenuMgrCtx_t _Ctx;
static const char* TAG = "MenuMgr";
static OSD_Result_t MenuMgr_OnButton(const Button_t Button, const ButtonState_t State, void *arg);
static OSD_Result_t MenuMgr_Draw(void* arg);
static OSD_Result_t MenuMgr_OnTransition(void *arg);
static void MenuMgr_NextTab(void);
static void MenuMgr_PrevTab(void);

static lv_obj_t *Box(lv_obj_t *pScreen, int X, int Y, int Width, int Height)
{
    lv_obj_t *pBox = lv_obj_create(pScreen);
    lv_obj_remove_style_all(pBox);
    lv_obj_set_pos(pBox, X, Y);
    lv_obj_set_size(pBox, Width, Height);
    lv_obj_clear_flag(pBox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(pBox, LV_SCROLLBAR_MODE_OFF);
    return pBox;
}

/* Preserve the stock header/corners; repeat only a straight frame row. */
static lv_obj_t *BackupFrame(lv_obj_t *Screen, const lv_img_dsc_t *Base, lv_color_t Accent)
{
    enum { Width = 143, Top = 80, Extra = 27, Bottom = 26 };
    static lv_img_dsc_t Slices[2][3];
    const lv_img_dsc_t *Images[2] = {Base, &menu_backups_accent};
    lv_obj_t *Frame = Box(Screen, 8, 7, Width, Top + Extra + Bottom);
    for (unsigned Layer = 0; Layer < 2; ++Layer)
    {
        const unsigned Stride = Layer ? (Width + 7) / 8 : Width * sizeof(lv_color_t);
        for (unsigned Part = 0; Part < 3; ++Part)
        {
            const unsigned SourceY = Part == 0 ? 0 : Part == 1 ? 90 : Top;
            const unsigned Height = Part == 0 ? Top : Part == 1 ? 2 : Bottom;
            lv_img_dsc_t *Slice = &Slices[Layer][Part];
            *Slice = *Images[Layer];
            Slice->header.h = Height;
            Slice->data_size = Height * Stride;
            Slice->data += SourceY * Stride;
            lv_obj_t *Image = lv_img_create(Frame);
            lv_img_set_src(Image, Slice);
            lv_obj_set_pos(Image, 0, Part == 0 ? 0 : Part == 1 ? Top : Top + Extra);
            /* LVGL 8 tile loops exclude the final origin; overlap the bottom by one row. */
            if (Part == 1) lv_obj_set_size(Image, Width, Extra + 1);
            if (Layer)
            {
                lv_obj_set_style_img_recolor(Image, Accent, 0);
                lv_obj_set_style_img_recolor_opa(Image, LV_OPA_COVER, 0);
            }
        }
    }
    return Frame;
}

static void DrawBackupTab(lv_obj_t *pScreen)
{
    if (_Ctx.pBackupTab != NULL) return;
    const bool Selected = _Ctx.eCurTab == kTabID_Backups;
    const lv_color_t Accent = Selected ? _Ctx.pMenus[_Ctx.eCurTab]->Accent
                                      : lv_color_black();
    const lv_color_t Color = lv_color_hex(Selected ? 0xffffff : 0x666666);
    _Ctx.pBackupTab = Box(pScreen, 108, 7, 17, 16);
    lv_obj_set_style_bg_color(_Ctx.pBackupTab, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_Ctx.pBackupTab, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_Ctx.pBackupTab, Selected ? Accent : Color, 0);
    lv_obj_set_style_border_width(_Ctx.pBackupTab, 1, 0);
    lv_obj_set_style_radius(_Ctx.pBackupTab, 3, 0);
    lv_obj_t *pGlyph = lv_img_create(_Ctx.pBackupTab);
    lv_img_set_src(pGlyph, &BackupGlyph);
    lv_obj_center(pGlyph);
    lv_obj_set_style_img_recolor(pGlyph, Color, 0);
    lv_obj_set_style_img_recolor_opa(pGlyph, LV_OPA_COVER, 0);
    if (Selected)
    {
        _Ctx.pInactiveSystem = Box(pScreen, 88, 7, 19, 16);
        lv_obj_set_style_bg_color(_Ctx.pInactiveSystem, lv_color_hex(0xff00ff), 0);
        lv_obj_set_style_bg_opa(_Ctx.pInactiveSystem, LV_OPA_COVER, 0);
        lv_obj_t *pStock = lv_img_create(_Ctx.pInactiveSystem);
        lv_img_set_src(pStock, &menu_status);
        lv_obj_set_pos(pStock, -80, 0);
        lv_obj_t *pBorder = Box(_Ctx.pInactiveSystem, 0, 15, 19, 1);
        lv_obj_set_style_bg_color(pBorder, Accent, 0);
        lv_obj_set_style_bg_opa(pBorder, LV_OPA_COVER, 0);
        _Ctx.pBackupTitle = Box(pScreen, 20, 29, 76, 8);
        lv_obj_set_style_bg_color(_Ctx.pBackupTitle, Accent, 0);
        lv_obj_set_style_bg_opa(_Ctx.pBackupTitle, LV_OPA_COVER, 0);
        lv_obj_t *pTitle = lv_img_create(_Ctx.pBackupTitle);
        lv_img_set_src(pTitle, &BackupTitle);
        lv_obj_set_style_img_recolor(pTitle, lv_color_black(), 0);
        lv_obj_set_style_img_recolor_opa(pTitle, LV_OPA_COVER, 0);
        lv_obj_set_pos(pTitle, 0, 0);
    }
}

OSD_Result_t MenuMgr_Initialize(OSD_Widget_t* const pWidget, lv_obj_t *const pScreen)
{
    if (pScreen == NULL)
    {
        return kOSD_Result_Err_NullDataPtr;
    }

    if (sys_dnode_is_linked(&pWidget->Node))
    {
        return kOSD_Result_Err_MenuAlreadyInit;
    }

    pWidget->fnDraw = MenuMgr_Draw;
    pWidget->fnOnButton = MenuMgr_OnButton;
    pWidget->fnOnTransition = MenuMgr_OnTransition;

    _Ctx.eCurTab = kTabID_Backups;

    return kOSD_Result_Ok;
}

OSD_Result_t MenuMgr_AddTab(TabID_t eID, MenuTab_t *const pTab)
{
    if ((unsigned)eID >= kNumTabIDs)
    {
        return kOSD_Result_Err_InvalidTabID;
    }

    if ((pTab == NULL) || (pTab->pImageDesc == NULL))
    {
        return kOSD_Result_Err_NullDataPtr;
    }

    if ((_Ctx.pMenus[eID] != NULL) || (pTab->pImgObj != NULL))
    {
        return kOSD_Result_Err_MenuTabAlreadyInit;
    }

    _Ctx.pMenus[eID] = pTab;

    return kOSD_Result_Ok;
}

static OSD_Result_t MenuMgr_Draw(void* arg)
{
    if (arg == NULL)
    {
        return kOSD_Result_Err_NullDataPtr;
    }

    lv_obj_t *const pScreen = (lv_obj_t*)arg;

    const TabID_t eID = _Ctx.eCurTab;

    if ((unsigned)eID >= kNumTabIDs)
    {
        return kOSD_Result_Err_InvalidTabID;
    }

    MenuTab_t *const pTab = _Ctx.pMenus[eID];
    if ((pTab == NULL) || (pTab->pImageDesc == NULL))
    {
        return kOSD_Result_Err_NullDataPtr;
    }

    Battery_SetCompact(eID == kTabID_Backups && OSD_IsVisible());
    if (eID == kTabID_Backups && !OSD_IsVisible())
    {
        if (pTab->pImgObj != NULL) MenuMgr_OnTransition(NULL);
        return kOSD_Result_Ok;
    }

    if (pTab->pImgObj == NULL)
    {
        if (eID == kTabID_Backups)
            pTab->pImgObj = BackupFrame(pScreen, pTab->pImageDesc, pTab->Accent);
        else
        {
            pTab->pImgObj = lv_img_create(pScreen);
            lv_obj_align(pTab->pImgObj , LV_ALIGN_TOP_LEFT, _MenuOrigin_px.x, _MenuOrigin_px.y);
        }
    }

    if (eID != kTabID_Backups) lv_img_set_src(pTab->pImgObj, pTab->pImageDesc);
    DrawBackupTab(pScreen);

    if (pTab->Widget.fnDraw != NULL)
    {
        Tab_DrawCtx_t Ctx = { pTab->Menu, pScreen, pTab->Accent };
        pTab->Widget.fnDraw(&Ctx);
    }

    return kOSD_Result_Ok;
}

static OSD_Result_t MenuMgr_OnButton(const Button_t Button, const ButtonState_t State, void *arg)
{
    (void)arg;

    const TabID_t eID = _Ctx.eCurTab;
    if (eID == kTabID_Backups && !OSD_IsVisible()) return kOSD_Result_Ok;

    if ((unsigned)eID >= kNumTabIDs)
    {
        return kOSD_Result_Err_InvalidTabID;
    }

    MenuTab_t *const pTab = _Ctx.pMenus[eID];
    if (pTab == NULL)
    {
        ESP_LOGE(TAG, "no tab in %s", __func__ );
        return kOSD_Result_Err_NullDataPtr;
    }

    switch (Button)
    {
        // Fall-through
        case kButton_B:
        case kButton_A:
        case kButton_Down:
        case kButton_Up:
            if (pTab->Widget.fnOnButton != NULL)
            {
                const OSD_Result_t eResult = pTab->Widget.fnOnButton(Button, State, pTab->Menu);

                if (eResult != kOSD_Result_Ok)
                {
                    ESP_LOGE(TAG, "%s OnButton call failed with %d", pTab->Widget.Name, eResult);
                }
            }
            break;
        case kButton_Left:
            if (eID == kTabID_Backups && RomBrowserUI_HandleHorizontal(Button, State)) break;
            if (State == kButtonState_Pressed)
            {
                MenuMgr_PrevTab();
            }
            break;
        case kButton_Right:
            if (eID == kTabID_Backups && RomBrowserUI_HandleHorizontal(Button, State)) break;
            if (State == kButtonState_Pressed)
            {
                MenuMgr_NextTab();
            }
            break;
        default:
            break;
    }

    return kOSD_Result_Ok;
}

static void MenuMgr_NextTab(void)
{
    TabID_t eNextID = _Ctx.eCurTab + 1;
    if (eNextID >= kNumTabIDs)
    {
        eNextID = kTabID_First;
    }

    // Clean up the old tab data
    MenuMgr_OnTransition(NULL);

    _Ctx.eCurTab = eNextID;
}

static void MenuMgr_PrevTab(void)
{
    TabID_t ePrevID = _Ctx.eCurTab - 1;
    if ((signed) ePrevID < 0)
    {
        ePrevID = kTabID_Last;
    }

    // Clean up the old tab data
    MenuMgr_OnTransition(NULL);

    _Ctx.eCurTab = ePrevID;
}

static OSD_Result_t MenuMgr_OnTransition(void *arg)
{
    (void) arg;
    OSD_Result_t eResult = kOSD_Result_Ok;

    MenuTab_t *const pTab = _Ctx.pMenus[_Ctx.eCurTab];

    if (pTab->Widget.fnOnTransition != NULL)
    {
        eResult = pTab->Widget.fnOnTransition(pTab->Menu);
        if (eResult != kOSD_Result_Ok)
        {
            ESP_LOGE(TAG, "%s OnTransition failed with %d", pTab->Widget.Name, eResult);
        }
    }

    if (pTab->pImgObj != NULL)
    {
        lv_obj_del(pTab->pImgObj);
        pTab->pImgObj = NULL;
    }

    lv_obj_t **Extra[] = {&_Ctx.pBackupTab, &_Ctx.pBackupFrameAccent,
                         &_Ctx.pInactiveSystem, &_Ctx.pBackupTitle};
    for (size_t Index = 0; Index < ARRAY_SIZE(Extra); ++Index)
    {
        if (*Extra[Index] != NULL) lv_obj_del(*Extra[Index]);
        *Extra[Index] = NULL;
    }

    return eResult;
}
