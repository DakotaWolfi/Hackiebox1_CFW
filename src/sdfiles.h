/*
 * Files of the content cache: content paths, directory
 * creation, the trash, the room procedure, the outdated mark.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SDFILES_H
#define SDFILES_H

#include <stdbool.h>
#include <stdint.h>

/* "content/<d>/<f>" in lower case; path needs 26 bytes. */
void sd_content_path(const uint8_t uid[8], char *path);

/* A FAT date and time (FILINFO fdate, ftime) in Unix seconds; 0 when the
 * entry has none or an invalid one. */
uint32_t sd_fat_time(uint16_t fdate, uint16_t ftime);

/* creates the missing directories of a file's path. */
bool sd_make_dirs(const char *file_path);

/* moves the content file of uid to the directory
 * dir (TRASHCAN or BADBLOCK). */
void sd_move_aside(const uint8_t uid[8], const char *dir);
static inline void sd_trash(const uint8_t uid[8])
{
    sd_move_aside(uid, "TRASHCAN");
}

/* Frees room on the card, threshold and target in clusters; stop is polled. */
bool sd_room(uint32_t threshold, uint32_t target, volatile bool *stop, uint8_t *work);

/* Frees room for a new content file of z bytes. */
bool sd_room_for(uint32_t z, volatile bool *stop, uint8_t *work);

/* outdated mark (hidden attribute). */
bool sd_outdated(const char *path);
void sd_set_outdated(const char *path, bool on);

/* Setup page: deletes every content except the sound prompts, the trash
 * and the extension's files. */
void sd_delete_content(void);

#endif
