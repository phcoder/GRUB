/* xapic.c - Switch all CPUs from x2apic to xapic mode.  */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2008,2025  Free Software Foundation, Inc.
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
#include <grub/acpi.h>
#include <grub/command.h>
#include <grub/i18n.h>
#include <grub/dl.h>
#include <grub/i386/msr.h>
#include <grub/smp.h>

GRUB_MOD_LICENSE ("GPLv3+");

static void
switch_to_xapic(grub_uint32_t ignored __attribute__((unused)))
{
  grub_uint64_t m = grub_rdmsr(0x1b);
  grub_wrmsr(0x1b, m & ~0xc00);
  grub_wrmsr(0x1b, (m & ~0xc00) | 0x800);
}

static grub_err_t
grub_cmd_xapic (grub_command_t cmd __attribute__ ((unused)),
		 int argc __attribute__ ((unused)),
		 char **args __attribute__ ((unused)))
{
  switch_to_xapic(0);
  grub_smp_run (switch_to_xapic);
  return GRUB_ERR_NONE;
}

static grub_command_t cmd;

GRUB_MOD_INIT(xapic)
{
  cmd = grub_register_command ("xapic", grub_cmd_xapic,
			       0, N_("Switch to xapic."));
}

GRUB_MOD_FINI(xapic)
{
  grub_unregister_command (cmd);
}


