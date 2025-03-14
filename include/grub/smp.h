#ifndef GRUB_SMP_HEADER
#define GRUB_SMP_HEADER 1

#include <grub/types.h>
#include <grub/err.h>

/* cpu id is machine dependent. On x86 it's APIC ID.  */
grub_err_t
grub_smp_run (void (*f) (grub_uint32_t cpu_id));

#endif
