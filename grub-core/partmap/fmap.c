/* gpt.c - Read GUID Partition Tables (GPT).  */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2023  Free Software Foundation, Inc.
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

#include <grub/disk.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/partition.h>
#include <grub/dl.h>
#include <grub/i18n.h>
#include <grub/coreboot/lbio.h>

GRUB_MOD_LICENSE ("GPLv3+");

/* Definitions copied from fmap_serialized.h using BSDL.  */
#define GRUB_FMAP_SIGNATURE		"__FMAP__"
#define GRUB_FMAP_VER_MAJOR		1	/* this header's FMAP minor version */
#define GRUB_FMAP_VER_MINOR		1	/* this header's FMAP minor version */
#define GRUB_FMAP_STRLEN		32	/* maximum length for strings, */
					/* including null-terminator */

/* Mapping of volatile and static regions in firmware binary */
struct grub_fmap_entry {
  grub_uint32_t offset;                /* offset relative to base */
  grub_uint32_t size;                  /* size in bytes */
  grub_uint8_t  name[GRUB_FMAP_STRLEN];     /* descriptive name */
  grub_uint16_t flags;                 /* flags for this area */
} GRUB_PACKED;

struct grub_fmap_header {
  grub_uint8_t  signature[8];		/* "__FMAP__" (0x5F5F464D41505F5F) */
  grub_uint8_t  ver_major;		/* major version */
  grub_uint8_t  ver_minor;		/* minor version */
  grub_uint64_t base;			/* address of the firmware binary */
  grub_uint32_t size;			/* size of firmware binary in bytes */
  grub_uint8_t  name[GRUB_FMAP_STRLEN];	/* name of this firmware binary */
  grub_uint16_t nareas;		/* number of areas described by
				   fmap_areas[] below */
} GRUB_PACKED;

static struct grub_partition_map grub_fmap_partition_map;



static int
validate_fmap_header(struct grub_fmap_header *header)
{
  if (grub_memcmp (header->signature, GRUB_FMAP_SIGNATURE, sizeof (header->signature)) != 0)
    return 0;
  if (header->ver_major != GRUB_FMAP_VER_MAJOR || header->ver_minor != GRUB_FMAP_VER_MINOR)
    return 0;
  return 1;
}

static grub_uint64_t cbfsdisk_fmap_offset = 0xffffffff;

static int cbtable_iter (grub_linuxbios_table_item_t item,
			 void *ctxt_in __attribute__((unused)))
{
  if (item->tag == GRUB_LINUXBIOS_MEMBER_BOOT_MEDIA)
    {
      cbfsdisk_fmap_offset = ((struct grub_linuxbios_table_boot_media *) (item + 1))->fmap_offset;
      return 1;
    }

  return 0;
}

static void
discover_cbfsdisk_fmap_offset(void)
{
  static int discovery_done;
  if (discovery_done)
    return;
   grub_linuxbios_table_iterate (cbtable_iter, NULL);
}


static grub_err_t
grub_fmap_partition_map_iterate (grub_disk_t disk,
				grub_partition_iterate_hook_t hook,
				void *hook_data)
{
  struct grub_partition part;
  struct grub_fmap_header header;
  struct grub_fmap_entry entry;
  unsigned int i;
  grub_uint64_t header_offset = 0;
  grub_uint64_t current_offset = 0;

  if (disk->dev->id == GRUB_DISK_DEVICE_CBFSDISK_ID)
    {
      discover_cbfsdisk_fmap_offset();
      header_offset = cbfsdisk_fmap_offset;
      if (cbfsdisk_fmap_offset == 0xffffffff || cbfsdisk_fmap_offset == 0xffffffffffffffffULL)
	return grub_error(GRUB_ERR_BAD_PART_TABLE, "fmap not declared");
    }
  else
    return grub_error(GRUB_ERR_BAD_PART_TABLE, "fmap in non-cbfs devices isn't implemented yet");

  /* Read the FMAP header.  */
  if (grub_disk_read (disk, header_offset >> GRUB_DISK_SECTOR_BITS, header_offset & (GRUB_DISK_SECTOR_SIZE - 1), sizeof (header), &header))
	return grub_errno;

  if (!validate_fmap_header(&header))
    return grub_error (GRUB_ERR_BAD_PART_TABLE, "no valid FMAP header");

  grub_dprintf ("fmap", "Read a valid FMAP header\n");

  current_offset = header_offset + sizeof(header);

  for (i = 0; i < grub_le_to_cpu16 (header.nareas); i++)
    {
      if (grub_disk_read (disk, current_offset >> GRUB_DISK_SECTOR_BITS, current_offset & (GRUB_DISK_SECTOR_SIZE - 1),
			  sizeof (entry), &entry))
	return grub_errno;

      /* TODO: What to do if it's not 512-bytes aligned?  */
      grub_uint64_t end = (grub_uint64_t) grub_le_to_cpu32 (entry.offset) + (grub_uint64_t) grub_le_to_cpu32 (entry.size);
      part.start = grub_le_to_cpu32 (entry.offset) >> GRUB_DISK_SECTOR_BITS;
      part.len = (end >> GRUB_DISK_SECTOR_BITS) - part.start;
      part.offset = current_offset >> GRUB_DISK_SECTOR_BITS;
      part.number = i;
      part.index = current_offset & (GRUB_DISK_SECTOR_SIZE - 1);
      part.partmap = &grub_fmap_partition_map;
      part.parent = disk->partition;

      grub_dprintf ("gpt", "FMAP entry %d: start=0x%llx, length=0x%llx\n", i,
		    (unsigned long long) grub_le_to_cpu32 (entry.offset),
		    (unsigned long long) grub_le_to_cpu32 (entry.size));

      if (hook (disk, &part, hook_data))
	return grub_errno;

      current_offset += sizeof (entry);
    }

  return GRUB_ERR_NONE;
}


/* Partition map type.  */
static struct grub_partition_map grub_fmap_partition_map =
  {
    .name = "fmap",
    .iterate = grub_fmap_partition_map_iterate,
  };

GRUB_MOD_INIT(part_fmap)
{
  grub_partition_map_register (&grub_fmap_partition_map);
}

GRUB_MOD_FINI(part_fmap)
{
  grub_partition_map_unregister (&grub_fmap_partition_map);
}
