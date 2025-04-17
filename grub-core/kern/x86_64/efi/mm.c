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
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/err.h>
#include <grub/dl.h>
#include <grub/cache.h>
#include <grub/kernel.h>
#include <grub/efi/efi.h>

static grub_efi_status_t
create_paging_entry(grub_uint64_t *entry)
{
  grub_efi_status_t status;
  grub_efi_boot_services_t *b;

  grub_uint64_t address = 0xffffffff;

  b = grub_efi_system_table->boot_services;
  /* TODO: Which type should it be?  */
  status = b->allocate_pages (GRUB_EFI_ALLOCATE_MAX_ADDRESS, GRUB_EFI_LOADER_DATA, 1, &address);
  if (status != GRUB_EFI_SUCCESS)
    return status;
  *entry = address | 7;
  return GRUB_EFI_SUCCESS;
}

grub_efi_status_t
grub_efi_arch_ensure_mapping (grub_efi_physical_address_t address,
			      grub_efi_uintn_t pages)
{
  grub_uint64_t cr3;
  asm volatile("movq   %%cr3, %0\n" : "=r"(cr3));
  for (grub_uint64_t page = 0; page < pages; page++)
    {
      grub_uint64_t pageidx = (address >> 12) + page;
      grub_uint64_t *pt4 = (grub_uint64_t *) cr3;
      if (!(pt4[(pageidx >> 27) & 0x1ff] & 1)) {
	grub_efi_status_t status = create_paging_entry(&pt4[(pageidx >> 27) & 0x1ff]);
	if (status != GRUB_EFI_SUCCESS)
	  return status;
      }
      grub_uint64_t *pt3 = (grub_uint64_t *) (pt4[(pageidx >> 27) & 0x1ff] & ~0xfff);
      if (!(pt3[(pageidx >> 18) & 0x1ff] & 1)) {
	grub_efi_status_t status = create_paging_entry(&pt3[(pageidx >> 18) & 0x1ff]);
	if (status != GRUB_EFI_SUCCESS)
	  return status;
      }
      if (pt3[(pageidx >> 18) & 0x1ff] & 0x80)
	continue;
      grub_uint64_t *pt2 = (grub_uint64_t *) (pt3[(pageidx >> 18) & 0x1ff] & ~0xfff);
      if (!(pt2[(pageidx >> 9) & 0x1ff] & 1)) {
	grub_efi_status_t status = create_paging_entry(&pt3[(pageidx >> 9) & 0x1ff]);
	if (status != GRUB_EFI_SUCCESS)
	  return status;
      }
      if (pt2[(pageidx >> 9) & 0x1ff] & 0x80)
	continue;

      grub_uint64_t *pt1 = (grub_uint64_t *) (pt2[(pageidx >> 9) & 0x1ff] & ~0xfff);
      if (!(pt1[pageidx & 0x1ff] & 1)) {
	pt1[pageidx & 0x1ff] = (pageidx << 12) | 7;
      }
    }

  return GRUB_EFI_SUCCESS;
}
