/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2025  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <grub/smp.h>
#include <grub/i386/msr.h>
#include <grub/i18n.h>
#include <grub/acpi.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/time.h>
#ifdef GRUB_MACHINE_EFI
#include <grub/efi/efi.h>
#endif

#define STACK_SIZE 0x100000

static int bsp_is_x2apic = 0;
volatile grub_uint32_t *bsp_apic;

enum {
  APIC_ID = 0x2,
  APIC_ERROR_STATUS = 0x28,
  APIC_INTR_COMMAND_1 = 0x30,
  APIC_INTR_COMMAND_2 = 0x31,
};

#define IA32_MSR_APIC_BASE 0x1b
#define IA32_MSR_APIC_BASE_X2APIC (1 << 10)
#define IA32_MSR_APIC_BASE_ADDRESS 0xfffff000

static grub_err_t
init_bsp_lapic (void)
{
  grub_err_t err;

  err = grub_cpu_is_msr_supported ();

  if (err != GRUB_ERR_NONE)
    return grub_error (err, N_("RDMSR is unsupported"));

  grub_uint64_t apic_base = grub_rdmsr (IA32_MSR_APIC_BASE);

  bsp_is_x2apic = !!(apic_base & IA32_MSR_APIC_BASE_X2APIC);
  bsp_apic = (volatile grub_uint32_t *) (grub_addr_t) (apic_base & IA32_MSR_APIC_BASE_ADDRESS);

  return GRUB_ERR_NONE;
}

/* For now only to be used on BSP.  */
static grub_uint64_t
apic_read(grub_uint32_t offset)
{
  if (bsp_is_x2apic)
    return grub_rdmsr(0x800 + offset);
  else
    return bsp_apic[4 * offset];
}

static void
apic_write(grub_uint32_t offset, grub_uint32_t value)
{
  if (bsp_is_x2apic)
    grub_wrmsr(0x800 + offset, value);
  else
    bsp_apic[4 * offset] = value;
}

static grub_uint32_t
apic_get_apic_id(void)
{
  if (bsp_is_x2apic)
    return apic_read(APIC_ID);
  else
    return apic_read(APIC_ID) >> 24;
}

static void
wait_for_interrupt_delivered(void)
{
  if (bsp_is_x2apic)
    return;
  while (apic_read(APIC_INTR_COMMAND_1) & 0x1000);
}

static void
apic_send_interrupt(grub_uint32_t apicid, grub_uint32_t val)
{
  if (bsp_is_x2apic)
    grub_wrmsr(0x830, val | ((grub_uint64_t)apicid << 32));
  else
    {
      apic_write(APIC_INTR_COMMAND_2, apicid << 24);
      apic_write(APIC_INTR_COMMAND_1, val);
    }
}

static void (*smp_callback) (grub_uint32_t);
static volatile grub_uint32_t smp_current_apicid;
static volatile int ap_is_booted = 0;
extern void *grub_smp_trampoline_callback;
extern void *grub_smp_trampoline_stack;
#ifdef __x86_64__
extern grub_uint32_t grub_smp_trampoline_temporary_page_table;
extern grub_uint64_t grub_smp_trampoline_page_table;
#endif
extern grub_uint8_t grub_smp_trampoline_begin[];
extern grub_uint8_t grub_smp_trampoline_end[];

static void
trampoline_callback(void)
{
  ap_is_booted = 1;
  smp_callback(smp_current_apicid);
  /* TODO: free stack.  */
  while (1) {
    asm volatile ("cli\nhlt\n");
  }
}

static grub_err_t
grub_smp_x86_run (void (*f) (grub_uint32_t apicid), void *trampoline_space,
		  void *pgtable_space)
{
  struct grub_acpi_madt *madt = grub_acpi_find_table(GRUB_ACPI_MADT_SIGNATURE);
  if (!madt)
    return grub_error(GRUB_ERR_IO, "MADT not found");

  struct grub_acpi_madt_entry_header *d;
  grub_uint32_t len;
  int num_cpus = 0;

  for (len = madt->hdr.length - sizeof (struct grub_acpi_madt),
	 d = madt->entries
	 ; len > 0; len -= d->len, d = (void *) ((grub_uint8_t *) d + d->len))
    {
      if (d->type != GRUB_ACPI_MADT_ENTRY_TYPE_LAPIC)
	continue;

      num_cpus++;
    }

  if (num_cpus <= 1)
    {
      grub_dprintf("smp", "no APs found");
      return GRUB_ERR_NONE;
    }

  init_bsp_lapic ();

  grub_uint32_t bsp_apicid = apic_get_apic_id();

#ifdef __x86_64__
  grub_uint64_t page_table;

  asm volatile("movq	%%cr3, %0\n" : "=r"(page_table));

  void *temporary_page_table = pgtable_space;
  grub_memcpy(temporary_page_table, (void*)page_table, 4096);
#else
  (void)pgtable_space;
#endif

  smp_callback = f;

  for (len = madt->hdr.length - sizeof (struct grub_acpi_madt),
	 d = madt->entries
	 ; len > 0; len -= d->len, d = (void *) ((grub_uint8_t *) d + d->len))
    {
      grub_uint8_t *stack;
      if (d->type != GRUB_ACPI_MADT_ENTRY_TYPE_LAPIC)
	continue;

      struct grub_acpi_madt_entry_lapic *dt = (void *) d;
      if (dt->apicid == bsp_apicid)
	continue;

      stack = grub_malloc(STACK_SIZE);
      if (!stack)
	return grub_errno;

      stack += STACK_SIZE;

      smp_current_apicid = dt->apicid;
      grub_smp_trampoline_stack = stack;
      grub_smp_trampoline_callback = trampoline_callback;

#ifdef __x86_64__
      grub_smp_trampoline_temporary_page_table = (grub_uint32_t)(grub_addr_t)temporary_page_table;
      grub_smp_trampoline_page_table = page_table;
#endif
      grub_memcpy(trampoline_space, grub_smp_trampoline_begin, grub_smp_trampoline_end - grub_smp_trampoline_begin);

      ap_is_booted = 0;

      apic_write(APIC_ERROR_STATUS, 0);
      apic_read(APIC_ERROR_STATUS);

      /* INIT IPI */
      apic_send_interrupt(dt->apicid, 0xc500);

      wait_for_interrupt_delivered();

      grub_millisleep(1);

      /* Deassert INIT */
      apic_send_interrupt(dt->apicid, 0x8500);
      wait_for_interrupt_delivered();

      grub_millisleep(10);
      
      for (int j = 0; j < 2; j++) {
	apic_write(APIC_ERROR_STATUS, 0);

	/* SIPI */
	apic_send_interrupt(dt->apicid, 0x600 | ((grub_addr_t)trampoline_space >> 12));

	grub_millisleep(1);
	wait_for_interrupt_delivered();
      }

      while (!ap_is_booted);
    }

  return GRUB_ERR_NONE;
}

grub_err_t
grub_smp_run (void (*f) (grub_uint32_t apicid))
{
#ifdef GRUB_MACHINE_EFI
  grub_efi_physical_address_t trampoline_address = 0x100000, pgtable_address = 0xffffffff;
  grub_efi_boot_services_t *b;
  grub_efi_status_t status;
  b = grub_efi_system_table->boot_services;
  status = b->allocate_pages (GRUB_EFI_ALLOCATE_MAX_ADDRESS, GRUB_EFI_LOADER_CODE, 1, &trampoline_address);
  if (status != GRUB_EFI_SUCCESS)
    return grub_error (GRUB_ERR_OUT_OF_MEMORY, N_("out of memory"));

  status = b->allocate_pages (GRUB_EFI_ALLOCATE_MAX_ADDRESS, GRUB_EFI_LOADER_CODE, 1, &pgtable_address);
  if (status != GRUB_EFI_SUCCESS)
    {
      b->free_pages (trampoline_address, 1);
      return grub_error (GRUB_ERR_OUT_OF_MEMORY, N_("out of memory"));
    }

  grub_err_t err = grub_smp_x86_run (f, (void *)trampoline_address, (void *)pgtable_address);
  b->free_pages (trampoline_address, 1);
  b->free_pages (pgtable_address, 1);
  return err;
#else
#ifdef __x86_64__
#error "64-bit non-EFI isn't supported yet"
#endif
  return grub_smp_x86_run (f, (void*) 0x7000, 0);
#endif
}
