#include <config.h>
#if HAVE_ARM_HARDWARE
#include "../libarmbox/record_lib.h"
#elif HAVE_MIPS_HARDWARE
#include "../libmipsbox/record_lib.h"
#elif HAVE_GENERIC_HARDWARE
#include "../libgeneric-pc/record_lib.h"
#else
#error no valid hardware defined
#endif
