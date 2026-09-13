#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "fb_render.h"

static bool has_red;
bool epd_has_red(void) { return has_red; }

static fb_t frame(unsigned mode, uint8_t data[5][16])
{
    memset(data,0,5*16);memset(data[0],255,16);memset(data[1],0xA5,16);
    return (fb_t){.width=32,.height=4,.plane_bytes=16,.row_bytes=4,
        .black=data[0],.red=mode==2?NULL:data[1],.yellow=mode>=3?data[2]:NULL,
        .blue=mode==4?data[3]:NULL,.green=mode==4?data[4]:NULL};
}

int main(void)
{
    unsigned cases=0;
    const int xs[]={-5,-1,0,7,15,31,36},ys[]={-1,0,3,5},widths[]={1,2,9,30,44};
    for(unsigned mode=0;mode<5;++mode)for(int color=0;color<=6;++color)
    for(size_t ix=0;ix<sizeof(xs)/sizeof(xs[0]);++ix)
    for(size_t iy=0;iy<sizeof(ys)/sizeof(ys[0]);++iy)
    for(size_t iw=0;iw<sizeof(widths)/sizeof(widths[0]);++iw)for(int rows=1;rows<=3;rows+=2) {
        uint8_t expected[5][16],actual[5][16];has_red=mode!=0;
        fb_t a=frame(mode,expected),b=frame(mode,actual);
        int x=xs[ix],y=ys[iy],w=widths[iw];
        for(int dy=0;dy<rows;++dy)for(int dx=0;dx<w;++dx)fb_pixel(&a,x+dx,y+dy,color);
        if(rows==1)fb_hline(&b,x,y,w,color);else fb_fill_rect(&b,x,y,w,rows,color);
        assert(memcmp(expected,actual,sizeof(expected))==0);cases++;
    }
    printf("PASS: %u real framebuffer color/clip/span/rectangle cases\n",cases);
    return 0;
}
