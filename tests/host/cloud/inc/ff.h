#pragma once
#include <stdio.h>
#include <stdint.h>
#include <dirent.h>
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
    DIR *d;
    char path[300];
} FDIR;
#define DIR FDIR
typedef struct
{
    FSIZE_t fsize;
    uint16_t fdate, ftime;
    BYTE fattrib;
    char fname[256];
} FILINFO;
typedef struct
{
    uint8_t csize;
    uint32_t n_fatent;
} FATFS;
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
FRESULT f_sync(FIL *f);
FRESULT f_close(FIL *f);
FRESULT f_stat(const char *p, FILINFO *fi);
FRESULT f_unlink(const char *p);
FRESULT f_rename(const char *a, const char *b);
FRESULT f_mkdir(const char *p);
FRESULT f_chmod(const char *p, BYTE a, BYTE m);
FRESULT f_opendir(DIR *d, const char *p);
FRESULT f_readdir(DIR *d, FILINFO *fi);
FRESULT f_closedir(DIR *d);
FRESULT f_getfree(const char *p, DWORD *n, FATFS **fs);
