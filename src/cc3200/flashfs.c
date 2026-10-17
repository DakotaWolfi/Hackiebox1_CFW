/*
 * Flash files on the serial flash, through the network processor's file
 * system (SimpleLink).

 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stddef.h>
#include "simplelink.h"
#include "flashfs.h"

long flashfs_size(const char *name)
{
    SlFsFileInfo_t info;
    long r = sl_FsGetInfo((unsigned char *)name, 0, &info);
    return r < 0 ? r : (long)info.FileLen;
}

long flashfs_read(const char *name, uint32_t offset, void *buf, unsigned n)
{
    long fh;
    long r = sl_FsOpen((unsigned char *)name, FS_MODE_OPEN_READ, NULL, &fh);
    if (r < 0)
    {
        return r;
    }
    r = sl_FsRead(fh, offset, buf, n);
    sl_FsClose(fh, NULL, NULL, 0);
    return r;
}

long flashfs_write(const char *name, const void *buf, unsigned n)
{
    long fh, r = sl_FsOpen((unsigned char *)name, FS_MODE_OPEN_WRITE, NULL, &fh);
    if (r < 0)
    {
        r = sl_FsOpen((unsigned char *)name, FS_MODE_OPEN_CREATE(n < 64 ? 64 : n, _FS_FILE_OPEN_FLAG_COMMIT), NULL,
                      &fh);
    }
    if (r < 0)
    {
        return r;
    }
    r = sl_FsWrite(fh, 0, (unsigned char *)buf, n);
    long c = sl_FsClose(fh, NULL, NULL, 0);
    return c < 0 ? c : r;
}

long flashfs_replace(const char *name, const void *buf, unsigned n, bool failsafe)
{
    long fh;
    sl_FsDel((unsigned char *)name, 0);
    long r = sl_FsOpen((unsigned char *)name, FS_MODE_OPEN_CREATE(n, failsafe ? _FS_FILE_OPEN_FLAG_COMMIT : 0), NULL,
                       &fh);
    if (r >= 0)
    {
        r = sl_FsWrite(fh, 0, (unsigned char *)buf, n);
        sl_FsClose(fh, NULL, NULL, 0);
    }
    return r;
}

long flashfs_delete(const char *name)
{
    return sl_FsDel((unsigned char *)name, 0);
}
