# Third-party code (unmodified copies)

Source: https://github.com/jkovacic/FreeRTOS-GCC-ARM926ejs, commit aa1507f (2020-09-21).

| Directory | Files | Origin in that repository | License |
|---|---|---|---|
| `freertos-kernel/` | tasks.c, queue.c, list.c, timers.c, heap_4.c, include/*.h, LICENSE.md, README.md | `FreeRTOS/Source/` (FreeRTOS Kernel V10.4.0, Amazon.com) | MIT (file headers + LICENSE.md) |
| `freertos-arm926-port/` | port.c, portISR.c, portmacro.h, tick_timer_settings.h | `FreeRTOS/Source/portable/GCC/ARM926EJ-S/` (Jernej Kovacic, based on the FreeRTOS GCC/ARM7_LPC2000 port) | MIT (file headers + LICENSE.txt) |

The files are kept byte-identical to that commit (see `SHA256SUMS`). The Versatile-board drivers of that repository
(PL190 VIC, SP804 timer, PL011 UART) and its demo/startup are **not** used; `g3pre/` provides new PV implementations
of the driver functions the port calls.
