/* CELT-only probe: SILK and hybrid packets are refused before decoding (TB-AUD-013). */
#include "opus_types.h"
#include "entdec.h"
#include "silk/control.h"
opus_int silk_Get_Decoder_Size(opus_int *s) { *s = 0; return 0; }
opus_int silk_InitDecoder(void *d) { (void)d; return 0; }
opus_int silk_Decode(void *d, silk_DecControlStruct *c, opus_int l, opus_int n, ec_dec *r, opus_int16 *o, opus_int32 *len)
{ (void)d; (void)c; (void)l; (void)n; (void)r; (void)o; *len = 0; return -1; }
