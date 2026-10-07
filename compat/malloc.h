#pragma once
// MSVC's <malloc.h>.  The system's comes first: Android's <stdlib.h> includes
// <malloc.h> for malloc/free themselves, and would otherwise get this file.
#if defined(__has_include_next)
#if __has_include_next(<malloc.h>)
#include_next <malloc.h>
#endif
#endif
#include <stdlib.h>
