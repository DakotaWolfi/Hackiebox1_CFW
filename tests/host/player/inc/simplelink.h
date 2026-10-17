#pragma once
#include <stdint.h>
#define FS_MODE_OPEN_READ 0
long sl_FsOpen(unsigned char *n, unsigned long mode, unsigned long *tok, long *fh);
long sl_FsRead(long fh, unsigned long off, unsigned char *buf, unsigned long len);
short sl_FsClose(long fh, unsigned char *a, unsigned char *b, unsigned long c);
