/*
  fs_fatfs.h - VFS mount for FatFs

  Part of grblHAL

  Copyright (c) 2022-2026 Terje Io

  grblHAL is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  grblHAL is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with grblHAL. If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#if defined(ESP_PLATFORM) || defined(STM32_PLATFORM) ||  defined(__LPC17XX__) ||  defined(__IMXRT1062__) || defined(__MSP432E401Y__)
#define NEW_FATFS
#endif

#ifdef NEW_FATFS
typedef struct {
    FATFS *fs;
    char name[10];
} fatfs_dev_t;
#else
typedef struct {
    FATFS *fs;
    int drive;
} fatfs_dev_t;
#endif

void fs_fatfs_mount (const char *path, const fatfs_dev_t *device);
