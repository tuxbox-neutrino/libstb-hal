#include <config.h>
#if HAVE_ARM_HARDWARE
#include "../libarmbox/playback_libeplayer3.h"
#elif HAVE_MIPS_HARDWARE
#include "../libmipsbox/playback_libeplayer3.h"
#elif HAVE_GENERIC_HARDWARE
#include "../libgeneric-pc/playback_lib.h"
#else
#error no valid hardware defined
#endif
