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

#ifndef MACRO_STACK_DEPTH
#define MACRO_STACK_DEPTH 5
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
static driver_reset_ptr driver_reset;

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
            stream_redirect_close(macro[stack_idx].file);
#if NGC_EXPRESSIONS_ENABLE
            ngc_flowctrl_unwind_stack(macro[stack_idx].file);
#endif
            ngc_call_pop();
            macro[stack_idx].file = NULL;
        }
        stack_idx--;
    }

    if(stack_idx == -1) {
        grbl.on_macro_return = on_macro_return;
        on_macro_return = NULL;
    }
}

// Called on a soft reset so that normal operation can be restored.
static void plugin_reset (void)
{
    while(stack_idx >= 0)
        end_macro();

    driver_reset();
}

static status_code_t onG65MacroError (status_code_t status_code)
{
    if(stack_idx >= 0) {

        char msg[40];
        sprintf(msg, "error %d in macro P%d.macro", (uint8_t)status_code, macro[stack_idx].id);
        report_message(msg, Message_Warning);

        end_macro();
        grbl.report.status_message(status_code);
    }

    return status_code;
}

static status_code_t onG65MacroEOF (vfs_file_t *file, status_code_t status)
{
    if(stack_idx >= 0 && macro[stack_idx].file == file) {
        if(status == Status_OK) {
            end_macro();
            grbl.report.status_message(status);
        } else {
            while(stack_idx >= 0)
                end_macro();
        }
    }

    return status;
}

static status_code_t macro_start (char *filename, macro_id_t macro_id)
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
            grbl.on_macro_return = end_macro;
        }

        stack_idx++;
        macro[stack_idx].file = file;
        macro[stack_idx].id = macro_id;
    }

    return Status_Handled;
}

static status_code_t macro_execute (macro_id_t macro_id)
{
    status_code_t status = Status_Unhandled;

    if(macro_id >= 100) {
        if(stack_idx >= (MACRO_STACK_DEPTH - 1))
            status = Status_FlowControlStackOverflow;

    //    else if(state_get() != STATE_IDLE)
    //        status = Status_IdleError;

        else {

            char filename[32];

    #if LITTLEFS_ENABLE == 1
            sprintf(filename, "/littlefs/P%d.macro", macro_id);

            if((status = macro_start(filename, macro_id)) != Status_Handled)
    #endif
            {
                sprintf(filename, "/P%d.macro", macro_id);

                status = macro_start(filename, macro_id);
            }
        }
    }

    return status == Status_Unhandled && on_macro_execute ? on_macro_execute(macro_id) : status;
}

#if NGC_EXPRESSIONS_ENABLE

// Set next and/or current tool. Called by gcode.c on on a Tn or M61 command (via HAL).
static void macro_tool_select (tool_data_t *tool, bool next)
{
    char filename[30];

    if(tool->tool_id > 0)
        macro_start(strcat(strcpy(filename, tc_path), "ts.macro"), 98);
}

static status_code_t macro_tool_change (parser_state_t *parser_state)
{
    char filename[30];
    int32_t current_tool = (int32_t)ngc_named_param_get_by_id(NGCParam_current_tool),
            next_tool =  (int32_t)ngc_named_param_get_by_id(NGCParam_selected_tool);

    if(next_tool == -1)
        return Status_GCodeToolError;

    if(current_tool == next_tool || (!settings.macro_atc_flags.execute_m6t0 && next_tool == 0))
        return Status_OK;

    status_code_t status = macro_start(strcat(strcpy(filename, tc_path), "tc.macro"), 99);

    return status == Status_Handled ? Status_Unhandled : status;
}

// Perform a pallet shuttle.
static void macro_pallet_shuttle (void)
{
    char filename[30];

    macro_start(strcat(strcpy(filename, tc_path), "ps.macro"), 97);

    if(on_pallet_shuttle)
        on_pallet_shuttle();
}

static void atc_path_fix (char *path)
{
    path = strchr(path, '\0') - 1;
    if(*path != '/')
        strcat(path, "/");
}

static bool is_setting_available (const setting_detail_t *setting, uint_fast16_t offset)
{
    return hal.tool.change == macro_tool_change;
}

static const setting_detail_t macro_settings[] = {
    { Setting_MacroATC_Options, Group_Toolchange, "Macro ATC options", NULL, Format_Bitfield, "Execute M6T0", NULL, NULL, Setting_IsExtended, &settings.macro_atc_flags.value, NULL, is_setting_available },
};

#ifndef NO_SETTINGS_DESCRIPTIONS
static const setting_descr_t macro_settings_descr[] = {
    { Setting_MacroATC_Options, "Options for ATC macros." }
};
#endif

static void macro_settings_restore (void)
{
    settings.macro_atc_flags.value = 0;
}

static void atc_macros_attach (const char *path, const vfs_t *fs, vfs_st_mode_t mode)
{
    static bool settings_registered = false;

    static setting_details_t macro_setting_details = {
        .is_core = true,
        .settings = macro_settings,
        .n_settings = sizeof(macro_settings) / sizeof(setting_detail_t),
    #ifndef NO_SETTINGS_DESCRIPTIONS
        .descriptions = macro_settings_descr,
        .n_descriptions = sizeof(macro_settings_descr) / sizeof(setting_descr_t),
    #endif
        .restore = macro_settings_restore,
        .save = settings_write_global
    };

    vfs_stat_t st;
    char filename[30];

    if(!hal.driver_cap.atc) {

        strcpy(tc_path, path);
        atc_path_fix(tc_path);

        if(vfs_stat(strcat(strcpy(filename, tc_path), "tc.macro"), &st) == 0) {

            hal.driver_cap.atc = On;
            hal.tool.select = macro_tool_select;
            hal.tool.change = macro_tool_change;

            if(!settings_registered) {
                settings_registered = true;
                settings_register(&macro_setting_details);
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

    if(hal.tool.select == macro_tool_select) {

        strcpy(tc_path, path);
        atc_path_fix(tc_path);

        if(!strcmp(path, tc_path)) {
            hal.driver_cap.atc = Off;
            hal.tool.select = NULL;
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
        report_plugin("FS macro plugin", "0.18");
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

    on_vfs_mount = vfs.on_mount;
    vfs.on_mount = atc_macros_attach;

    on_vfs_unmount = vfs.on_unmount;
    vfs.on_unmount = atc_macros_detach;

#endif
}

#endif // FS_ENABLE
