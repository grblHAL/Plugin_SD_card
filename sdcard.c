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

#if SDCARD_ENABLE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFLEN 80

#if defined(ESP_PLATFORM) || defined(STM32_PLATFORM) ||  defined(__LPC17XX__) ||  defined(__IMXRT1062__) || defined(__MSP432E401Y__)
#define NEW_FATFS
#endif

#ifdef ARDUINO
  #include "../grbl/report.h"
  #include "../grbl/protocol.h"
  #include "../grbl/state_machine.h"
  #include "../grbl/stream_file.h"
  #include "../grbl/vfs.h"
#else
  #include "grbl/report.h"
  #include "grbl/protocol.h"
  #include "grbl/state_machine.h"
  #include "grbl/stream_file.h"
  #include "grbl/vfs.h"
#endif

#include "ymodem.h"
#include "macros.h"
#include "fs_fatfs.h"

#if defined(NEW_FATFS)
static char dev[10] = "";
#endif

// https://e2e.ti.com/support/tools/ccs/f/81/t/428524?Linking-error-unresolved-symbols-rom-h-pinout-c-

/* uses fatfs - http://www.elm-chan.org/fsw/ff/00index_e.html */

#define MAX_PATHLEN 128

char const *const filetypes[] = {
    "nc",
    "ncc",
    "ngc",
    "cnc",
    "gcode",
    "txt",
    "text",
    "tap",
    "macro",
    ""
};

static vfs_file_t *cncfile;

typedef enum {
    Filename_Filtered = 0,
    Filename_Valid,
    Filename_Invalid
} file_status_t;

typedef struct
{
    FATFS *fs;
    vfs_file_t *handle;
    char name[50];
    size_t size;
    size_t pos;
    uint32_t line;
    uint8_t eol;
} file_t;

static file_t file = {
    .fs = NULL,
    .handle = NULL,
    .size = 0,
    .pos = 0
};

static bool frewind = false, webui = false, mount_changed = false, realtime_report_subscribed = false, sd_detectable = false;
static io_stream_t active_stream;
static sdcard_events_t sdcard;
static driver_reset_ptr driver_reset;
static on_realtime_report_ptr on_realtime_report;
static on_state_change_ptr state_change_requested;
static on_program_completed_ptr on_program_completed;
static enqueue_realtime_command_ptr enqueue_realtime_command;
static on_report_options_ptr on_report_options;
static on_stream_changed_ptr on_stream_changed;
static stream_read_ptr read_redirected;
static status_message_ptr status_message = NULL;

static void sdcard_end_job (bool flush);
static void onRealtimeReport (stream_write_ptr stream_write, report_tracking_flags_t report);
static void trap_state_change_request(uint_fast16_t state);
static status_code_t trap_status_messages (status_code_t status_code);
static void sdcard_on_program_completed (program_flow_t program_flow, bool check_mode);
//static report_t active_reports;

#ifdef __MSP432E401Y__
/*---------------------------------------------------------*/
/* User Provided Timer Function for FatFs module           */
/*---------------------------------------------------------*/
/* This is a real time clock service to be called from     */
/* FatFs module. Any valid time must be returned even if   */
/* the system does not support a real time clock.          */

DWORD fatfs_getFatTime (void)
{

    return    ((2007UL-1980) << 25)    // Year = 2007
            | (6UL << 21)            // Month = June
            | (5UL << 16)            // Day = 5
            | (11U << 11)            // Hour = 11
            | (38U << 5)            // Min = 38
            | (0U >> 1)                // Sec = 0
            ;

}
#endif

static file_status_t filename_valid (char *filename)
{
    return strlen(filename) > 40 || strchr(filename, CMD_STATUS_REPORT) || strchr(filename, CMD_CYCLE_START) || strchr(filename, CMD_FEED_HOLD)
            ? Filename_Invalid
            : Filename_Valid;
}

static file_status_t allowed (char *filename, bool is_file)
{
    uint_fast8_t idx = 0;
    char filetype[8], *ftptr;
    file_status_t status = is_file ? Filename_Filtered : Filename_Valid;

    if(is_file && (ftptr = strrchr(filename, '.'))) {
        ftptr++;
        if(strlen(ftptr) > sizeof(filetype) - 1)
            return status;
        while(ftptr[idx]) {
            filetype[idx] = LCAPS(ftptr[idx]);
            idx++;
        }
        filetype[idx] = '\0';
        idx = 0;
        while(status == Filename_Filtered && filetypes[idx][0]) {
            if(!strcmp(filetype, filetypes[idx]))
                status = Filename_Valid;
            idx++;
        }
    }

    return status == Filename_Valid ? filename_valid(filename) : status;
}

static int scan_dir (char *path, uint_fast8_t depth, char *buf, bool filtered)
{
    int res = 0;
    bool subdirs = false;
    vfs_dir_t *dir;
    vfs_dirent_t *dirent;
    file_status_t status;

    if((dir = vfs_opendir(*path == '\0' ? "/" : path)) == NULL)
        return vfs_errno;

    // Pass 1: Scan files
    while(true) {

        if((dirent = vfs_readdir(dir)) == NULL || dirent->name[0] == '\0')
            break;
        if(vfs_errno)
            break;

        subdirs |= dirent->st_mode.directory;

        if(!dirent->st_mode.directory && (status = filtered ? allowed(dirent->name, true) : filename_valid(dirent->name)) != Filename_Filtered) {
            if(snprintf(buf, BUFLEN, "[FILE:%s/%s|SIZE:" UINT32FMT "%s]" ASCII_EOL, path, dirent->name, (uint32_t)dirent->size, status == Filename_Invalid ? "|UNUSABLE" : "") < BUFLEN)
                hal.stream.write(buf);
        }
    }

    int err = vfs_errno;

    vfs_closedir(dir);
    dir = NULL;

    if(err)
        return err;

    if((subdirs = (subdirs && --depth)))
        subdirs = (dir = vfs_opendir(*path == '\0' ? "/" : path)) != NULL;

    // Pass 2: Scan directories
    while(subdirs) {

        if((dirent = vfs_readdir(dir)) == NULL || dirent->name[0] == '\0')
            break;

        if(dirent->st_mode.directory) { // It is a directory
            size_t pathlen = strlen(path);
            if(pathlen + strlen(dirent->name) > (MAX_PATHLEN - 1))
                break;
            sprintf(&path[pathlen], "/%s", dirent->name);
            if((res = scan_dir(path, depth, buf, filtered)) != 0)
                break;
            path[pathlen] = '\0';
        }
    }

    if(dir)
        vfs_closedir(dir);

    return res;
}

static void file_close (void)
{
    if(file.handle) {
        vfs_close(file.handle);
        if(hal.stream.file == file.handle)
            hal.stream.file = NULL;
        file.handle = NULL;
    }
}

static bool file_open (char *filename)
{
    if(file.handle)
        file_close();

    if((cncfile = vfs_open(filename, "r")) != NULL) {

        vfs_stat_t st;

        vfs_stat(filename, &st);

        file.handle = cncfile;
        file.size = st.st_size;
        file.pos = 0;
        file.line = 0;
        file.eol = false;
        char *leafname = strrchr(filename, '/');
        strncpy(file.name, leafname ? leafname + 1 : filename, sizeof(file.name));
        file.name[sizeof(file.name) - 1] = '\0';
    }

    return file.handle != NULL;
}

static int16_t file_read (void)
{
    signed char c[1];

    if(vfs_read(&c, 1, 1, file.handle) == 1)
        file.pos = vfs_tell(file.handle);
    else
        *c = -1;

    if(*c == '\r' || *c == '\n')
        file.eol++;
    else
        file.eol = 0;

    return (int16_t)*c;
}

static bool sdcard_mount (void)
{
    static FATFS *fs = NULL;

    bool is_mounted = !!file.fs;

    if(sdcard.on_mount) {

        char *mdev = sdcard.on_mount(&file.fs);

        if(file.fs != NULL) {
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
            file.fs = fs;
        else
            file.fs = NULL;
    }

    if((mount_changed = is_mounted != !!file.fs) && !realtime_report_subscribed) {
        realtime_report_subscribed = true;
        on_realtime_report = grbl.on_realtime_report;
        grbl.on_realtime_report = onRealtimeReport; // Add mount status changes and job percent complete to real time report
    }

    if(file.fs != NULL)
        fs_fatfs_mount("/");

    return file.fs != NULL;
}

static void sdcard_auto_mount (void *data)
{
    if(file.fs == NULL && !sdcard_mount())
        report_message("SD card automount failed", Message_Info);
}

static bool sdcard_unmount (void)
{
    if(file.fs) {
        if(sdcard.on_unmount)
            mount_changed = sdcard.on_unmount(&file.fs);
#ifdef NEW_FATFS
        else
            mount_changed = f_unmount(dev) == FR_OK;
#else
        else
            mount_changed = f_mount(0, NULL) == FR_OK;
#endif
        if(mount_changed && file.fs) {
            file.fs = NULL;
            vfs_unmount("/");
        }
    }

    return file.fs == NULL;
}

static status_code_t sdcard_ls (bool filtered)
{
    char path[MAX_PATHLEN] = "", name[BUFLEN]; // NB! also used as work area when recursing directories

    return file.fs ? (scan_dir(path, 10, name, filtered) == FR_OK ? Status_OK : Status_SDFailedOpenDir) : Status_SDNotMounted;
}

static void sdcard_end_job (bool flush)
{
    file_close();

    if(grbl.on_program_completed == sdcard_on_program_completed)
        grbl.on_program_completed = on_program_completed;

    if(grbl.on_state_change == trap_state_change_request)
        grbl.on_state_change = state_change_requested;

    grbl.on_stream_changed = on_stream_changed;

    memcpy(&hal.stream, &active_stream, sizeof(io_stream_t));       // Restore stream pointers,
    stream_set_type(hal.stream.type, hal.stream.file);              // ...
    active_stream.type = StreamType_Null;                           // ...
    hal.stream.set_enqueue_rt_handler(enqueue_realtime_command);    // real time command handling and
    if(grbl.report.status_message == trap_status_messages)          // ...
        grbl.report.status_message = status_message;                // normal status message handling.
    else
        report_init_fns();

    status_message = NULL;

    if(flush)                                                       // Flush input buffer?
        hal.stream.reset_read_buffer();                             // Yes, do it.

    state_change_requested = NULL;

    webui = frewind = false;

    if(grbl.on_stream_changed)
        grbl.on_stream_changed(hal.stream.type);
}

static int16_t sdcard_read (void)
{
    int16_t c = SERIAL_NO_DATA;
    sys_state_t state = state_get();

    if(file.eol == 1)
        file.line++;

    if(file.handle) {

        if(state == STATE_IDLE || (state & (STATE_CYCLE|STATE_HOLD|STATE_CHECK_MODE|STATE_TOOL_CHANGE)))
            c = file_read();

        if(c == -1) { // EOF or error reading or grblHAL problem
            file_close();
            if(file.eol == 0) // Return newline if line was incorrectly terminated
                c = '\n';
        }

    } else if((state == STATE_IDLE || state == STATE_CHECK_MODE) && grbl.on_program_completed == sdcard_on_program_completed) { // TODO: end on ok count match line count?
        sdcard_on_program_completed(ProgramFlow_CompletedM30, state == STATE_CHECK_MODE);
        grbl.report.feedback_message(Message_ProgramEnd);
    }

    return c;
}

static int16_t await_cycle_start (void)
{
    return -1;
}

// Drop input from current stream except realtime commands
ISR_CODE static bool  ISR_FUNC(drop_input_stream)(char c)
{
    enqueue_realtime_command(c);

    return true;
}

static void trap_state_change_request (sys_state_t state)
{
    if(state == STATE_CYCLE) {

        if(hal.stream.read == await_cycle_start)
            hal.stream.read = read_redirected;

        if(grbl.on_state_change== trap_state_change_request) {
            grbl.on_state_change = state_change_requested;
            state_change_requested = NULL;
        }
    }

    if(state_change_requested)
        state_change_requested(state);
}

static status_code_t trap_status_messages (status_code_t status_code)
{
    if(hal.stream.read != read_redirected)
        status_code = status_message(status_code);

    else if(status_code != Status_OK) {

        // TODO: all errors should terminate job?
        char buf[50]; // TODO: check if extended error reports are permissible
        sprintf(buf, "error:%d in SD file at line " UINT32FMT ASCII_EOL, (uint8_t)status_code, file.line);
        hal.stream.write(buf);

        sdcard_end_job(true);
        grbl.report.status_message(status_code);
    }

    return status_code;
}

static void sdcard_restart_msg (void *data)
{
    grbl.report.feedback_message(Message_CycleStartToRerun);
}

static void sdcard_on_program_completed (program_flow_t program_flow, bool check_mode)
{
#if WEBUI_ENABLE // TODO: somehow add run time check?
    frewind = false; // Not (yet?) supported.
#else
    frewind = frewind || program_flow == ProgramFlow_CompletedM2; // || program_flow == ProgramFlow_CompletedM30;
#endif
    if(frewind) {
        vfs_seek(file.handle, 0);
        file.pos = file.line = 0;
        file.eol = false;
        hal.stream.read = await_cycle_start;
        if(grbl.on_state_change != trap_state_change_request) {
            state_change_requested = grbl.on_state_change;
            grbl.on_state_change = trap_state_change_request;
        }
        protocol_enqueue_foreground_task(sdcard_restart_msg, NULL);
    } else
        sdcard_end_job(true);

    if(on_program_completed)
        on_program_completed(program_flow, check_mode);
}

ISR_CODE static bool ISR_FUNC(await_toolchange_ack)(char c)
{
    if(c == CMD_TOOL_ACK) {
        hal.stream.read = active_stream.read;                           // Restore normal stream input for tool change (jog etc)
        active_stream.set_enqueue_rt_handler(enqueue_realtime_command); // ...
    } else
        return enqueue_realtime_command(c);

    return true;
}

static bool sdcard_suspend (bool suspend)
{
    if(suspend) {
        hal.stream.read = stream_get_null;                              // Set read function to return empty,
        active_stream.reset_read_buffer();                              // flush input buffer,
        active_stream.set_enqueue_rt_handler(await_toolchange_ack);     // and set handler to wait for tool change acknowledge.
    } else {
        hal.stream.read = read_redirected;                              // Resume reading from SD card
        hal.stream.set_enqueue_rt_handler(drop_input_stream);           // ..
    }

    return true;
}

static void terminate_job (void *data)
{
    if(state_get() == STATE_CYCLE) {
        // Halt motion so that executing stop does not result in loss of position
        system_set_exec_state_flag(EXEC_MOTION_CANCEL);
        do {
            if(!protocol_execute_realtime()) // Check for system abort
                break;
        } while (state_get() != STATE_IDLE);
    }

    sys.flags.keep_input = On;
    system_set_exec_state_flag(EXEC_STOP);

    sdcard_end_job(false);

    report_message("SD card job terminated due to connection change", Message_Info);
}

static bool check_input_stream (char c)
{
    bool ok;

    if(!(ok = enqueue_realtime_command(c))) {
        if(hal.stream.read != stream_get_null) {
            hal.stream.read = stream_get_null;
            protocol_enqueue_foreground_task(terminate_job, NULL);
        }
    }

    return ok;
}

static void stream_changed (stream_type_t type)
{
    if(type != StreamType_File && file.handle != NULL) {

        // Reconnect from WebUI?
        if(webui && (type != StreamType_WebSocket || hal.stream.state.webui_connected)) {
            active_stream.set_enqueue_rt_handler(enqueue_realtime_command); // Restore previous real time handler,
            memcpy(&active_stream, &hal.stream, sizeof(io_stream_t));       // save current stream pointers
            hal.stream.read = read_redirected;                              // then redirect to read from SD card instead
            stream_set_type(StreamType_File, file.handle);                  // ...

            if(hal.stream.suspend_read)                                     // If active stream support tool change suspend
                hal.stream.suspend_read = sdcard_suspend;                   // then we do as well
            else                                                            //
                hal.stream.suspend_read = NULL;                             // else not

            if(type == StreamType_WebSocket)                                                        // If WebUI came back online
                enqueue_realtime_command = hal.stream.set_enqueue_rt_handler(drop_input_stream);    // restore normal operation
            else                                                                                    // else
                enqueue_realtime_command = hal.stream.set_enqueue_rt_handler(check_input_stream);   // check for stream takeover
        } else // Terminate job.
            protocol_enqueue_foreground_task(terminate_job, NULL);
    }

    if(on_stream_changed)
        on_stream_changed(type);
}

status_code_t stream_file (sys_state_t state, char *fname)
{
    status_code_t retval = Status_Unhandled;

    if(!file.fs)
        retval = Status_SDNotMounted;
    else if(!(state == STATE_IDLE || state == STATE_CHECK_MODE))
        retval = Status_SystemGClock;
    else if(fname && file_open(fname)) {

        gc_state.last_error = Status_OK;            // Start with no errors
        grbl.report.status_message(Status_OK);      // and confirm command to originator.
        webui = hal.stream.state.webui_connected;   // Did WebUI start this job?

        if(!(grbl.on_file_open && (retval = grbl.on_file_open(fname, file.handle, true)) == Status_OK)) {

            memcpy(&active_stream, &hal.stream, sizeof(io_stream_t));   // Save current stream pointers
            hal.stream.read = sdcard_read;                              // then redirect to read from SD card instead
            stream_set_type(StreamType_File, file.handle);              // ...
            if(hal.stream.suspend_read)                                 // If active stream support tool change suspend
                hal.stream.suspend_read = sdcard_suspend;               // then we do as well
            else                                                        //
                hal.stream.suspend_read = NULL;                         // else not.

            on_program_completed = grbl.on_program_completed;
            grbl.on_program_completed = sdcard_on_program_completed;

            status_message = grbl.report.status_message;                // Add trap for status messages
            grbl.report.status_message = trap_status_messages;          // so we can terminate on errors.

            enqueue_realtime_command = hal.stream.set_enqueue_rt_handler(drop_input_stream);    // Drop input from current stream except realtime commands

            if(grbl.on_stream_changed)
                grbl.on_stream_changed(hal.stream.type);

            read_redirected = hal.stream.read;

            if(grbl.on_stream_changed != stream_changed) {
                on_stream_changed = grbl.on_stream_changed;
                grbl.on_stream_changed = stream_changed;
            }

            retval = Status_OK;
        } else
            file.handle = NULL;
    } else
        retval = Status_FileOpenFailed;

    return retval;
}

static status_code_t sd_cmd_file_filtered (sys_state_t state, char *args)
{
    status_code_t retval = Status_Unhandled;

    if(args)
        retval = stream_file(state, args);

    else {
        frewind = false;
        retval = sdcard_ls(true); // (re)use line buffer for reporting filenames
    }

    return retval;
}

static status_code_t sd_cmd_file_all (sys_state_t state, char *args)
{
    status_code_t retval = Status_Unhandled;

    if(args)
        retval = stream_file(state, args);

    else {
        frewind = false;
        retval = sdcard_ls(false); // (re)use line buffer for reporting filenames
    }

    return retval;
}

static status_code_t sd_cmd_mount (sys_state_t state, char *args)
{
    frewind = false;

    return sdcard_mount() ? Status_OK : Status_SDMountError;
}

static status_code_t sd_cmd_unmount (sys_state_t state, char *args)
{
    frewind = false;

    return file.fs ? (sdcard_unmount() ? Status_OK : Status_SDMountError) : Status_SDNotMounted;
}

static void sd_detect (void *mount)
{
    if((uint32_t)mount == 0)
        sdcard_unmount();
    else if(file.fs == NULL)
        sdcard_mount();
}

static status_code_t sd_cmd_rewind (sys_state_t state, char *args)
{
    frewind = true;

    return Status_OK;
}

static status_code_t sd_cmd_to_output (sys_state_t state, char *args)
{
    status_code_t retval = Status_Unhandled;

    if(!file.fs)
        retval = Status_SDNotMounted;
    else if(!(state == STATE_IDLE || state == STATE_CHECK_MODE))
        retval = Status_SystemGClock;
    else if(args) {
        if(file_open(args)) {

            if(!(grbl.on_file_open && (retval = grbl.on_file_open(args, file.handle, false)) == Status_OK)) {

                int16_t c;
                char buf[2] = {0};

                while((c = file_read()) != -1) {
                    if(file.eol == 0) {
                        buf[0] = (char)c;
                        hal.stream.write(buf);
                    } else if(file.eol == 1)
                        hal.stream.write(ASCII_EOL);
                }

                file_close();
                retval = Status_OK;
            } else
                file.handle = NULL;
        } else
            retval = Status_FileOpenFailed;
    }

    return retval;
}

#if FF_FS_READONLY == 0 && FF_FS_MINIMIZE == 0

static status_code_t sd_cmd_unlink (sys_state_t state, char *args)
{
    status_code_t retval = Status_Unhandled;

    if(!file.fs)
        retval = Status_SDNotMounted;
    else if(!(state == STATE_IDLE || state == STATE_CHECK_MODE))
        retval = Status_SystemGClock;
    else if(args)
        retval = vfs_unlink(args) ? Status_OK : Status_SDReadError;

    return retval;
}

#endif

static void sdcard_reset (void)
{
    if(hal.stream.type == StreamType_File && active_stream.type != StreamType_Null) {
        if(file.line > 0) {
            char buf[70];
            sprintf(buf, "Reset during streaming of SD file at line: " UINT32FMT, file.line);
            report_message(buf, Message_Plain);
        } else if(frewind)
            grbl.report.feedback_message(Message_None);
        sdcard_end_job(true);
    }

    driver_reset();
}

ISR_CODE void ISR_FUNC(sdcard_detect)(bool mount)
{
    task_add_immediate(sd_detect, (void *)mount);
}

static void onRealtimeReport (stream_write_ptr stream_write, report_tracking_flags_t report)
{
    if(hal.stream.read == read_redirected) {

        char *pct_done = ftoa((float)file.pos / (float)file.size * 100.0f, 1);

        if(state_get() != STATE_IDLE && !strncmp(pct_done, "100.0", 5))
            strcpy(pct_done, "99.9");

        stream_write("|SD:");
        stream_write(pct_done);
        stream_write(",");
        stream_write(file.name);
    } else if(hal.stream.read == await_cycle_start)
        stream_write("|SD:Pending");
    else if(report.all || mount_changed) {
        stream_write("|SD:");
        stream_write(uitoa((sd_detectable ? 2 : 0) + !!file.fs));
        mount_changed = false;
    }

    if(on_realtime_report)
        on_realtime_report(stream_write, report);
}

static void sd_detect_pin (xbar_t *pin, void *data)
{
    if(pin->id == Input_SdCardDetect)
        sd_detectable = true;
}

static void onReportOptions (bool newopt)
{
    on_report_options(newopt);

    if(newopt)
#if SDCARD_ENABLE == 2 && FF_FS_READONLY == 0
        hal.stream.write(hal.stream.write_char == NULL ? ",SD" : ",SD,YM");
#else
        hal.stream.write(",SD");
#endif
    else
        report_plugin("SDCARD", "1.21");
}

sdcard_events_t *sdcard_init (void)
{
    PROGMEM static const sys_command_t sdcard_command_list[] = {
        {"F", sd_cmd_file_filtered, {}, {
            .str = "list files on SD card, filtered"
         ASCII_EOL "$F=<filename> - run SD card file"
        } },
        {"F+", sd_cmd_file_all, {}, { .str = "$F+ - list all files on SD card" } },
        {"FM", sd_cmd_mount, { .noargs = On }, { .str = "mount SD card" } },
        {"FU", sd_cmd_unmount, { .noargs = On }, { .str = "unmount SD card" } },
        {"FR", sd_cmd_rewind, { .noargs = On }, { .str = "enable rewind mode for next SD card file to run" } },
    #if FF_FS_READONLY == 0 && FF_FS_MINIMIZE == 0
        {"FD", sd_cmd_unlink, {}, { .str = "$FD=<filename> - delete SD card file" } },
    #endif
        {"F<", sd_cmd_to_output, {}, { .str = "$F<=<filename> - dump SD card file to output" } },
    };

    static sys_commands_t sdcard_commands = {
        .n_commands = sizeof(sdcard_command_list) / sizeof(sys_command_t),
        .commands = sdcard_command_list
    };

    PROGMEM static const status_detail_t status_detail[] = {
        { Status_SDMountError, "SD Card mount failed." },
        { Status_SDReadError, "SD Card file delete failed." },
        { Status_SDFailedOpenDir, "SD Card directory listing failed." },
        { Status_SDDirNotFound, "SD Card directory not found." },
        { Status_SDNotMounted, "SD Card not mounted." }
    };

    static error_details_t error_details = {
        .errors = status_detail,
        .n_errors = sizeof(status_detail) / sizeof(status_detail_t)
    };

    active_stream.type = StreamType_Null;

    hal.driver_cap.sd_card = On;

    hal.enumerate_pins(false, sd_detect_pin, NULL);

    driver_reset = hal.driver_reset;
    hal.driver_reset = sdcard_reset;

    on_report_options = grbl.on_report_options;
    grbl.on_report_options = onReportOptions;

    errors_register(&error_details);
    system_register_commands(&sdcard_commands);

#if SDCARD_ENABLE == 2 && FF_FS_READONLY == 0
    if(hal.stream.write_char != NULL)
        ymodem_init();
#endif

    fs_macros_init();

    if(settings.fs_options.sd_mount_on_boot)
        protocol_enqueue_foreground_task(sdcard_auto_mount, NULL);

    return &sdcard;
}

bool sdcard_busy (void)
{
    return stream_is_file();
}

sdcard_job_t *sdcard_get_job_info (void)
{
    static sdcard_job_t job;

    if(stream_is_file()) {
        strcpy(job.name, file.name);
        job.size = file.size;
        job.pos = file.pos;
        job.line = file.line;
    }

    return stream_is_file() ? &job : NULL;
}

FATFS *sdcard_getfs (void)
{
    if(file.fs == NULL)
        sdcard_mount();

    return file.fs;
}

#endif
