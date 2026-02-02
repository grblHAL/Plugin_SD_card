/*
  fs_stream.c - file streaming plugin

  Part of grblHAL

  Copyright (c) 2018-2026 Terje Io

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

#if FS_ENABLE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFLEN 80

#include "grbl/report.h"
#include "grbl/protocol.h"
#include "grbl/state_machine.h"
#include "grbl/stream_file.h"
#include "grbl/vfs.h"
#include "grbl/task.h"

#include "fs_stream.h"
#include "macros.h"

#if FS_ENABLE & FS_SDCARD
#include "fs_fatfs.h"
#endif

#if FS_ENABLE & FS_YMODEM
extern void ymodem_init (void);
#endif

#define MAX_PATHLEN 128

static char const *const filetypes[] = {
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
    vfs_file_t *handle;
    char name[50];
    size_t size;
    size_t pos;
    uint32_t line;
    uint8_t eol;
    bool scan_subs;
} file_t;

static file_t file = {
    .handle = NULL,
    .size = 0,
    .pos = 0
};

static struct {
    bool mounted;
    vfs_st_mode_t mode;
} fs = {};

static bool frewind = false, webui = false;
static io_stream_t active_stream;
static driver_reset_ptr driver_reset = NULL;
static on_realtime_report_ptr on_realtime_report;
static on_cycle_start_ptr on_cycle_start;
static on_program_completed_ptr on_program_completed;
static enqueue_realtime_command_ptr enqueue_realtime_command;
static on_report_options_ptr on_report_options;
static on_stream_changed_ptr on_stream_changed;
static stream_read_ptr read_redirected;
static status_message_ptr status_message = NULL;
static on_vfs_mount_ptr on_vfs_mount;
static on_vfs_unmount_ptr on_vfs_unmount;

static void onCycleStart (void);
static void stream_end_job (bool flush);
static void onRealtimeReport (stream_write_ptr stream_write, report_tracking_flags_t report);
static status_code_t trap_status_messages (status_code_t status_code);
static void onProgramCompleted (program_flow_t program_flow, bool check_mode);
//static report_t active_reports;

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
    bool subdirs = false, add_sep, is_root = !strcmp(path, "/");
    vfs_dir_t *dir;
    vfs_dirent_t *dirent;
    file_status_t status;

    if((dir = vfs_opendir(*path == '\0' ? "/" : path)) == NULL)
        return vfs_errno;

    if(!is_root && depth == 0) {
        *path = '\0';
        hal.stream.write("[FILE:..|SIZE:-1]" ASCII_EOL);
    }

    add_sep = strlen(path) > 1;

    // Pass 1: Scan files
    while(true) {

        if((dirent = vfs_readdir(dir)) == NULL || dirent->name[0] == '\0')
            break;
        if(vfs_errno)
            break;

        subdirs |= depth > 0 && dirent->st_mode.directory;

        if(!dirent->st_mode.directory && (status = filtered ? allowed(dirent->name, true) : filename_valid(dirent->name)) != Filename_Filtered) {
            if(snprintf(buf, BUFLEN, "[FILE:%s%s%s|SIZE:" UINT32FMT "%s]" ASCII_EOL, path, add_sep ? "/" : "", dirent->name, (uint32_t)dirent->size, status == Filename_Invalid ? "|UNUSABLE" : "") < BUFLEN)
                hal.stream.write(buf);
        }

        if(depth == 0 && dirent->st_mode.directory && snprintf(buf, BUFLEN, "[FILE:%s%s|SIZE:-1]" ASCII_EOL, path, dirent->name))
            hal.stream.write(buf);
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

        if(dirent->st_mode.directory) {

            size_t pathlen = strlen(path);
            if(pathlen + strlen(dirent->name) >= (MAX_PATHLEN - 1))
                break;

            if(pathlen > 1)
                strcat(&path[pathlen], "/");
            strcat(&path[pathlen], dirent->name);

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
        file.pos = file.line = file.eol = 0;
        file.scan_subs = false;
        char *leafname = strrchr(filename, '/');
        strncpy(file.name, leafname ? leafname + 1 : filename, sizeof(file.name));
        file.name[sizeof(file.name) - 1] = '\0';
    }

    return file.handle != NULL;
}

static void file_rewind (void)
{
    vfs_seek(file.handle, 0);
    file.pos = file.line = file.eol = 0;
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

static status_code_t list_files (bool filtered)
{
    char path[MAX_PATHLEN] = "", name[BUFLEN]; // NB! also used as work area when recursing directories

    vfs_getcwd(path, MAX_PATHLEN);

    return fs.mounted ? (scan_dir(path, settings.fs_options.hierarchical_listing ? 0 : 10, name, filtered) == 0 ? Status_OK : Status_FsFailedOpenDir) : Status_FsNotMounted;
}

static void stream_end_job (bool flush)
{
    file_close();

    if(grbl.on_program_completed == onProgramCompleted)
        grbl.on_program_completed = on_program_completed;

    if(grbl.on_cycle_start == onCycleStart) {
        grbl.on_cycle_start = on_cycle_start;
        on_cycle_start = NULL;
    }

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

    webui = frewind = false;

    if(grbl.on_stream_changed)
        grbl.on_stream_changed(hal.stream.type);
}

static int32_t stream_read (void)
{
    int32_t c = SERIAL_NO_DATA;
    sys_state_t state = state_get();

    if(file.eol == 1)
        file.line++;

    if(file.handle) {

        if(state == STATE_IDLE || (state & (STATE_CYCLE|STATE_HOLD|STATE_CHECK_MODE|STATE_TOOL_CHANGE)))
            c = file_read();

        if(c == -1) { // EOF, error reading or grblHAL problem

            if(file.scan_subs) {
                hal.stream.state.m98_macro_prescan = file.scan_subs = false;
                file_rewind();
                state_set(STATE_IDLE);
            } else
                file_close();

            if(file.eol == 0) // Return newline if line was incorrectly terminated
                c = '\n';
        }

    } else if((state == STATE_IDLE || state == STATE_CHECK_MODE) && grbl.on_program_completed == onProgramCompleted) { // TODO: end on ok count match line count?
        onProgramCompleted(ProgramFlow_CompletedM30, state == STATE_CHECK_MODE);
        grbl.report.feedback_message(Message_ProgramEnd);
    }

    return c;
}

static int32_t await_cycle_start (void)
{
    return -1;
}

// Drop input from current stream except realtime commands
ISR_CODE static bool ISR_FUNC(drop_input_stream)(uint8_t c)
{
    enqueue_realtime_command(c);

    return true;
}

ISR_CODE static void ISR_FUNC(onCycleStart)(void)
{
    if(hal.stream.read == await_cycle_start)
        hal.stream.read = read_redirected;

    if(grbl.on_cycle_start == onCycleStart) {
        grbl.on_cycle_start = on_cycle_start;
        on_cycle_start = NULL;
    }

    if(on_cycle_start)
        on_cycle_start();
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

        stream_end_job(true);
        grbl.report.status_message(status_code);
    }

    return status_code;
}

static void sdcard_restart_msg (void *data)
{
    grbl.report.feedback_message(Message_CycleStartToRerun);
}

static void onProgramCompleted (program_flow_t program_flow, bool check_mode)
{
    if(file.scan_subs) {
        if(!hal.stream.state.m98_macro_prescan && !(hal.stream.state.m98_macro_prescan = program_flow == ProgramFlow_CompletedM2 || program_flow == ProgramFlow_CompletedM30)) {
            file.scan_subs = false;
            file_rewind();
            state_set(STATE_IDLE);
        }
    } else if(!hal.stream.state.m98_macro_prescan) {
#if !WEBUI_ENABLE // TODO: somehow add run time check?
        frewind = false; // Not (yet?) supported.
#else
        frewind = frewind || program_flow == ProgramFlow_CompletedM2;
#endif
        if((frewind && !hal.stream.state.webui_connected) || program_flow == ProgramFlow_Return) {
            file_rewind();
            if(program_flow != ProgramFlow_Return) {
                hal.stream.read = await_cycle_start;
                if(grbl.on_cycle_start != onCycleStart) {
                    on_cycle_start = grbl.on_cycle_start;
                    grbl.on_cycle_start = onCycleStart;
                }
                task_add_immediate(sdcard_restart_msg, NULL);
            }
        } else
            stream_end_job(true);
    }

    if(on_program_completed)
        on_program_completed(program_flow, check_mode);
}

ISR_CODE static bool ISR_FUNC(await_toolchange_ack)(uint8_t c)
{
    if(c == CMD_TOOL_ACK) {
        hal.stream.read = active_stream.read;                           // Restore normal stream input for tool change (jog etc)
        active_stream.set_enqueue_rt_handler(enqueue_realtime_command); // ...
        if(grbl.on_toolchange_ack)
            grbl.on_toolchange_ack();
    } else
        return enqueue_realtime_command(c);

    return true;
}

static bool stream_suspend (bool suspend)
{
    if(suspend) {
        hal.stream.read = stream_get_null;                              // Set read function to return empty,
        active_stream.reset_read_buffer();                              // flush input buffer,
        active_stream.set_enqueue_rt_handler(await_toolchange_ack);     // and set handler to wait for tool change acknowledge.
    } else {
        hal.stream.read = read_redirected;                              // Resume reading from file
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

    stream_end_job(false);

    report_message("Job terminated due to connection change", Message_Info);
}

static bool check_input_stream (uint8_t c)
{
    bool ok;

    if(!(ok = enqueue_realtime_command(c))) {
        if(hal.stream.read != stream_get_null) {
            hal.stream.read = stream_get_null;
            task_add_immediate(terminate_job, NULL);
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
            hal.stream.read = read_redirected;                              // then redirect to read from file instead
            stream_set_type(StreamType_File, file.handle);                  // ...

            if(hal.stream.suspend_read)                                     // If active stream support tool change suspend
                hal.stream.suspend_read = stream_suspend;                   // then we do as well
            else                                                            //
                hal.stream.suspend_read = NULL;                             // else not

            if(type == StreamType_WebSocket)                                                        // If WebUI came back online
                enqueue_realtime_command = hal.stream.set_enqueue_rt_handler(drop_input_stream);    // restore normal operation
            else                                                                                    // else
                enqueue_realtime_command = hal.stream.set_enqueue_rt_handler(check_input_stream);   // check for stream takeover
        } else // Terminate job.
            task_add_immediate(terminate_job, NULL);
    }

    if(on_stream_changed)
        on_stream_changed(type);
}

status_code_t stream_file (sys_state_t state, char *fname)
{
    vfs_stat_t st;
    status_code_t retval = Status_Unhandled;

    if(!fs.mounted)
        retval = Status_SDNotMounted;
    else if(!(state == STATE_IDLE || state == STATE_CHECK_MODE))
        retval = Status_SystemGClock;
    else if(fname && vfs_stat(fname, &st) == 0) {

        if(st.st_mode.directory)
            retval = vfs_chdir(fname) ? Status_FSDirNotFound : Status_OK;
        else if(file_open(fname)) {

            gc_state.last_error = Status_OK;            // Start with no errors
            grbl.report.status_message(Status_OK);      // and confirm command to originator.
            webui = hal.stream.state.webui_connected;   // Did WebUI start this job?

            if(!(grbl.on_file_open && (retval = grbl.on_file_open(fname, file.handle, true)) == Status_OK)) {

                memcpy(&active_stream, &hal.stream, sizeof(io_stream_t));   // Save current stream pointers
                hal.stream.read = stream_read;                              // then redirect to read from file
                stream_set_type(StreamType_File, file.handle);              // ...
                if(hal.stream.suspend_read)                                 // If active stream support tool change suspend
                    hal.stream.suspend_read = stream_suspend;               // then we do as well
                else                                                        //
                    hal.stream.suspend_read = NULL;                         // else not.

                if((file.scan_subs = state != STATE_CHECK_MODE && settings.flags.m98_prescan_enable))
                    state_set(STATE_CHECK_MODE);

                on_program_completed = grbl.on_program_completed;
                grbl.on_program_completed = onProgramCompleted;

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
        }
    } else
        retval = Status_FileOpenFailed;

    return retval;
}

static status_code_t cmd_file_filtered (sys_state_t state, char *args)
{
    status_code_t retval = Status_Unhandled;

    if(args)
        retval = stream_file(state, args);

    else {
        frewind = false;
        retval = list_files(true); // (re)use line buffer for reporting filenames
    }

    return retval;
}

static status_code_t cmd_file_all (sys_state_t state, char *args)
{
    status_code_t retval = Status_Unhandled;

    if(args)
        retval = stream_file(state, args);

    else {
        frewind = false;
        retval = list_files(false); // (re)use line buffer for reporting filenames
    }

    return retval;
}

static status_code_t cmd_rewind (sys_state_t state, char *args)
{
    frewind = true;

    return Status_OK;
}

static status_code_t sd_cmd_to_output (sys_state_t state, char *args)
{
    status_code_t retval = Status_Unhandled;

    if(!fs.mounted)
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

static status_code_t cmd_unlink (sys_state_t state, char *args)
{
    status_code_t retval = Status_Unhandled;

    if(!fs.mounted)
        retval = Status_SDNotMounted;
    else if(fs.mode.read_only)
        retval = Status_FsReadOnly;
    else if(!(state == STATE_IDLE || state == STATE_CHECK_MODE))
        retval = Status_SystemGClock;
    else if(args)
        retval = vfs_unlink(args) ? Status_FileReadError : Status_OK;

    return retval;
}

static void onReset (void)
{
    if(hal.stream.type == StreamType_File && active_stream.type != StreamType_Null) {
        if(file.line > 0) {
            char buf[70];
            sprintf(buf, "Reset during streaming of file at line: " UINT32FMT, file.line);
            report_message(buf, Message_Plain);
        } else if(frewind)
            grbl.report.feedback_message(Message_None);
        stream_end_job(true);
    }

    driver_reset();
}

static void onRealtimeReport (stream_write_ptr stream_write, report_tracking_flags_t report)
{
    if(!report.all) {
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
    }

    if(on_realtime_report)
        on_realtime_report(stream_write, report);
}

static void onReportOptions (bool newopt)
{
    if(on_report_options)
        on_report_options(newopt);

    if(newopt) {
#if FS_ENABLE & FS_YMODEM
  #if (FS_ENABLE & FS_SDCARD) && FF_FS_READONLY == 0
        if(hal.stream.write_char)
            hal.stream.write(",YM");
  #elif FS_ENABLE & FS_LFS_ROOT
        hal.stream.write(hal.stream.write_char == NULL ? ",FS" : ",FS,YM");
  #endif
#else
        hal.stream.write(",FS");
#endif
    } else
        report_plugin("FS stream", "1.04");
}

static void onFsUnmount (const char *path)
{
    if(path[0] == '/' && path[1] == '\0')
        fs.mounted = false;

    if(on_vfs_unmount)
        on_vfs_unmount(path);
}

static void onFsMount (const char *path, const vfs_t *vfs, vfs_st_mode_t mode)
{
    if(path[0] == '/' && path[1] == '\0') {

        fs.mounted = true;
        fs.mode = mode;

        if(driver_reset == NULL) {

            driver_reset = hal.driver_reset;
            hal.driver_reset = onReset;

            on_realtime_report = grbl.on_realtime_report;
            grbl.on_realtime_report = onRealtimeReport;
        }
    }

    if(on_vfs_mount)
        on_vfs_mount(path, vfs, mode);
}

void fs_stream_init (void)
{
    PROGMEM static const sys_command_t sdcard_command_list[] = {
        {"F", cmd_file_filtered, {}, {
            .str = "list files, filtered"
         ASCII_EOL "$F=<filename> - run file"
        } },
        {"F+", cmd_file_all, {}, { .str = "$F+ - list all files" } },
        {"FR", cmd_rewind, { .noargs = On }, { .str = "enable rewind mode for next file to run" } },
    #if FF_FS_READONLY == 0 && FF_FS_MINIMIZE == 0
        {"FD", cmd_unlink, {}, { .str = "$FD=<filename> - delete file" } },
    #endif
        {"F<", sd_cmd_to_output, {}, { .str = "$F<=<filename> - dump file to output" } },
    };

    static sys_commands_t sdcard_commands = {
        .n_commands = sizeof(sdcard_command_list) / sizeof(sys_command_t),
        .commands = sdcard_command_list
    };

    PROGMEM static const status_detail_t status_detail[] = {
        { Status_FileReadError, "File delete failed." },
        { Status_FsFailedOpenDir, "Directory listing failed." },
        { Status_FSDirNotFound, "Directory not found." },
        { Status_SDNotMounted, "SD Card not mounted." },
        { Status_FsNotMounted, "File system not mounted." },
        { Status_FsReadOnly, "File system is read only." },
        { Status_FsFormatFailed, "File system format failed." }
    };

    static error_details_t error_details = {
        .errors = status_detail,
        .n_errors = sizeof(status_detail) / sizeof(status_detail_t)
    };

    active_stream.type = StreamType_Null;

    on_report_options = grbl.on_report_options;
    grbl.on_report_options = onReportOptions;

    on_vfs_mount = vfs.on_mount;
    vfs.on_mount = onFsMount;

    on_vfs_unmount = vfs.on_unmount;
    vfs.on_unmount = onFsUnmount;

    errors_register(&error_details);
    system_register_commands(&sdcard_commands);

#if (FS_ENABLE & FS_YMODEM) && (((FS_ENABLE & FS_SDCARD) && FF_FS_READONLY == 0) || (FS_ENABLE & FS_LFS_ROOT))
    if(hal.stream.write_char != NULL)
        ymodem_init();
#endif

    fs_macros_init();
}

bool fs_busy (void)
{
    return stream_is_file();
}

stream_job_t *stream_get_job_info (void)
{
    static stream_job_t job;

    if(stream_is_file()) {
        strcpy(job.name, file.name);
        job.size = file.size;
        job.pos = file.pos;
        job.line = file.line;
    }

    return stream_is_file() ? &job : NULL;
}

#endif // FS_ENABLE
