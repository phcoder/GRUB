/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2009  Free Software Foundation, Inc.
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

#include <grub/dl.h>
#include <grub/command.h>
#include <grub/extcmd.h>
#include <grub/misc.h>
#include <grub/cmos.h>
#include <grub/i18n.h>
#include <grub/mm.h>
#include <grub/env.h>

GRUB_MOD_LICENSE ("GPLv3+");

static grub_err_t
parse_args (int argc, char *argv[], int *byte, int *bit)
{
  const char *rest;

  if (argc != 1)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "address required");

  *byte = grub_strtoul (argv[0], &rest, 0);
  if (*rest != ':')
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "address required");

  *bit = grub_strtoul (rest + 1, 0, 0);

  return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_cmostest (struct grub_command *cmd __attribute__ ((unused)),
		   int argc, char *argv[])
{
  int byte = 0, bit = 0;
  grub_err_t err;
  grub_uint8_t value;

  err = parse_args (argc, argv, &byte, &bit);
  if (err)
    return err;

  err = grub_cmos_read (byte, &value);
  if (err)
    return err;

  if (value & (1 << bit))
    return GRUB_ERR_NONE;

  return grub_error (GRUB_ERR_TEST_FAILURE, N_("false"));
}

static grub_err_t
grub_cmd_cmosclean (struct grub_command *cmd __attribute__ ((unused)),
		    int argc, char *argv[])
{
  int byte = 0, bit = 0;
  grub_err_t err;
  grub_uint8_t value;

  err = parse_args (argc, argv, &byte, &bit);
  if (err)
    return err;
  err = grub_cmos_read (byte, &value);
  if (err)
    return err;

  return grub_cmos_write (byte, value & (~(1 << bit)));
}

static grub_err_t
grub_cmd_cmosset (struct grub_command *cmd __attribute__ ((unused)),
		    int argc, char *argv[])
{
  int byte = 0, bit = 0;
  grub_err_t err;
  grub_uint8_t value;

  err = parse_args (argc, argv, &byte, &bit);
  if (err)
    return err;
  err = grub_cmos_read (byte, &value);
  if (err)
    return err;

  return grub_cmos_write (byte, value | (1 << bit));
}

static grub_err_t
grub_cmd_cmoswrite (struct grub_command *cmd __attribute__ ((unused)),
		    int argc, char *argv[])
{
  int byte = -1, value = -1;

  if (argc != 2)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("two arguments expected"));

  byte = grub_strtoul (argv[0], NULL, 0);
  if (grub_errno)
    return grub_errno;

  if (byte < 0 || byte >= 0x100)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("invalid address"));

  value = grub_strtoul (argv[1], NULL, 0);
  if (grub_errno)
    return grub_errno;

  if (value < 0 || value >= 0x100)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("invalid value"));

  return grub_cmos_write (byte, value);
}

static grub_err_t
grub_cmd_cmosread (grub_extcmd_context_t ctxt, int argc, char **argv)
{
  int byte = -1;
  grub_uint8_t value = 0;
  grub_err_t err;

  if (argc != 1)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("one argument expected"));

  byte = grub_strtoul (argv[0], NULL, 0);
  if (grub_errno)
    return grub_errno;

  if (byte < 0 || byte >= 0x100)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("invalid address"));

  err = grub_cmos_read (byte, &value);
  if (err)
    return err;

  if (ctxt->state[2].set) {
    char buf[sizeof ("XX")];
    grub_snprintf (buf, sizeof (buf), "%x", value);
    grub_env_set(ctxt->state[2].arg, buf);
  } else
    grub_printf_("CMOS value at 0x%x is 0x%x\n", byte, value);
  return GRUB_ERR_NONE;
}

static const struct grub_arg_option read_options[] =
  {
    {0, 'v', 0, N_("Save read value into variable VARNAME."),
     N_("VARNAME"), ARG_TYPE_STRING},
    {0, 0, 0, 0, 0, 0}
  };

static grub_command_t cmd, cmd_clean, cmd_set, cmd_write;
static grub_extcmd_t cmd_read;


GRUB_MOD_INIT(cmostest)
{
  cmd = grub_register_command_lockdown ("cmostest", grub_cmd_cmostest,
			       N_("BYTE:BIT"),
			       N_("Test bit at BYTE:BIT in CMOS."));
  cmd_clean = grub_register_command_lockdown ("cmosclean", grub_cmd_cmosclean,
				     N_("BYTE:BIT"),
				     N_("Clear bit at BYTE:BIT in CMOS."));
  cmd_set = grub_register_command_lockdown ("cmosset", grub_cmd_cmosset,
				   N_("BYTE:BIT"),
				   /* TRANSLATORS: A bit may be either set (1) or clear (0).  */
				   N_("Set bit at BYTE:BIT in CMOS."));
  cmd_read = grub_register_extcmd_lockdown ("cmosread", grub_cmd_cmosread, 0,
				       N_("[-v VAR] ADDR"),
				       N_("Read CMOS byte at ADDR."), read_options);
  cmd_write = grub_register_command_lockdown ("cmoswrite", grub_cmd_cmoswrite,
					      N_("ADDR VALUE"),
					      N_("Set CMOS byte at ADDR to VALUE."));
}

GRUB_MOD_FINI(cmostest)
{
  grub_unregister_command (cmd);
  grub_unregister_command (cmd_clean);
  grub_unregister_command (cmd_set);
  grub_unregister_extcmd (cmd_read);
  grub_unregister_command (cmd_write);
}
