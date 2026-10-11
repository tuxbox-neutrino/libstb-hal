#include <config.h>
#if HAVE_ARM_HARDWARE
#include "../libarmbox/audio_lib.h"
#elif HAVE_MIPS_HARDWARE
#include "../libmipsbox/audio_lib.h"
#elif HAVE_GENERIC_HARDWARE
#include "../libgeneric-pc/audio_lib.h"
#else
#error no valid hardware defined
#endif
