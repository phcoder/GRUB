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

#include <grub/symbol.h>
#include <include/grub/i386/memory.h>

#define CODE32_SEGMENT	0x8
#define DATA32_SEGMENT	0x10
#define CODE64_SEGMENT	0x28

	.p2align	5	/* force 32-byte alignment */
VARIABLE(grub_smp_trampoline_begin)
begin:	
	.code16
	movw 	%cs, %ax
	movw	%ax, %ds

	/* We start at VV00:0000. Relocate the remainder.
	 * Use the fact that %cs is divisible by 0x10,
	 * shift it and add to the word at second byte
	 */
	shrw	$4, %ax
	addw	%ax, (gdt_addr-begin+1)
	addw	%ax, (procseg_addr-begin+1)
	addw	%ax, (cseg64_addr-begin+1)

	lgdtl	(gdtdesc-begin)

	/* turn on protected mode */
	movl	%cr0, %eax
	orl	$GRUB_MEMORY_CPU_CR0_PE_ON, %eax
	movl	%eax, %cr0

	/* jump to relocation, flush prefetch queue, and reload %cs */
	.byte 0x66, 0xea
procseg_addr:	
	.long (protcseg-begin)
	.word CODE32_SEGMENT

	.code32
protcseg:
	/* reload other segment registers */
	movw	$DATA32_SEGMENT, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %fs
	movw	%ax, %gs
	movw	%ax, %ss

#ifndef __x86_64__
	/* mov imm32, %eax */
	.byte	0xb8
VARIABLE(grub_smp_trampoline_stack)
	.long	0
	movl	%eax, %esi
	/* mov imm32, %eax */
	.byte	0xb8
VARIABLE(grub_smp_trampoline_callback)
	.long	0
	jmpl 	*%eax
#else
	/* Turn on PAE. */
	movl	%cr4, %eax
	orl	$(GRUB_MEMORY_CPU_CR4_PAE_ON | GRUB_MEMORY_CPU_CR4_PSE_ON), %eax
	movl	%eax, %cr4

	/* mov imm32, %eax */
	.byte	0xb8
VARIABLE(grub_smp_trampoline_temporary_page_table)
	.long	0
	movl	%eax, %cr3

	/* Turn on amd64. */
	movl	$GRUB_MEMORY_CPU_AMD64_MSR, %ecx
	rdmsr
	orl	$GRUB_MEMORY_CPU_AMD64_MSR_ON, %eax
	wrmsr

	/* Enable paging. */
	movl	%cr0, %eax
	orl	$GRUB_MEMORY_CPU_CR0_PAGING_ON, %eax
	movl	%eax, %cr0

	/* jump to relocation, flush prefetch queue, and reload %cs */
	.byte 0xea
cseg64_addr:	
	.long (cseg64-begin)
	.word CODE64_SEGMENT

	.code64
cseg64:
	/* mov imm64, %rax */
	.byte 	0x48
	.byte	0xb8
VARIABLE(grub_smp_trampoline_page_table)
	.quad	0
	movq	%rax, %cr3

	/* mov imm64, %rax */
	.byte 	0x48
	.byte	0xb8
VARIABLE(grub_smp_trampoline_stack)
	.quad	0
	movq	%rax, %rsp

	/* mov imm64, %rax */
	.byte 	0x48
	.byte	0xb8
VARIABLE(grub_smp_trampoline_callback)
	.quad	0
	jmpq	*%rax
#endif

	.p2align	5	/* force 32-byte alignment */
gdt:	
	.word	0, 0
	.byte	0, 0, 0, 0

	/* -- code segment --
	 * base = 0x00000000, limit = 0xFFFFF (4 KiB Granularity), present
	 * type = 32bit code execute/read, DPL = 0
	 */
	.word	0xFFFF, 0
	.byte	0, 0x9A, 0xCF, 0

	/* -- data segment --
	 * base = 0x00000000, limit 0xFFFFF (4 KiB Granularity), present
	 * type = 32 bit data read/write, DPL = 0
	 */
	.word	0xFFFF, 0
	.byte	0, 0x92, 0xCF, 0

	/* -- 16 bit real mode CS --
	 * base = 0x00000000, limit 0x0FFFF (1 B Granularity), present
	 * type = 16 bit code execute/read only/conforming, DPL = 0
	 */
	.word	0xFFFF, 0
	.byte	0, 0x9E, 0, 0

	/* -- 16 bit real mode DS --
	 * base = 0x00000000, limit 0x0FFFF (1 B Granularity), present
	 * type = 16 bit data read/write, DPL = 0
	 */
	.word	0xFFFF, 0
	.byte	0, 0x92, 0, 0

	/* -- code segment 64 --
	 * base = 0x00000000, limit = 0xFFFFF (4 KiB Granularity), present
	 * type = 64bit code execute/read, DPL = 0
	 */
	.word	0xFFFF, 0
	.byte	0, 0x9A, 0xAF, 0

	.p2align 5
/* this is the GDT descriptor */
gdtdesc:
	.word	0x2f			/* limit */
gdt_addr:
	.long	(gdt-begin)			/* addr */
	
	
	
VARIABLE(grub_smp_trampoline_end)
	
