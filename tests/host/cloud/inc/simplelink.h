#pragma once
#include <stdint.h>
#define SL_AF_INET 2
#define SL_SOCK_STREAM 1
#define SL_SEC_SOCKET 100
#define SL_SOL_SOCKET 1
#define SL_SO_RCVTIMEO 20
#define SL_SO_SECURE_FILES_PRIVATE_KEY_FILE_NAME 30
#define SL_SO_SECURE_FILES_CERTIFICATE_FILE_NAME 31
#define SL_SO_SECURE_FILES_CA_FILE_NAME 32
#define SL_EAGAIN (-11)
#define SL_ENOBUFS (-105)
#define SL_ESECT00MANYSSLOPENED (-460)
#define SL_DEVICE_GENERAL_CONFIGURATION 1
#define SL_DEVICE_GENERAL_CONFIGURATION_DATE_TIME 11
#define SL_IPV4_DNS_CLIENT 10
#define SL_IPV4_VAL(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define FS_MODE_OPEN_READ 0
#define FS_MODE_OPEN_WRITE 2
#define FS_MODE_OPEN_CREATE(s, f) 1
#define _FS_FILE_OPEN_FLAG_COMMIT 1
typedef uint32_t SlSocklen_t;
typedef struct
{
    uint32_t tv_sec, tv_usec;
} SlTimeval_t;
typedef struct
{
    uint16_t sa_family;
    uint8_t sa_data[14];
} SlSockAddr_t;
typedef struct
{
    uint16_t sin_family;
    uint16_t sin_port;
    struct
    {
        uint32_t s_addr;
    } sin_addr;
    uint8_t z[8];
} SlSockAddrIn_t;
typedef struct
{
    uint32_t sl_tm_sec, sl_tm_min, sl_tm_hour, sl_tm_day, sl_tm_mon, sl_tm_year, sl_tm_week_day, sl_tm_year_day, reserved[3];
} SlDateTime_t;
typedef struct
{
    uint16_t flags;
    uint32_t FileLen;
    uint32_t AllocatedLen;
    uint32_t Token[4];
} SlFsFileInfo_t;
typedef struct
{
    uint32_t DnsSecondServerAddr, DnsMaxRetries;
} SlNetCfgIpV4DnsClientArgs_t;
static inline uint16_t sl_Htons(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }
static inline uint32_t sl_Htonl(uint32_t v) { return __builtin_bswap32(v); }
int16_t sl_Socket(int16_t d, int16_t t, int16_t p);
int16_t sl_Close(int16_t s);
int16_t sl_Connect(int16_t s, const SlSockAddr_t *a, int16_t l);
int16_t sl_SetSockOpt(int16_t s, int16_t l, int16_t o, const void *v, SlSocklen_t n);
int16_t sl_Send(int16_t s, const void *b, int16_t n, int16_t f);
int16_t sl_Recv(int16_t s, void *b, int16_t n, int16_t f);
int16_t sl_NetAppDnsGetHostByName(signed char *h, uint16_t n, unsigned long *ip, uint8_t f);
int32_t sl_DevSet(uint8_t a, uint8_t b, uint8_t c, uint8_t *d);
int32_t sl_NetCfgSet(uint8_t a, uint8_t b, uint8_t c, uint8_t *d);
long sl_FsOpen(unsigned char *n, unsigned long mode, unsigned long *tok, long *fh);
long sl_FsRead(long fh, unsigned long off, unsigned char *buf, unsigned long len);
long sl_FsWrite(long fh, unsigned long off, unsigned char *buf, unsigned long len);
short sl_FsClose(long fh, unsigned char *a, unsigned char *b, unsigned long c);
short sl_FsDel(unsigned char *n, unsigned long t);
short sl_FsGetInfo(unsigned char *n, unsigned long t, SlFsFileInfo_t *i);
/* Server-side socket calls (the setup page, src/cc3200/sock_port.h). */
#define SL_SO_NONBLOCKING 24
#define SL_NET_APP_HTTP_SERVER_ID 1
typedef struct
{
    uint32_t NonblockingEnabled;
} SlSockNonblocking_t;
int16_t sl_Bind(int16_t s, const SlSockAddr_t *a, int16_t l);
int16_t sl_Listen(int16_t s, int16_t n);
int16_t sl_Accept(int16_t s, SlSockAddr_t *a, SlSocklen_t *l);
int16_t sl_NetAppStop(uint32_t m);
