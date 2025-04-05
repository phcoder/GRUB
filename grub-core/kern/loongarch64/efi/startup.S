/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2023 Free Software Foundation, Inc.
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

#include <grub/symbol.h>

	.file 	"startup.S"
	.text

FUNCTION(_start)
	/*
	 * EFI_SYSTEM_TABLE and EFI_HANDLE are passed in $a1/$a0.
	 */

	pcalau12i       $r13,%got_pc_hi20(EXT_C(grub_efi_image_handle))
	addi.d  	$r12,$r0,%got_pc_lo12(EXT_C(grub_efi_image_handle))
	lu32i.d 	$r12,%got64_pc_lo20(EXT_C(grub_efi_image_handle))
	lu52i.d 	$r12,$r12,%got64_pc_hi12(EXT_C(grub_efi_image_handle))
	ldx.d   	$r12,$r13,$r12
	st.d		$a0, $r12, 0

	pcalau12i       $r13,%got_pc_hi20(EXT_C(grub_efi_system_table))
	addi.d  	$r12,$r0,%got_pc_lo12(EXT_C(grub_efi_system_table))
	lu32i.d 	$r12,%got64_pc_lo20(EXT_C(grub_efi_system_table))
	lu52i.d 	$r12,$r12,%got64_pc_hi12(EXT_C(grub_efi_system_table))
	ldx.d   	$r12,$r13,$r12
	st.d		$a1, $r12, 0

	b		EXT_C(grub_main)
