## File system (SD card and littlefs) and related plugins

#### SD card

The SD card plugin adds a few `$` commands for listing files and running G-code from a SD Card:

`$FM`

Mount card as the root \(/\) file system.

`$F`

List files on the card recursively. Only CNC related filetypes are listed: _.nc_, _.ncc_, _.ngc_, _.cnc_, _.gcode_, _.txt_, _.text_, .tap_ and _.macro_.

`$F+`

List all files on the card recursively regardless of filetype.


`$F=<filename>`

Run g-code in file. If the file ends with `M2` or rewind mode is active then it will be "rewound" and a cycle start command will start it again.

`$FR`

Enable rewind mode for next file to be run.

`$F<=<filename>`

Dump file content to output stream.

`$FD=<filename>`

Delete file.

Dependencies:

[FatFS library](http://www.elm-chan.org/fsw/ff/00index_e.html)

__NOTE:__ some drivers uses ports of FatFS provided by the MCU supplier.

#### YModem

Experimental ymodem upload support to SD card has been added as an option from build 20210422. Enable by setting `SDCARD_ENABLE` to `2` in _my_machine.h_.  
This requires a compatible sender as the initial `C` used to start the transfer will not be sent by the controller.
Transfer is initiated by the sender by sending the first packet.

#### littlefs

[littlefs](https://github.com/littlefs-project/littlefs) is a flash based file system that some drivers support.
When available it will typically be enabled along the [WebUI plugin](https://github.com/grblHAL/Plugin_WebUI) and mounted in the `/littlefs` directory.
It can be used for storing the WebUI application and its configurations, and may also be used to store `G65` and tool change macros.

To write files to littlefs either use the WebUI maintenance page (`forcefallback` enabled) or ftp transfer.

#### Macros

The macros plugin is handling `G65` calls by either redirecting the input stream to the file containg the macro or
executing an [inbuilt macro](https://github.com/grblHAL/core/wiki/Expressions-and-flow-control#inbuilt-g65-macros).  

Syntax is `G65P<n>` where `<n>` is the macro number.  
If `<n>` is >= 100 `/littlefs/P<n>.macro`, or `/P<n>.macro` if not present in littlefs, will be executed.  
If `<n>` is < 100 it is considered to be an inbuilt macro of which [a few](https://github.com/grblHAL/core/wiki/Expressions-and-flow-control#inbuilt-g65-macros) are currently implemented.

[Parameters](https://github.com/grblHAL/core/wiki/Expressions-and-flow-control#numbered-parameters-passed-as-arguments-to-g65-macro-call)
can be passed to macros if [expression](https://github.com/grblHAL/core/wiki/Expressions-and-flow-control#inbuilt-g65-macros) support is enabled.

If the macro `/littlefs/tc.macro` or `/tc.macro` is present then the plugin will attach itself to the tool change HAL functions.
`tc.macro` will then be called when a `M6` command is executed.
If `/littlefs/ts.macro` or `/ts.macro` is also present it will be called when a `T` command is executed,
this can be used to preposition a tool carousel or whatever else is needed to prepare for a tool change.

Similarly if `/littlefs/ps.macro` or `/ps.macro` is present it will be called when a `M30` or a `M60` \(pallet shuttle\) command is executed.

`tc.macro` and `ts.macro` can be used to add support for automatic tool changers \(ATCs\), [this discussion](https://github.com/grblHAL/core/discussions/577) contains examples and ideas for of how to.

---
2024-09-25
