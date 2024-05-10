/* loopback.c - command to add loopback devices.  */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2005,2006,2007  Free Software Foundation, Inc.
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
#include <grub/misc.h>
#include <grub/file.h>
#include <grub/disk.h>
#include <grub/mm.h>
#include <grub/extcmd.h>
#include <grub/i18n.h>

GRUB_MOD_LICENSE ("GPLv3+");

#define QCOW_MAGIC 0x514649fb
#define LX_OFFSET_MASK 0xfffffffffffe00LL
struct qcow_header
{
  grub_uint32_t magic;
  grub_uint32_t version;
  grub_uint64_t backing_file_offset;
  grub_uint32_t backing_file_size;
  grub_uint32_t cluster_bits;
  grub_uint64_t size;
  grub_uint32_t crypt_method;
  grub_uint32_t l1_size;
  grub_uint64_t l1_table_offset;
  grub_uint64_t refcount_table_offset;
  grub_uint32_t refcount_table_clusters;
  grub_uint32_t nb_snapshots;
  grub_uint64_t snapshots_offset;

  /* v3 only */
  grub_uint64_t feat_incompat;
  grub_uint64_t feat_compat;
  grub_uint64_t feat_autoclear;
  grub_uint32_t refcount_order;
  grub_uint32_t header_length;
};

struct qcow_header_extension
{
  grub_uint32_t type;
  grub_uint32_t length;
};

struct grub_qcow
{
  struct grub_qcow *next;
  char *devname;
  grub_file_t file;
  unsigned long id;
  struct qcow_header head;
  grub_uint64_t *l1;
  grub_uint64_t *l2_0;
  grub_uint64_t *l2_cache;
  grub_uint64_t l2_cache_current;
};

static struct grub_qcow *qcow_list;
static unsigned long last_id = 0;

static const struct grub_arg_option options[] =
  {
    /* TRANSLATORS: The disk is simply removed from the list of available ones,
       not wiped, avoid to scare user.  */
    {"delete", 'd', 0, N_("Delete the specified qcow drive."), 0, 0},
    {0, 0, 0, 0, 0, 0}
  };

static grub_err_t
open_qcow (struct grub_qcow *qcow)
{
  grub_file_read (qcow->file, &qcow->head, sizeof(qcow->head));
  if (grub_errno)
    return grub_errno;
  if (qcow->head.magic != grub_cpu_to_be32_compile_time(QCOW_MAGIC))
    return grub_error(GRUB_ERR_BAD_ARGUMENT, "invalid qcow magic");
  if (qcow->head.version != grub_cpu_to_be32_compile_time(2)
      && qcow->head.version != grub_cpu_to_be32_compile_time(3))
    return grub_error(GRUB_ERR_NOT_IMPLEMENTED_YET, "unsupported qcow version");
  if (qcow->head.backing_file_offset || qcow->head.backing_file_size)
    return grub_error(GRUB_ERR_NOT_IMPLEMENTED_YET, "qcow backing file unsupported");
  if (qcow->head.crypt_method)
    return grub_error(GRUB_ERR_NOT_IMPLEMENTED_YET, "encrypted qcow is not supported");

  if (grub_be_to_cpu32(qcow->head.l1_size) >= (1 << 28))
    return grub_error(GRUB_ERR_BAD_ARGUMENT, "qcow l1 table is too large");
  if (!qcow->head.l1_size)
    return grub_error(GRUB_ERR_BAD_ARGUMENT, "L1 table is missing");
  if (grub_be_to_cpu32(qcow->head.cluster_bits) >= 26)
    return grub_error(GRUB_ERR_BAD_ARGUMENT, "qcow cluster size is too large");

  grub_size_t l1_bytes = grub_be_to_cpu32(qcow->head.l1_size) << 3;
  qcow->l1 = grub_malloc(l1_bytes);
  if (!qcow->l1)
    return grub_errno;

  grub_file_seek (qcow->file, grub_be_to_cpu64(qcow->head.l1_table_offset));
  grub_file_read (qcow->file, qcow->l1, l1_bytes);
  if (grub_errno)
    return grub_errno;

  grub_size_t l2_bytes = 1 << grub_be_to_cpu32(qcow->head.cluster_bits);
  qcow->l2_0 = grub_zalloc(l2_bytes);
  qcow->l2_cache = grub_zalloc(l2_bytes);
  if (!qcow->l2_0 || !qcow->l2_cache)
    {
      grub_free(qcow->l2_0);
      grub_free(qcow->l2_cache);
      return grub_errno;
    }

  if (qcow->l1[0])
    {
      grub_file_seek (qcow->file, grub_be_to_cpu64(qcow->l1[0]) & LX_OFFSET_MASK);
      grub_file_read (qcow->file, qcow->l2_0, l2_bytes);
    }
  if (grub_errno)
    return grub_errno;

  return GRUB_ERR_NONE;
}

/* Delete the qcow device NAME.  */
static grub_err_t
delete_qcow (const char *name)
{
  struct grub_qcow *dev;
  struct grub_qcow **prev;

  /* Search for the device.  */
  for (dev = qcow_list, prev = &qcow_list;
       dev;
       prev = &dev->next, dev = dev->next)
    if (grub_strcmp (dev->devname, name) == 0)
      break;

  if (! dev)
    return grub_error (GRUB_ERR_BAD_DEVICE, "device not found");

  /* Remove the device from the list.  */
  *prev = dev->next;

  grub_free (dev->devname);
  grub_file_close (dev->file);
  grub_free (dev);

  return 0;
}

/* The command to add and remove qcow devices.  */
static grub_err_t
grub_cmd_qcow (grub_extcmd_context_t ctxt, int argc, char **args)
{
  struct grub_arg_list *state = ctxt->state;
  grub_file_t file;
  enum grub_file_type type = GRUB_FILE_TYPE_LOOPBACK;
  struct grub_qcow *newdev;
  grub_err_t ret;

  if (argc < 1)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "device name required");

  /* Check if `-d' was used.  */
  if (state[0].set)
      return delete_qcow (args[0]);

  type |= GRUB_FILE_TYPE_NO_DECOMPRESS;

  if (argc < 2)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("filename expected"));

  /* Check that a device with requested name does not already exist. */
  for (newdev = qcow_list; newdev; newdev = newdev->next)
    if (grub_strcmp (newdev->devname, args[0]) == 0)
      return grub_error (GRUB_ERR_BAD_ARGUMENT, "device name already exists");

  file = grub_file_open (args[1], type);
  if (! file)
    return grub_errno;

  /* Unable to replace it, make a new entry.  */
  newdev = grub_zalloc (sizeof (struct grub_qcow));
  if (! newdev)
    goto fail;

  newdev->devname = grub_strdup (args[0]);
  if (! newdev->devname)
    {
      grub_free (newdev);
      goto fail;
    }

  newdev->file = file;
  newdev->id = last_id++;

  ret = open_qcow(newdev);
  if (ret)
    {
      grub_free(newdev->devname);
      grub_free(newdev);
      goto fail;
    }

  /* Add the new entry to the list.  */
  newdev->next = qcow_list;
  qcow_list = newdev;

  return 0;

fail:
  ret = grub_errno;
  grub_file_close (file);
  return ret;
}


static int
grub_qcow_iterate (grub_disk_dev_iterate_hook_t hook, void *hook_data,
		       grub_disk_pull_t pull)
{
  struct grub_qcow *d;
  if (pull != GRUB_DISK_PULL_NONE)
    return 0;
  for (d = qcow_list; d; d = d->next)
    {
      if (hook (d->devname, hook_data))
	return 1;
    }
  return 0;
}

static grub_err_t
grub_qcow_open (const char *name, grub_disk_t disk)
{
  struct grub_qcow *dev;

  for (dev = qcow_list; dev; dev = dev->next)
    if (grub_strcmp (dev->devname, name) == 0)
      break;

  if (! dev)
    return grub_error (GRUB_ERR_UNKNOWN_DEVICE, "can't open device");

  /* Use the filesize for the disk size, round up to a complete sector.  */
  disk->total_sectors = grub_be_to_cpu64(dev->head.size) >> GRUB_DISK_SECTOR_BITS;
  /* Avoid reading more than 512M.  */
  disk->max_agglomerate = 1 << (29 - GRUB_DISK_SECTOR_BITS
				- GRUB_DISK_CACHE_BITS);

  disk->id = dev->id;

  disk->data = dev;

  return 0;
}

static grub_err_t
get_l2_entry(struct grub_qcow *qcow, grub_uint64_t cluster, grub_uint64_t *l2e)
{
  grub_size_t l2_bytes = 1 << grub_be_to_cpu32(qcow->head.cluster_bits);
  grub_uint64_t l2_table_bits = grub_be_to_cpu32(qcow->head.cluster_bits) - 3;
  grub_uint64_t l2n = cluster & ((1 << l2_table_bits) - 1);
  grub_uint64_t l1n = cluster >> l2_table_bits;
  if (qcow->l1[l1n] == 0)
    {
      *l2e = 0;
      return GRUB_ERR_NONE;
    }
    
  if (l1n >= grub_be_to_cpu32(qcow->head.l1_size))
    return grub_error(GRUB_ERR_IO, "seeking outside of L1 table");
  if (l1n == 0)
    {
      *l2e = qcow->l2_0[l2n];
      return GRUB_ERR_NONE;
    }
  if (l1n == qcow->l2_cache_current)
    {
      *l2e = qcow->l2_cache[l2n];
      return GRUB_ERR_NONE;
    }

  qcow->l2_cache_current = 0;
  grub_file_seek (qcow->file, grub_be_to_cpu64(qcow->l1[l1n]) & LX_OFFSET_MASK);
  grub_file_read (qcow->file, qcow->l2_cache, l2_bytes);
  if (grub_errno)
    return grub_errno;
  qcow->l2_cache_current = l1n;
  *l2e = qcow->l2_cache[l2n];
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_qcow_read (grub_disk_t disk, grub_disk_addr_t sector,
		    grub_size_t size, char *buf)
{
  struct grub_qcow *qcow = (struct grub_qcow *) disk->data;
  unsigned cluster_sec_bits = grub_be_to_cpu32(qcow->head.cluster_bits) - GRUB_DISK_SECTOR_BITS;
  grub_uint64_t cluster_sec_size = 1 << cluster_sec_bits;
  grub_uint64_t cluster = sector >> cluster_sec_bits;
  grub_size_t cluster_sec_offset = sector & ((1 << cluster_sec_bits) - 1);
  grub_file_t file = qcow->file;

  while (size)
    {
      grub_size_t max_read = cluster_sec_size - cluster_sec_offset;
      grub_uint64_t l2e = 0;
      if (max_read > size)
	max_read = size;
      
      grub_err_t err = get_l2_entry(qcow, cluster, &l2e);
      if (err)
	return err;

      if (l2e)
	{
	  grub_file_seek (file, (grub_be_to_cpu64(l2e) & LX_OFFSET_MASK) + (cluster_sec_offset << GRUB_DISK_SECTOR_BITS));
	  grub_file_read (file, buf, max_read << GRUB_DISK_SECTOR_BITS);
	  if (grub_errno)
	    return grub_errno;
	}
      else
	{
	  grub_memset (buf, 0, max_read << GRUB_DISK_SECTOR_BITS);
	}
      buf += max_read << GRUB_DISK_SECTOR_BITS;
      size -= max_read;
      cluster_sec_offset = 0;
      cluster++;
    }

  return 0;
}

static grub_err_t
grub_qcow_write (grub_disk_t disk __attribute ((unused)),
		     grub_disk_addr_t sector __attribute ((unused)),
		     grub_size_t size __attribute ((unused)),
		     const char *buf __attribute ((unused)))
{
  return grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET,
		     "qcow write is not supported");
}

static struct grub_disk_dev grub_qcow_dev =
  {
    .name = "qcow",
    .id = GRUB_DISK_DEVICE_QCOW_ID,
    .disk_iterate = grub_qcow_iterate,
    .disk_open = grub_qcow_open,
    .disk_read = grub_qcow_read,
    .disk_write = grub_qcow_write,
    .next = 0
  };

static grub_extcmd_t cmd;

GRUB_MOD_INIT(qcow)
{
  cmd = grub_register_extcmd ("qcow", grub_cmd_qcow, 0,
			      N_("[-d] [-D] DEVICENAME FILE."),
			      /* TRANSLATORS: The file itself is not destroyed
				 or transformed into drive.  */
			      N_("Make a virtual drive from a file."), options);
  grub_disk_dev_register (&grub_qcow_dev);
}

GRUB_MOD_FINI(qcow)
{
  grub_unregister_extcmd (cmd);
  grub_disk_dev_unregister (&grub_qcow_dev);
}
