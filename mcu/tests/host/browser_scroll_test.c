#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "rom_browser_scroll.h"
#include "rom_title.h"

int main(void) {
    RomBrowserScroll_t s = {0};
    RomBrowserScrollStep_t step = RomBrowserScroll_Update(&s, 0, kScrollDown, kScrollDown);
    assert(step.Steps == 1 && step.Wrap);
    for (unsigned t = 10; t < 350; t += 10) assert(!RomBrowserScroll_Update(&s,t,2,0).Steps);
    assert(RomBrowserScroll_Update(&s,350,2,0).Steps == 1 && s.Rate == 5);
    unsigned count = 0;
    for (unsigned t = 360; t <= 5000; t += 10) {
        step = RomBrowserScroll_Update(&s,t,2,0); count += step.Steps;
        assert(!step.Wrap && step.Steps >= 0 && step.Steps <= 1);
        if (t == 1190) assert(s.Rate == 5);
        if (t == 1200 || t == 2490) assert(s.Rate == 12);
        if (t == 2500 || t == 3990) assert(s.Rate == 30);
        if (t == 4000) assert(s.Rate == 60);
    }
    assert(count == 124); /* 4.25 + 15.6 + 45 + 60 repeats, rounded down. */
    assert(!RomBrowserScroll_Update(&s,5010,0,0).Steps && s.Rate == 0);
    assert(!RomBrowserScroll_Update(&s,9000,0,0).Steps);
    assert(RomBrowserScroll_Update(&s,9010,1,1).Steps == -1 && s.Rate == 0);
    assert(!RomBrowserScroll_Update(&s,9020,3,2).Steps && s.Rate == 0);
    assert(RomBrowserScroll_Update(&s,9030,2,0).Steps == 1 && s.Rate == 0);
    RomBrowserScroll_Reset(&s,2);
    assert(!RomBrowserScroll_Update(&s,10000,2,2).Steps);
    assert(!RomBrowserScroll_Update(&s,10010,0,0).Steps);
    assert(RomBrowserScroll_Update(&s,10020,0,2).Steps == 1); /* tap released between frames */
    assert(!RomBrowserScroll_Update(&s,10030,0,0).Steps);
    assert(RomBrowserScroll_Update(&s,10040,2,2).Steps == 1);
    assert(RomBrowserScroll_Update(&s,60000,2,0).Steps <= 4); /* long task stall */
    assert(!RomBrowserScroll_Update(&s,100,2,0).Steps); /* clock rollback */
    for (int dir = -1; dir <= 1; dir += 2) {
        s = (RomBrowserScroll_t){0}; unsigned mask=dir<0?1:2;
        assert(RomBrowserScroll_Update(&s,0,mask,mask).Steps == dir);
        int total=0;
        for (unsigned t=30;t<6000;t+=30) total+=RomBrowserScroll_Update(&s,t,mask,0).Steps;
        assert(total * dir >= 180 && total * dir <= 190);
    }
    char title[256], letter[2];
    RomTitle_Make("  _Pokemon_Crystal.gbc",false,title,sizeof(title));
    assert(!strcmp(title,"POKEMON CRYSTAL"));
    assert(!strcmp(RomTitle_Group(title,false,letter),"P"));
    RomTitle_Make("Japan.v1",true,title,sizeof(title)); assert(!strcmp(title,"JAPAN.V1"));
    assert(!strcmp(RomTitle_Group(title,true,letter),"DIR"));
    assert(!strcmp(RomTitle_Group("1999",false,letter),"0-9"));
    RomTitle_Make("\xc3\xa9xample.gb",false,title,sizeof(title)); assert(!strcmp(title,"?XAMPLE"));
    assert(!strcmp(RomTitle_Group(title,false,letter),"#"));
    assert(RomTitle_Compare(" _Alpha.gb","Beta.gb",false)<0);
    assert(RomTitle_Compare("alpha.gb","alpha.gbc",false)<0);
    puts("PASS: acceleration tiers, tap/release/reversal, cancellation, stalled frames and title groups");
}
