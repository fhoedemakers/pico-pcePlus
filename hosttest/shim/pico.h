// Host-build shim for the Pico SDK's pico.h. The pce-go core only uses the
// flash-placement macro, which is meaningless on a host build.
#pragma once
#include <stdint.h>

#ifndef __not_in_flash_func
#define __not_in_flash_func(f) f
#endif
#ifndef __time_critical_func
#define __time_critical_func(f) f
#endif
