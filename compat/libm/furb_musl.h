/* Force-included when compiling these musl sources (build.py): the two
   macros musl's own <features.h> would supply.  Everything else comes from
   the system headers. */
#define hidden __attribute__((__visibility__("hidden")))
#define weak_alias(old, new) extern __typeof(old) new __attribute__((__weak__, __alias__(#old)))
