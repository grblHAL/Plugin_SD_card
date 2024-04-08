/*
  macros.c - plugin for executing macros stored in a file system

  Part of grblHAL SD card plugins

  Copyright (c) 2023-2024 Terje Io

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

#ifdef ARDUINO
#include "../driver.h"
#else
#include "driver.h"
#endif

#if SDCARD_ENABLE || LITTLEFS_ENABLE

#include <stdio.h>
#include <string.h>

#include "grbl/hal.h"
#include "grbl/state_machine.h"
#include "grbl/tool_change.h"
#include "grbl/ngc_flowctrl.h"

#ifndef MACRO_STACK_DEPTH
#define MACRO_STACK_DEPTH 1 // for now
#endif

typedef struct {
    macro_id_t id;
    vfs_file_t *file;
//    uint32_t line;
} macro_stack_entry_t;

static volatile int_fast16_t stack_idx = -1;
static macro_stack_entry_t macro[MACRO_STACK_DEPTH] = {0};
static on_report_options_ptr on_report_options;
static on_macro_execute_ptr on_macro_execute;
static on_macro_return_ptr on_macro_return = NULL;
static status_message_ptr status_message = NULL;
static driver_reset_ptr driver_reset;
static io_stream_t active_stream;

static int16_t file_read (void);
static status_code_t trap_status_messages (status_code_t status_code);

#if NGC_EXPRESSIONS_ENABLE
static on_vfs_mount_ptr on_vfs_mount;
static on_vfs_unmount_ptr on_vfs_unmount;
static pallet_shuttle_ptr on_pallet_shuttle;
static char tc_path[15];
#endif

// Ends macro execution if currently running
// and restores normal operation.
static void end_macro (void)
{
    if(stack_idx >= 0) {
        if(macro[stack_idx].file) {
            vfs_close(macro[stack_idx].file);
            ngc_flowctrl_unwind_stack(macro[stack_idx].file);
            macro[stack_idx].file = NULL;
        }
        stack_idx--;
        hal.stream.file = stack_idx >= 0 ? macro[stack_idx].file : NULL;
    }

    if(stack_idx == -1) {

        if(hal.stream.read == file_read)
            memcpy(&hal.stream, &active_stream, sizeof(io_stream_t));

        grbl.on_macro_return = on_macro_return;
        on_macro_return = NULL;

        if(grbl.report.status_message == trap_status_messages)
            grbl.report.status_message = status_message;

        status_message = NULL;
    }
}

// Called on a soft reset so that normal operation can be restored.
static void plugin_reset (void)
{
    while(stack_idx >= 0)
        end_macro();

    driver_reset();
}

// Macro stream input function.
// Reads character by character from the macro and returns them when
// requested by the foreground process.
static int16_t file_read (void)
{
    static bool eol_ok = false;

    char c;

    if(vfs_read(&c, 1, 1, macro[stack_idx].file) == 1) {
        if(c == ASCII_CR || c == ASCII_LF) {
            if(eol_ok)
                return SERIAL_NO_DATA;
            eol_ok = true;
        } else
            eol_ok = false;
    } else if(eol_ok) {
        if(status_message)
            status_message(gc_state.last_error);
        end_macro();            // Done
        return SERIAL_NO_DATA;  // ...
    } else {
        eol_ok = true;
        return ASCII_LF;        // Return a linefeed if the last character was not a linefeed.
    }

    return (int16_t)c;
}

// This code will be executed after each command is sent to the parser,
// If an error is detected macro execution will be stopped and the status_code reported.
static status_code_t trap_status_messages (status_code_t status_code)
{
    gc_state.last_error = status_code;

    if(hal.stream.read != file_read)
        status_code = status_message(status_code);

    else if(!(status_code == Status_OK || status_code == Status_Unhandled)) {

        char msg[40];
        sprintf(msg, "error %d in macro P%d.macro", (uint8_t)status_code, macro[stack_idx].id);
        report_message(msg, Message_Warning);

        if(grbl.report.status_message == trap_status_messages && (grbl.report.status_message = status_message))
            status_code = grbl.report.status_message(status_code);

        while(stack_idx >= 0)
            end_macro();
    }

    return status_code;
}

static void macro_start (vfs_file_t *file, macro_id_t macro_id)
{
    stack_idx++;
    macro[stack_idx].file = file;
    macro[stack_idx].id = macro_id;

    if(hal.stream.read != file_read) {

        memcpy(&active_stream, &hal.stream, sizeof(io_stream_t));   // Redirect input stream to read from the macro instead
        hal.stream.type = StreamType_File;                          // from the active stream.
        hal.stream.read = file_read;                                // This ensures that input streams are not mingled.

        status_message = grbl.report.status_message;                // Add trap for status messages
        grbl.report.status_message = trap_status_messages;          // so we can terminate on errors.

        on_macro_return = grbl.on_macro_return;
        grbl.on_macro_return = end_macro;
    }

    hal.stream.file = file;
}

static status_code_t macro_execute (macro_id_t macro_id)
{
    bool ok = false;

    if(stack_idx < (MACRO_STACK_DEPTH - 1) && macro_id >= 100 && state_get() == STATE_IDLE) {

        char filename[32];
        vfs_file_t *file;

#if LITTLEFS_ENABLE
        sprintf(filename, "/littlefs/P%d.macro", macro_id);

        if((file = vfs_open(filename, "r")) == NULL)
#endif
        {
            sprintf(filename, "/P%d.macro", macro_id);

            file = vfs_open(filename, "r");
        }

        if((ok = !!file))
            macro_start(file, macro_id);
    }

    return ok ? Status_OK : (on_macro_execute ? on_macro_execute(macro_id) : Status_Unhandled);
}

#if NGC_EXPRESSIONS_ENABLE

// Set next and/or current tool. Called by gcode.c on on a Tn or M61 command (via HAL).
static void tool_select (tool_data_t *tool, bool next)
{
    vfs_file_t *file;
    char filename[30];

    if(tool->tool_id > 0 && (file = vfs_open(strcat(strcpy(filename, tc_path), "ts.macro"), "r")))
        macro_start(file, 98);
}

static status_code_t tool_change (parser_state_t *parser_state)
{
    vfs_file_t *file;
    char filename[30];
    int32_t current_tool = (int32_t)ngc_named_param_get_by_id(NGCParam_current_tool),
            next_tool =  (int32_t)ngc_named_param_get_by_id(NGCParam_selected_tool);

    if(next_tool == -1)
        return Status_GCodeToolError;

    if(current_tool == next_tool || next_tool == 0)
        return Status_OK;

    if((file = vfs_open(strcat(strcpy(filename, tc_path), "tc.macro"), "r")))
        macro_start(file, 99);
    else
        return Status_GCodeToolError;

    return Status_Unhandled;
}

// Perform a pallet shuttle.
static void pallet_shuttle (void)
{
    vfs_file_t *file;
    char filename[30];

    if((file = vfs_open(strcat(strcpy(filename, tc_path), "ps.macro"), "r")))
        macro_start(file, 97);

    if(on_pallet_shuttle)
        on_pallet_shuttle();
}

static void atc_path_fix (char *path)
{
    path = strchr(path, '\0') - 1;
    if(*path != '/')
        strcat(path, "/");
}

static void atc_macros_attach (const char *path, const vfs_t *fs)
{
    vfs_stat_t st;
    char filename[30];

    if(!hal.driver_cap.atc) {

        strcpy(tc_path, path);
        atc_path_fix(tc_path);

        if(vfs_stat(strcat(strcpy(filename, tc_path), "tc.macro"), &st) == 0) {
            hal.driver_cap.atc = On;
            hal.tool.select = tool_select;
            hal.tool.change = tool_change;
        }
    }

    if(on_pallet_shuttle == NULL && vfs_stat(strcat(strcpy(filename, tc_path), "ps.macro"), &st) == 0) {

        strcpy(tc_path, path);
        atc_path_fix(tc_path);

        on_pallet_shuttle = hal.pallet_shuttle;
        hal.pallet_shuttle = pallet_shuttle;
    }

    if(on_vfs_mount)
        on_vfs_mount(path, fs);
}

static void atc_macros_detach (const char *path)
{
    char tc_path[15];

    if(hal.tool.select == tool_select) {

        strcpy(tc_path, path);
        atc_path_fix(tc_path);

        if(!strcmp(path, tc_path)) {
            hal.driver_cap.atc = Off;
            hal.tool.select = NULL;
            hal.tool.change = NULL;
            tc_init();
        }
    }

    if(hal.pallet_shuttle == pallet_shuttle) {
        hal.pallet_shuttle = on_pallet_shuttle;
        on_pallet_shuttle = NULL;
    }

    if(on_vfs_unmount)
        on_vfs_unmount(path);
}

#endif // NGC_EXPRESSIONS_ENABLE

// Add info about our plugin to the $I report.
static void report_options (bool newopt)
{
    on_report_options(newopt);

    if(!newopt)
        hal.stream.write("[PLUGIN:FS macro plugin v0.07]" ASCII_EOL);
}

void fs_macros_init (void)
{
    on_report_options = grbl.on_report_options;
    grbl.on_report_options = report_options;

    on_macro_execute = grbl.on_macro_execute;
    grbl.on_macro_execute = macro_execute;

    driver_reset = hal.driver_reset;
    hal.driver_reset = plugin_reset;

#if NGC_EXPRESSIONS_ENABLE

    on_vfs_mount = vfs.on_mount;
    vfs.on_mount = atc_macros_attach;

    on_vfs_unmount = vfs.on_unmount;
    vfs.on_unmount = atc_macros_detach;

#endif
}

#endif // SDCARD_ENABLE || LITTLEFS_ENABLE
