/* Deterministic synthetic fixture; no photos, external fonts or device state. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lodepng.h"

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    const unsigned w = 1280, h = 960;
    unsigned char *rgba = malloc((size_t)w*h*4);
    if (!rgba) return 3;
    for (unsigned y=0; y<h; ++y) for (unsigned x=0; x<w; ++x) {
        unsigned char *p = rgba+((size_t)y*w+x)*4;
        if (y<h/2 && x<w/2) p[0]=p[1]=p[2]=(unsigned char)(x*255/(w/2-1));
        else if (y<h/2) { p[0]=160+(x-w/2)*75/(w/2);p[1]=p[0]-8;p[2]=p[0]-22; }
        else if (x<w/2) { p[0]=200+x*40/(w/2);p[1]=25+x*150/(w/2);p[2]=25; }
        else { p[0]=30+(x-w/2)*80/(w/2);p[1]=40+(y-h/2)*130/(h/2);p[2]=180; }
        if (x<4 || y<4 || x>=w-4 || y>=h-4) p[0]=p[1]=p[2]=0;
        if (y>h-60 && x>=20 && x<w-20) p[0]=p[1]=p[2]=(x/12)%2 ? 255 : 0;
        p[3]=255;
    }
    unsigned err=lodepng_encode32_file(argv[1],rgba,w,h);
    free(rgba);
    if (err) return 4;
    unsigned char *qr=NULL; unsigned qw=0,qh=0;
    if (lodepng_decode32_file(&qr,&qw,&qh,argv[2]) || qw>400 || qh>300) return 5;
    rgba=malloc(400*300*4);if(!rgba){free(qr);return 6;}memset(rgba,255,400*300*4);
    for (unsigned y=0;y<qh;++y) memcpy(rgba+(((300-qh)/2+y)*400+(400-qw)/2)*4,qr+y*qw*4,qw*4);
    err=lodepng_encode32_file(argv[3],rgba,400,300);
    free(qr);free(rgba);
    return err ? 7 : 0;
}
