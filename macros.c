/*
  macros.c - plugin for executing macros stored in a file system

  Part of grblHAL SD card plugins

  Copyright (c) 2023-2025 Terje Io

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

#include "driver.h"

#if FS_ENABLE

#include <stdio.h>
#include <string.h>

#include "grbl/hal.h"
#include "grbl/state_machine.h"
#include "grbl/tool_change.h"
#include "grbl/ngc_flowctrl.h"
#include "grbl/stream_file.h"
#include "grbl/task.h"

#ifndef MACRO_STACK_DEPTH
#define MACRO_STACK_DEPTH 5
#endif

typedef struct {
    macro_id_t id;
    uint32_t repeats;
    vfs_file_t *file;
} macro_stack_entry_t;

static volatile int_fast16_t stack_idx = -1;
static macro_stack_entry_t macro[MACRO_STACK_DEPTH] = {0};
static on_report_options_ptr on_report_options;
static on_macro_execute_ptr on_macro_execute;
static on_macro_return_ptr on_macro_return = NULL;
static driver_reset_ptr driver_reset;

#if NGC_EXPRESSIONS_ENABLE
static on_vfs_mount_ptr on_vfs_mount;
static on_vfs_unmount_ptr on_vfs_unmount;
static tool_select_ptr tool_select;
static pallet_shuttle_ptr on_pallet_shuttle;
static char tc_path[15];
#endif

static void macro_exit (void);

// Ends macro execution if currently running
// and restores normal operation.
static bool end_macro (bool failed)
{
    if(stack_idx >= 0) {
        if(macro[stack_idx].file) {

            if(!failed && --macro[stack_idx].repeats) {
                vfs_seek(macro[stack_idx].file, 0);
                return false;
            }

            stream_redirect_close(macro[stack_idx].file);
#if NGC_EXPRESSIONS_ENABLE
            ngc_flowctrl_unwind_stack(macro[stack_idx].file);
#endif
#if NGC_PARAMETERS_ENABLE
            if(macro[stack_idx].id >= 100)
                ngc_call_pop();
#endif
            macro[stack_idx].file = NULL;
        }
        stack_idx--;
    }

    if(stack_idx == -1) {
        grbl.on_macro_return = macro_exit;
        on_macro_return = NULL;
    }

    return true;
}

// Called on a soft reset so that normal operation can be restored.
static void plugin_reset (void)
{
    while(stack_idx >= 0)
        end_macro(true);

    driver_reset();
}

static status_code_t onG65MacroError (status_code_t status_code)
{
    if(stack_idx >= 0) {

        char msg[40];
        sprintf(msg, "error %d in macro P%d.macro", (uint8_t)status_code, macro[stack_idx].id);
        report_message(msg, Message_Warning);

        end_macro(true);
        grbl.report.status_message(status_code);
    }

    return status_code;
}

static status_code_t onG65MacroEOF (vfs_file_t *file, status_code_t status)
{
    if(stack_idx >= 0 && macro[stack_idx].file == file) {
        if(status == Status_OK) {
            if(end_macro(false))
                grbl.report.status_message(status);
        } else {
            while(stack_idx >= 0)
                end_macro(true);
        }
    }

    return status;
}

static status_code_t macro_start (char *filename, macro_id_t macro_id, uint32_t repeats)
{
    vfs_file_t *file;

    if(stack_idx >= (MACRO_STACK_DEPTH - 1))
        return Status_FlowControlStackOverflow;

    if(state_get() == STATE_CHECK_MODE) {

        vfs_stat_t st;

        return vfs_stat(filename, &st) == 0 ? Status_OK : Status_FileOpenFailed;

    } else {

        if((file = stream_redirect_read(filename, onG65MacroError, onG65MacroEOF)) == NULL)
            return Status_FileOpenFailed;

        if(stack_idx == -1) {
            on_macro_return = grbl.on_macro_return;
            grbl.on_macro_return = macro_exit;
        }

        stack_idx++;
        macro[stack_idx].file = file;
        macro[stack_idx].id = macro_id;
        macro[stack_idx].repeats = repeats;
    }

    return Status_Handled;
}

static void macro_exit (void)
{
    if(stack_idx >= 0)
        end_macro(false);
    else if(on_macro_return)
        on_macro_return();
}

static status_code_t macro_execute (macro_id_t macro_id, parameter_words_t args, uint32_t repeats)
{
    status_code_t status = Status_Unhandled;

    if(macro_id >= 100) {

        char filename[32];

#if LITTLEFS_ENABLE == 1
        sprintf(filename, "/littlefs/P%d.macro", macro_id);

        if((status = macro_start(filename, macro_id, repeats)) != Status_Handled)
#endif
        {
            sprintf(filename, "/P%d.macro", macro_id);

            status = macro_start(filename, macro_id, repeats);
        }
    }

    return status == Status_Unhandled && on_macro_execute ? on_macro_execute(macro_id, args, repeats) : status;
}

#if NGC_EXPRESSIONS_ENABLE

static status_code_t macro_tool_change (parser_state_t *parser_state)
{
    char filename[30];
    int32_t current_tool = (int32_t)ngc_named_param_get_by_id(NGCParam_current_tool),
            next_tool = (int32_t)ngc_named_param_get_by_id(NGCParam_selected_tool);

    if(next_tool == -1)
        return Status_GCodeToolError;

    if(current_tool == next_tool || (!settings.macro_atc_flags.execute_m6t0 && next_tool == 0))
        return Status_OK;

    status_code_t status = macro_start(strcat(strcpy(filename, tc_path), "tc.macro"), 99, 1);

    return status == Status_Handled ? Status_Unhandled : status;
}

// Set next and/or current tool. Called by gcode.c on on a Tn or M61 command (via HAL).
static void macro_tool_select (tool_data_t *tool, bool next)
{
    char filename[30];

    if(tool_select)
        tool_select(tool, next);

    if(hal.tool.change == macro_tool_change && tool->tool_id > 0)
        macro_start(strcat(strcpy(filename, tc_path), "ts.macro"), 98, 1);
}

// Perform a pallet shuttle.
static void macro_pallet_shuttle (void)
{
    char filename[30];

    macro_start(strcat(strcpy(filename, tc_path), "ps.macro"), 97, 1);

    if(on_pallet_shuttle)
        on_pallet_shuttle();
}

static void atc_path_fix (char *path)
{
    path = strchr(path, '\0') - 1;
    if(*path != '/')
        strcat(path, "/");
}

static void macro_settings_restore (void)
{
    settings.macro_atc_flags.value = 0;
}

static atc_status_t atc_get_state (void)
{
    return hal.tool.change == macro_tool_change // TODO: recheck for tc.macro available?
            ? ATC_Online
            : (settings.macro_atc_flags.error_on_no_macro ? ATC_Offline : ATC_None);
}

static void atc_check (void *data)
{
    if(settings.macro_atc_flags.error_on_no_macro)
        hal.tool.atc_get_state = atc_get_state;
}

static void atc_macros_attach (const char *path, const vfs_t *fs, vfs_st_mode_t mode)
{
    static bool select_claimed = false;

    vfs_stat_t st;
    char filename[30];

    if(!hal.driver_cap.atc) {

        strcpy(tc_path, path);
        atc_path_fix(tc_path);

        if(vfs_stat(strcat(strcpy(filename, tc_path), "tc.macro"), &st) == 0) {

            hal.driver_cap.atc = On;
            hal.tool.change = macro_tool_change;
            hal.tool.atc_get_state = atc_get_state;

            if(!select_claimed) {
                select_claimed = true;
                tool_select = hal.tool.select;
                hal.tool.select = macro_tool_select;
            }
        }
    }

    if(on_pallet_shuttle == NULL && vfs_stat(strcat(strcpy(filename, tc_path), "ps.macro"), &st) == 0) {

        strcpy(tc_path, path);
        atc_path_fix(tc_path);

        on_pallet_shuttle = hal.pallet_shuttle;
        hal.pallet_shuttle = macro_pallet_shuttle;
    }

    if(on_vfs_mount)
        on_vfs_mount(path, fs, mode);
}

static void atc_macros_detach (const char *path)
{
    char tc_path[15];

    if(hal.tool.change == macro_tool_change) {

        strcpy(tc_path, path);
        atc_path_fix(tc_path);

        if(!strcmp(path, tc_path)) {
            hal.driver_cap.atc = Off;
            hal.tool.change = NULL;
            tc_init();
        }
    }

    if(hal.pallet_shuttle == macro_pallet_shuttle) {
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
        report_plugin("FS macro plugin", "0.21");
}

void fs_macros_init (void)
{
    if(on_report_options)
        return;

    on_report_options = grbl.on_report_options;
    grbl.on_report_options = report_options;

    on_macro_execute = grbl.on_macro_execute;
    grbl.on_macro_execute = macro_execute;

    driver_reset = hal.driver_reset;
    hal.driver_reset = plugin_reset;

#if NGC_EXPRESSIONS_ENABLE

    PROGMEM static const setting_detail_t macro_settings[] = {
        { Setting_MacroATC_Options, Group_Toolchange, "Macro ATC options", NULL, Format_Bitfield, "Execute M6T0,Fail M6 if tc.macro not found", NULL, NULL, Setting_IsExtended, &settings.macro_atc_flags.value, NULL },
    };

    PROGMEM static const setting_descr_t macro_settings_descr[] = {
        { Setting_MacroATC_Options, "Options for ATC macros." }
    };

    static setting_details_t macro_setting_details = {
        .is_core = true,
        .settings = macro_settings,
        .n_settings = sizeof(macro_settings) / sizeof(setting_detail_t),
        .descriptions = macro_settings_descr,
        .n_descriptions = sizeof(macro_settings_descr) / sizeof(setting_descr_t),
        .restore = macro_settings_restore,
        .save = settings_write_global
    };

    on_vfs_mount = vfs.on_mount;
    vfs.on_mount = atc_macros_attach;

    on_vfs_unmount = vfs.on_unmount;
    vfs.on_unmount = atc_macros_detach;

    settings_register(&macro_setting_details);

    task_run_on_startup(atc_check, NULL);

#endif
}

#endif // FS_ENABLE
