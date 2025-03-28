/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2008  Free Software Foundation, Inc.
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

#ifndef	GRUB_MACHINE_PCI_H
#define	GRUB_MACHINE_PCI_H	1

#include <grub/types.h>
#include <grub/cpu/io.h>
#include <grub/pci.h>

#define GRUB_MACHINE_PCI_IO_BASE 0xb8000000

grub_uint32_t
EXPORT_FUNC (grub_pci_read) (grub_pci_address_t addr);

grub_uint16_t
EXPORT_FUNC (grub_pci_read_word) (grub_pci_address_t addr);

grub_uint8_t
EXPORT_FUNC (grub_pci_read_byte) (grub_pci_address_t addr);

void
EXPORT_FUNC (grub_pci_write) (grub_pci_address_t addr, grub_uint32_t data);

void
EXPORT_FUNC (grub_pci_write_word) (grub_pci_address_t addr, grub_uint16_t data);

void
EXPORT_FUNC (grub_pci_write_byte) (grub_pci_address_t addr, grub_uint8_t data);

volatile void *
EXPORT_FUNC (grub_pci_device_map_range) (grub_pci_device_t dev,
					 grub_addr_t base, grub_size_t size);
void *
EXPORT_FUNC (grub_pci_device_map_range_cached) (grub_pci_device_t dev,
						grub_addr_t base,
						grub_size_t size);
void
EXPORT_FUNC (grub_pci_device_unmap_range) (grub_pci_device_t dev,
					   volatile void *mem,
					   grub_size_t size);

#define GRUB_PCI_NUM_DEVICES    32
#define GRUB_PCI_NUM_BUS        256

#endif
