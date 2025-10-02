/*
  sdcard.c - streaming plugin for SDCard/FatFs

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

#include "sdcard.h"

#if FS_ENABLE & FS_SDCARD

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFLEN 80

#if defined(ESP_PLATFORM) || defined(STM32_PLATFORM) ||  defined(__LPC17XX__) ||  defined(__IMXRT1062__) || defined(__MSP432E401Y__)
#define NEW_FATFS
#endif

#include "grbl/report.h"
//#include "grbl/protocol.h"
#include "grbl/state_machine.h"
#include "grbl/stream_file.h"
#include "grbl/vfs.h"
#include "grbl/task.h"

#include "fs_fatfs.h"

#if defined(NEW_FATFS)
static char dev[10] = "";
#endif

// https://e2e.ti.com/support/tools/ccs/f/81/t/428524?Linking-error-unresolved-symbols-rom-h-pinout-c-

/* uses fatfs - http://www.elm-chan.org/fsw/ff/00index_e.html */

static FATFS *fatfs = NULL;
static bool mount_changed = false, realtime_report_subscribed = false, sd_detectable = false;
static xbar_t *detect_pin = NULL;
static sdcard_events_t sdcard;
static on_realtime_report_ptr on_realtime_report;
static on_report_options_ptr on_report_options;
static driver_setup_ptr driver_setup;
static settings_changed_ptr settings_changed;

static void onRealtimeReport (stream_write_ptr stream_write, report_tracking_flags_t report);

#ifdef __MSP432E401Y__
/*---------------------------------------------------------*/
/* User Provided Timer Function for FatFs module           */
/*---------------------------------------------------------*/
/* This is a real time clock service to be called from     */
/* FatFs module. Any valid time must be returned even if   */
/* the system does not support a real time clock.          */

DWORD fatfs_getFatTime (void)
{
    return    ((2007UL-1980) << 25)  // Year = 2007
            | (6UL << 21)            // Month = June
            | (5UL << 16)            // Day = 5
            | (11U << 11)            // Hour = 11
            | (38U << 5)             // Min = 38
            | (0U >> 1)              // Sec = 0
            ;

}
#endif

static bool sdcard_mount (void)
{
    static FATFS *fs = NULL;

    bool is_mounted = !!fatfs;

    if(sdcard.on_mount) {

        char *mdev = sdcard.on_mount(&fatfs);

        if(fatfs != NULL) {
#ifdef NEW_FATFS
            if(mdev)
                strcpy(dev, mdev);
#endif
        }
    } else {

        if(fs == NULL)
            fs = malloc(sizeof(FATFS));

#ifdef NEW_FATFS
        if(fs && (f_mount(fs, dev, 1) == FR_OK))
#else
        if(fs && (f_mount(0, fs) == FR_OK))
#endif
            fatfs = fs;
        else
            fatfs = NULL;
    }

    if((mount_changed = is_mounted != !!fatfs) && !realtime_report_subscribed) {
        realtime_report_subscribed = true;
        on_realtime_report = grbl.on_realtime_report;
        grbl.on_realtime_report = onRealtimeReport; // Add mount status changes and job percent complete to real time report
    }

    if(fatfs != NULL)
        fs_fatfs_mount("/");

    return fatfs != NULL;
}

static void sdcard_auto_mount (void *data)
{
    if(fatfs == NULL && !sdcard_mount())
        report_message("SD card automount failed", Message_Info);
}

static bool sdcard_unmount (void)
{
    if(fatfs) {
        if(sdcard.on_unmount)
            mount_changed = sdcard.on_unmount(&fatfs);
#ifdef NEW_FATFS
        else
            mount_changed = f_unmount(dev) == FR_OK;
#else
        else
            mount_changed = f_mount(0, NULL) == FR_OK;
#endif
        if(mount_changed && fatfs) {
            fatfs = NULL;
            vfs_unmount("/");
        }
    }

    return fatfs == NULL;
}

static status_code_t sd_cmd_mount (sys_state_t state, char *args)
{
    return sdcard_mount() ? Status_OK : Status_SDMountError;
}

static status_code_t sd_cmd_unmount (sys_state_t state, char *args)
{
    return fatfs ? (sdcard_unmount() ? Status_OK : Status_SDMountError) : Status_SDNotMounted;
}

#if FF_FS_READONLY == 0 && FF_USE_MKFS == 1

static status_code_t sd_cmd_format (sys_state_t state, char *args)
{
    bool ok;
    status_code_t status = Status_NonPositiveValue; // i.e. not confirmed

    if(fatfs) {

        vfs_drive_t *drive = vfs_get_drive("/");

        if(drive->fs && !strcmp(args, "yes")) {

            report_message("Formatting SD card...", Message_Info);

            if((ok = vfs_drive_format(drive) >= 0))
                status = !sdcard_mount() ? Status_SDMountError : Status_OK;
            else
                status = Status_FsFormatFailed;

            report_message("", Message_Plain);
        }
    } else
        status = Status_SDNotMounted;

    return status;
}

#endif

static void sd_detect (void *mount)
{
    if((uint32_t)mount == 0)
        sdcard_unmount();
    else if(fatfs == NULL)
        sdcard_mount();
}

ISR_CODE void ISR_FUNC(sdcard_detect)(bool mount)
{
    task_add_immediate(sd_detect, (void *)mount);
}

static void onRealtimeReport (stream_write_ptr stream_write, report_tracking_flags_t report)
{
    if(report.all || mount_changed) {
        stream_write("|SD:");
        stream_write(uitoa((sd_detectable ? 2 : 0) + !!fatfs));
        mount_changed = false;
    }

    if(on_realtime_report)
        on_realtime_report(stream_write, report);
}

static void sd_detect_pin (xbar_t *pin, void *data)
{
    if(pin->id == Input_SdCardDetect) {
        sd_detectable = true;
        if(pin->get_value)
            detect_pin = pin;
    }
}

static void onSettingsChanged (settings_t *settings, settings_changed_flags_t changed)
{
    static bool mount_attempted = false; // in case some other code hooked into hal.settings_changed

    settings_changed(settings, changed);

    if(!mount_attempted) {
        mount_attempted = true;
        sdcard_mount();
    }
}

static bool onDriverSetup (settings_t *settings)
{
    bool ok;

    settings_changed = hal.settings_changed;
    hal.settings_changed = onSettingsChanged;

    ok = driver_setup(settings);

    if(hal.settings_changed == onSettingsChanged)
        hal.settings_changed = settings_changed;

    return ok;
}

// Attempt early mount before other clients access a shared SPI bus.
void sdcard_early_mount (void)
{
    if(detect_pin == NULL || detect_pin->get_value(detect_pin) == 0.0f) {
        driver_setup = hal.driver_setup;
        hal.driver_setup = onDriverSetup;
    }
}

static void onReportOptions (bool newopt)
{
    on_report_options(newopt);

    if(newopt)
        hal.stream.write(",SD");
    else
        report_plugin("SDCARD", "1.25");
}

sdcard_events_t *sdcard_init (void)
{
    PROGMEM static const sys_command_t sdcard_command_list[] = {
        {"FM", sd_cmd_mount, { .noargs = On }, { .str = "mount SD card" } },
        {"FU", sd_cmd_unmount, { .noargs = On }, { .str = "unmount SD card" } },
#if FF_FS_READONLY == 0 && FF_USE_MKFS == 1
        {"FF", sd_cmd_format, {}, { .str = "$FF=yes - format SD card" } },
#endif
    };

    static sys_commands_t sdcard_commands = {
        .n_commands = sizeof(sdcard_command_list) / sizeof(sys_command_t),
        .commands = sdcard_command_list
    };

    PROGMEM static const status_detail_t status_detail[] = {
        { Status_SDMountError, "SD Card mount failed." },
        { Status_SDNotMounted, "SD Card not mounted." }
    };

    static error_details_t error_details = {
        .errors = status_detail,
        .n_errors = sizeof(status_detail) / sizeof(status_detail_t)
    };

    hal.driver_cap.sd_card = On;

    hal.enumerate_pins(false, sd_detect_pin, NULL);

    on_report_options = grbl.on_report_options;
    grbl.on_report_options = onReportOptions;

    errors_register(&error_details);
    system_register_commands(&sdcard_commands);

    if(settings.fs_options.sd_mount_on_boot)
        task_run_on_startup(sdcard_auto_mount, NULL);

    return &sdcard;
}

FATFS *sdcard_getfs (void)
{
    if(fatfs == NULL)
        sdcard_mount();

    return fatfs;
}

#endif // FS_ENABLE & FS_SDCARD
