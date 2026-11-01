Modified copy of `../freertos-arm926-port/` (jkovacic/FreeRTOS-GCC-ARM926ejs @ aa1507f, MIT).
Only `portISR.c` differs (vTickISR: inline `BL` without lr clobber replaced by C calls; see the file header).
The unmodified original stays in `../freertos-arm926-port/` (checksums in `../SHA256SUMS`).
