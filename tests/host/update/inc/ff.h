#pragma once
/* FatFs stub on host files (tests/host/update). */
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
    void *d; /* host DIR */
} DIR;
typedef struct
{
    FSIZE_t fsize;
    BYTE fattrib;
    char fname[13]; /* 8.3 only (FF_USE_LFN 0) */
} FILINFO;
#define AM_DIR 0x10
#define FA_READ 1
#define FA_WRITE 2
#define FA_CREATE_ALWAYS 8
#define FA_OPEN_EXISTING 0
#define f_size(f) ((f)->size)
FRESULT f_open(FIL *f, const char *p, BYTE mode);
FRESULT f_read(FIL *f, void *b, UINT n, UINT *got);
FRESULT f_write(FIL *f, const void *b, UINT n, UINT *w);
FRESULT f_lseek(FIL *f, FSIZE_t o);
FRESULT f_close(FIL *f);
FRESULT f_unlink(const char *p);
FRESULT f_rename(const char *a, const char *b);
FRESULT f_opendir(DIR *d, const char *p);
FRESULT f_readdir(DIR *d, FILINFO *fi);
FRESULT f_closedir(DIR *d);
