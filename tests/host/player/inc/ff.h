#pragma once
#include <stdio.h>
#include <stdint.h>
typedef unsigned UINT;
typedef uint8_t BYTE;
typedef uint32_t DWORD;
typedef uint32_t FSIZE_t;
typedef enum
{
    FR_OK = 0,
    FR_DISK_ERR,
    FR_INT_ERR,
    FR_NOT_READY,
    FR_NO_FILE,
    FR_NO_PATH,
    FR_INVALID_NAME,
    FR_DENIED,
    FR_EXIST
} FRESULT;
typedef struct
{
    FILE *fp;
    FSIZE_t size;
} FIL;
typedef struct
{
    FSIZE_t fsize;
    uint16_t fdate, ftime;
    BYTE fattrib;
    char fname[256];
} FILINFO;
typedef uint16_t WORD;
typedef struct
{
    DWORD n_fatent;
    WORD csize;
} FATFS;
FRESULT f_getfree(const char *p, DWORD *n, FATFS **fs);
#define FA_READ 1
#define FA_WRITE 2
#define FA_CREATE_ALWAYS 8
#define FA_OPEN_EXISTING 0
#define AM_HID 2
#define AM_DIR 0x10
#define f_size(f) ((f)->size)
#define f_tell(f) ((FSIZE_t)ftell((f)->fp))
FRESULT f_open(FIL *f, const char *p, BYTE mode);
FRESULT f_read(FIL *f, void *b, UINT n, UINT *got);
FRESULT f_write(FIL *f, const void *b, UINT n, UINT *w);
FRESULT f_lseek(FIL *f, FSIZE_t o);
FRESULT f_close(FIL *f);
FRESULT f_stat(const char *p, FILINFO *fi);
FRESULT f_unlink(const char *p);
