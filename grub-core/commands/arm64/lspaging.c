/* lspaging.c  - Display paging table.  */
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
#include <grub/types.h>
#include <grub/mm.h>
#include <grub/misc.h>
#include <grub/normal.h>
#include <grub/charset.h>
#include <grub/efi/api.h>
#include <grub/efi/efi.h>
#include <grub/dl.h>

GRUB_MOD_LICENSE ("GPLv3+");

typedef enum {UNMAPPED, NONIDENTITY, IDENTITY} mapping_type_t;

static const char *mapping_names[] = {
  "unmapped",
  "non-identity",
  "identity",
};

static bool
process_level(grub_uint64_t *start_address, mapping_type_t *start_type,
	      grub_uint64_t *next_address, grub_uint64_t *lx, grub_uint64_t cur_address, int offset)
{
  mapping_type_t cur_type;
  grub_uint64_t entry = ((grub_uint64_t *)*lx)[(cur_address >> offset) & 0x1ff];
  switch (entry & 3)
    {
    case 0:
    case 2:
      cur_type = UNMAPPED;
      break;
    case 3: /* Table entry */
      if (offset != 12)
	{
	  *lx = (entry & 0xfffffffff000);
	  return false;
	}
      /* Fallthrough */
    case 1:
      cur_type = ((entry & 0xfffffffff000) == (cur_address & 0xfffffffff000)) ? IDENTITY : NONIDENTITY;
      break;
    }

  *next_address = ((cur_address >> offset) + 1) << offset;

  if (*start_type != cur_type)
    {
      if (cur_address != 0)
	grub_printf("%08lx-%08lx: %s\n", *start_address, cur_address, mapping_names[*start_type]);
      *start_type = cur_type;
      *start_address = cur_address;
    }

  return true;
}

static grub_err_t
grub_cmd_lspaging (struct grub_command *cmd __attribute__ ((unused)),
		int argc __attribute__ ((unused)),
		char **args __attribute__ ((unused)))
{
  grub_uint64_t tcr_el1, ttbr0_el1;
  /*
  grub_uint64_t tcr2_el1;

    asm volatile ("mrs       %0, tcr2_el1": "=r"(tcr2_el1));
  if ((tcr2_el1 >> 5) & 1)
  return grub_error(GRUB_ERR_NOT_IMPLEMENTED_YET, "Parsing D128 tables isn't supported yet");*/
  asm volatile ("mrs       %0, tcr_el1": "=r"(tcr_el1));

  if ((tcr_el1 >> 14) & 3)
    return grub_error(GRUB_ERR_NOT_IMPLEMENTED_YET, "non-4K page tables aren't supported yet");

  grub_uint64_t t0sz = tcr_el1 & 0x3f;

  if (t0sz >= 25)
    return grub_error(GRUB_ERR_NOT_IMPLEMENTED_YET, "T0SZ %ld isn't supported yet", t0sz);

  asm volatile ("mrs       %0, ttbr0_el1": "=r"(ttbr0_el1));

  grub_uint64_t baddr = (ttbr0_el1 & 0xfffffffffffell);
  grub_uint64_t start_address = 0, cur_address = 0, next_address = 0;
  mapping_type_t start_type = UNMAPPED;

  grub_printf("baddr=%lx\n", baddr);

  for (cur_address = 0; cur_address < 0x7fffffffffff; cur_address = next_address)
    {
      grub_uint64_t lx = baddr;
      if (process_level(&start_address, &start_type, &next_address, &lx, cur_address, 39))
	continue;
      if (process_level(&start_address, &start_type, &next_address, &lx, cur_address, 30))
	continue;
      if (process_level(&start_address, &start_type, &next_address, &lx, cur_address, 21))
	continue;
      process_level(&start_address, &start_type, &next_address, &lx, cur_address, 12);
    }

  grub_printf("%08lx-%08lx: %s\n", start_address, cur_address, mapping_names[start_type]);

  return GRUB_ERR_NONE;
}

static grub_command_t cmd;

GRUB_MOD_INIT(lspaging)
{
  cmd = grub_register_command ("lspaging", grub_cmd_lspaging, "",
			       "Display paging table.");
}

GRUB_MOD_FINI(lspaging)
{
  grub_unregister_command (cmd);
}
