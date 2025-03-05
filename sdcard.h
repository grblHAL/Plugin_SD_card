/*
  sdcard.h - SDCard plugin for FatFs

  Part of grblHAL

  Copyright (c) 2018-2025 Terje Io

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

#include "driver.h"
#include "grbl/hal.h"
#include "grbl/platform.h"

#if FS_ENABLE & FS_SDCARD

#include "fs_stream.h"

#if defined(ESP_PLATFORM)
#include "esp_vfs_fat.h"
#elif defined(__LPC176x__) || defined(__MSP432E401Y__)
#include "fatfs/ff.h"
#include "fatfs/diskio.h"
#elif defined(ARDUINO_SAMD_MKRZERO)
#include "../../ff.h"
#include "../../diskio.h"
#elif defined(STM32_PLATFORM) || defined(__LPC17XX__) || defined(__IMXRT1062__) || defined(RP2040)
#include "ff.h"
#include "diskio.h"
#else
#include "fatfs/src/ff.h"
#include "fatfs/src/diskio.h"
#endif

typedef char *(*on_mount_ptr)(FATFS **fs);
typedef bool (*on_unmount_ptr)(FATFS **fs);

typedef struct {
    on_mount_ptr on_mount;
    on_unmount_ptr on_unmount;
} sdcard_events_t;

typedef stream_job_t sdcard_job_t;

#define sdcard_get_job_info() stream_get_job_info() // Deprecated, call stream_get_job_info() directly.
#define sdcard_busy() stream_is_file()              // Deprecated, call stream_is_file() directly.

sdcard_events_t *sdcard_init (void);
FATFS *sdcard_getfs (void);
void sdcard_detect (bool mount);
void sdcard_early_mount (void);

#endif // FS_ENABLE & FS_SDCARD
