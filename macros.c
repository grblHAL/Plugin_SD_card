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

static bool macro_start (char *filename, macro_id_t macro_id)
{
    vfs_file_t *file;

    if(stack_idx >= (MACRO_STACK_DEPTH - 1))
        return false;

    if((file = stream_redirect_read(filename, onG65MacroError, onG65MacroEOF)) == NULL)
        return false;

    if(stack_idx == -1) {
        on_macro_return = grbl.on_macro_return;
        grbl.on_macro_return = end_macro;
    }

    stack_idx++;
    macro[stack_idx].file = file;
    macro[stack_idx].id = macro_id;

    return true;
}

#if NGC_PARAMETERS_ENABLE

static status_code_t macro_get_setting (void)
{
    float setting_id;
    status_code_t status = Status_OK;
    const setting_detail_t *setting;
    parameter_words_t args = gc_get_g65_arguments();

    if(!args.q)
        status = Status_GcodeValueWordMissing;
    if(ngc_param_get(17 /* Q word */, &setting_id) && (setting = setting_get_details((setting_id_t)setting_id, NULL))) {

        uint_fast8_t offset = (setting_id_t)setting_id - setting->id;

        if(setting->datatype == Format_Decimal) {
            ngc_named_param_set("_value", setting_get_float_value(setting, offset));
            ngc_named_param_set("_value_returned", 1.0f);
        } else if(setting_is_integer(setting) || setting_is_list(setting)) {
            ngc_named_param_set("_value", (float)setting_get_int_value(setting, offset));
            ngc_named_param_set("_value_returned", 1.0f);
        }
    } else
        status = Status_GcodeValueOutOfRange;

    return status;
}

static status_code_t macro_ngc_parameter_rw (void)
{
    float idx, value;
    status_code_t status = Status_OK;
    parameter_words_t args = gc_get_g65_arguments();

    if(!args.i)
        status = Status_GcodeValueWordMissing;
    else if(ngc_param_get(4 /* I word */, &idx)) {
        if(args.q) {
            if(!(ngc_param_get(17 /* Q word */, &value) && ngc_param_set((ngc_param_id_t)idx, value)))
                status = Status_GcodeValueOutOfRange;
        } else if(ngc_param_get((ngc_param_id_t)idx, &value)) {
            if(args.s) {
                if(!(ngc_param_get(19 /* S word */, &idx) && ngc_param_set((ngc_param_id_t)idx, value)))
                    status = Status_GcodeValueOutOfRange;
            } else {
                ngc_named_param_set("_value", value);
                ngc_named_param_set("_value_returned", 1.0f);
            }
        } else
            status = Status_GcodeValueOutOfRange;
    } else
        status = Status_GcodeValueOutOfRange;

    return status;
}

#if N_TOOLS

static status_code_t macro_get_tool_offset (void)
{
    float tool_id, axis_id;
    status_code_t status = Status_OK;
    parameter_words_t args = gc_get_g65_arguments();

    if(!(args.q && args.r))
        status = Status_GcodeValueWordMissing;
    else if(ngc_param_get(17 /* Q word */, &tool_id) && ngc_param_get(18 /* R word */, &axis_id)) {
        if((uint32_t)tool_id <= grbl.tool_table.n_tools && (uint8_t)axis_id < N_AXIS) {
            ngc_named_param_set("_value", grbl.tool_table.tool[(uint32_t)tool_id].offset[(uint8_t)axis_id]);
            ngc_named_param_set("_value_returned", 1.0f);
        } else
            status = Status_GcodeIllegalToolTableEntry;
    } else
        status = Status_GcodeIllegalToolTableEntry;

    return status;
}

#endif // N_TOOLS

#endif // NGC_PARAMETERS_ENABLE

static status_code_t macro_execute (macro_id_t macro_id)
{
    status_code_t status = Status_Unhandled;

    if(macro_id < 100) {
        switch(macro_id) { // TODO: add enum or defines?
#if NGC_PARAMETERS_ENABLE
            case 1:
                status = macro_get_setting();
                break;

            case 3:
                status = macro_ngc_parameter_rw();
                break;
  #if N_TOOLS
            case 2:
                status = macro_get_tool_offset();
                break;
  #endif
#endif
        }
    } else if(stack_idx < (MACRO_STACK_DEPTH - 1) && state_get() == STATE_IDLE) {

        bool ok;
        char filename[32];

#if LITTLEFS_ENABLE == 1
        sprintf(filename, "/littlefs/P%d.macro", macro_id);

        if(!(ok = macro_start(filename, macro_id)))
#endif
        {
            sprintf(filename, "/P%d.macro", macro_id);

            ok = macro_start(filename, macro_id);
        }

        if(ok)
            status = Status_Handled;
    }

    return status == Status_Unhandled && on_macro_execute ? on_macro_execute(macro_id) : status;
}

#if NGC_EXPRESSIONS_ENABLE

// Set next and/or current tool. Called by gcode.c on on a Tn or M61 command (via HAL).
static void tool_select (tool_data_t *tool, bool next)
{
    char filename[30];

    if(tool->tool_id > 0)
        macro_start(strcat(strcpy(filename, tc_path), "ts.macro"), 98);
}

static status_code_t tool_change (parser_state_t *parser_state)
{
    char filename[30];
    int32_t current_tool = (int32_t)ngc_named_param_get_by_id(NGCParam_current_tool),
            next_tool =  (int32_t)ngc_named_param_get_by_id(NGCParam_selected_tool);

    if(next_tool == -1)
        return Status_GCodeToolError;

    if(current_tool == next_tool || next_tool == 0)
        return Status_OK;

    if(!macro_start(strcat(strcpy(filename, tc_path), "tc.macro"), 99))
        return Status_GCodeToolError;

    return Status_Unhandled;
}

// Perform a pallet shuttle.
static void pallet_shuttle (void)
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
        report_plugin("FS macro plugin", "0.12");
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
