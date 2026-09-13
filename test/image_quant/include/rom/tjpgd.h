#pragma once
#include <stdint.h>
typedef unsigned int UINT;
typedef uint8_t BYTE;
typedef struct { void *device; unsigned width, height; } JDEC;
typedef struct { unsigned left, right, top, bottom; } JRECT;
typedef enum { JDR_OK, JDR_FMT1 } JRESULT;
static inline JRESULT jd_prepare(JDEC *jd, UINT (*input)(JDEC *, BYTE *, UINT),
                                 void *pool, UINT size, void *device)
{
    (void)jd; (void)input; (void)pool; (void)size; (void)device;
    return JDR_FMT1;
}
static inline JRESULT jd_decomp(JDEC *jd, UINT (*output)(JDEC *, void *, JRECT *), BYTE scale)
{ (void)jd; (void)output; (void)scale; return JDR_FMT1; }
