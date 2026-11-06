#include <assert.h>
#include <stdio.h>
#include "gfx.h"
#include "osd.h"

static lv_color_t Pixels[160*144];
static unsigned FrameStarts, InputPolls, Draws;
void OSD_HandleInputs(void) { ++InputPolls; }
void OSD_Draw(void *Arg) { (void)Arg; ++Draws; }
static void Flush(lv_disp_drv_t *Driver, const lv_area_t *Area, lv_color_t *Data)
{
    (void)Data;
    /* The FPGA's qMenuInit gate requires two writes starting at address zero. */
    assert(Area->x1==0 && Area->y1==0 && Area->x2==159 && Area->y2==143);
    ++FrameStarts;
    lv_disp_flush_ready(Driver);
}
int main(void)
{
    lv_init();
    static lv_disp_draw_buf_t Buffer;
    lv_disp_draw_buf_init(&Buffer,Pixels,NULL,160*144);
    static lv_disp_drv_t Driver;
    lv_disp_drv_init(&Driver);
    Driver.hor_res=160; Driver.ver_res=144; Driver.direct_mode=1; Driver.full_refresh=1;
    Driver.draw_buf=&Buffer; Driver.flush_cb=Flush;
    lv_disp_drv_register(&Driver);
    Gfx_Start(lv_scr_act());
    lv_refr_now(NULL); /* drain the initial dirty frame without later widget changes */
    /* No battery update, menu input, timer tick, or library read should be needed. */
    printf("Startup frame writes: %u\n",FrameStarts); fflush(stdout);
    assert(FrameStarts>=2 && !InputPolls && !Draws && lv_tick_get()==0);
    puts("PASS: menu frame gate primed before startup timers or user input");
}
