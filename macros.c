/*
  macros.c - plugin for executing macros stored in a file system

  Part of grblHAL SD card plugins

  Copyright (c) 2023 Terje Io

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
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

static macro_id_t macro_id = 0;
static vfs_file_t *file = NULL;
static on_report_options_ptr on_report_options;
static on_macro_execute_ptr on_macro_execute;
static on_macro_return_ptr on_macro_return = NULL;
static status_message_ptr status_message = NULL;
static stream_read_ptr stream_read;
static driver_reset_ptr driver_reset;

static int16_t file_read (void);
static status_code_t trap_status_messages (status_code_t status_code);

// Ends macro execution if currently running
// and restores normal operation.
static void end_macro (void)
{
    if(hal.stream.read == file_read)
        hal.stream.read = stream_read;

    if(file) {

        vfs_close(file);
        file = NULL;

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
    end_macro();    // End macro if currently running.
    driver_reset(); // Call the next reset handler in the chain.
}

// Macro stream input function.
// Reads character by character from the macro and returns them when
// requested by the foreground process.
static int16_t file_read (void)
{
    static bool eol_ok = false;

    char c;

    if(vfs_read(&c, 1, 1, file) == 1) {
        if(c == ASCII_CR || c == ASCII_LF) {
            if(eol_ok)
                return SERIAL_NO_DATA;
            eol_ok = true;
        } else
            eol_ok = false;
    } else if(eol_ok) {
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
    if(hal.stream.read != file_read)
        status_code = status_message(status_code);

    else if(status_code != Status_OK) {

        char msg[40];
        sprintf(msg, "error %d in macro P%d.macro", (uint8_t)status_code, macro_id);
        report_message(msg, Message_Warning);

        hal.stream.read = stream_read; // restore origial input stream

        if(grbl.report.status_message == trap_status_messages && (grbl.report.status_message = status_message))
            status_code = grbl.report.status_message(status_code);

        end_macro();
    }

    return status_code;
}

static status_code_t macro_execute (macro_id_t macro)
{
    bool ok = false;

    if(file == NULL && macro >= 100 && state_get() == STATE_IDLE) {

        char filename[32];

#if LITTLEFS_ENABLE
        sprintf(filename, "/littlefs/P%d.macro", macro);

        if((file = vfs_open(filename, "r")) == NULL)
#endif
        {
            sprintf(filename, "/P%d.macro", macro);

            file = vfs_open(filename, "r");
        }

        if((ok = !!file)) {

            macro_id = macro;

            stream_read = hal.stream.read;                      // Redirect input stream to read from the macro instead of
            hal.stream.read = file_read;                        // the active stream. This ensures that input streams are not mingled.

            status_message = grbl.report.status_message;        // Add trap for status messages
            grbl.report.status_message = trap_status_messages;  // so we can terminate on errors.

            on_macro_return = grbl.on_macro_return;
            grbl.on_macro_return = end_macro;
        }
    }

    return ok ? Status_OK : (on_macro_execute ? on_macro_execute(macro) : Status_Unhandled);
}

// Add info about our plugin to the $I report.
static void report_options (bool newopt)
{
    on_report_options(newopt);

    if(!newopt)
        hal.stream.write("[PLUGIN:FS macro plugin v0.01]" ASCII_EOL);
}

void fs_macros_init (void)
{
    on_report_options = grbl.on_report_options;
    grbl.on_report_options = report_options;

    on_macro_execute = grbl.on_macro_execute;
    grbl.on_macro_execute = macro_execute;

    driver_reset = hal.driver_reset;
    hal.driver_reset = plugin_reset;
}

#endif // SDCARD_ENABLE || LITTLEFS_ENABLE
