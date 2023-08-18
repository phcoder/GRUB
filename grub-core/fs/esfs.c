/* esfs.c - Essence file system */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2023  Free Software Foundation, Inc.
 *
 * This file is distributed at your choice under GPLv3, later version
 * of it, or MIT license.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this file, to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#define ESFS_SIGNATURE_STRING        			"!EssenceFS2-----"	// The signature in the superblock.
#define ESFS_DIRECTORY_ENTRY_SIGNATURE 			"DirEntry"		// The signature in directory entries.
#define ESFS_MAXIMUM_VOLUME_NAME_LENGTH 		(32)			// The volume name limit.
#define ESFS_DRIVER_VERSION 				(10)			// The current driver version.

#define ESFS_NODE_TYPE_FILE 				(1)			// DirectoryEntry.nodeType: a file.
#define ESFS_NODE_TYPE_DIRECTORY 			(2)			// DirectoryEntry.nodeType: a directory.

#define ESFS_ATTRIBUTE_DATA				(1)			// Contains the data of the file, or a list of DirectoryEntries.
#define ESFS_ATTRIBUTE_FILENAME				(2)			// The UTF-8 filename.

#define ESFS_INDIRECTION_DIRECT				(1)			// The data is stored in the attribute.
#define ESFS_INDIRECTION_L1				(2)			// The attribute contains a extent list that points to the data.

#include <grub/err.h>
#include <grub/file.h>
#include <grub/mm.h>
#include <grub/misc.h>
#include <grub/disk.h>
#include <grub/dl.h>
#include <grub/types.h>
#include <grub/fshelp.h>
#include <grub/safemath.h>

GRUB_MOD_LICENSE ("GPLv3+");

struct grub_esfs_unique_id {
  grub_uint8_t d[16];
};

struct grub_esfs_direntry_ref {
  /*  0 */ grub_uint64_t block;					// The block containing the directory entry.
  /*  8 */ grub_uint32_t offset_into_block;				// Offset into the block to find the directory entry.
  /* 12 */ grub_uint32_t _unused;					// Unused.
};

struct grub_esfs_superblock
{
  /*   0 */ char signature[16];					// The filesystem signature; should be ESFS_SIGNATURE_STRING.
  /*  16 */ char volume_name[ESFS_MAXIMUM_VOLUME_NAME_LENGTH];	// The name of the volume.
	
  /*  48 */ grub_uint16_t requiredReadVersion;				// If this is greater than the driver's version, then the filesystem cannot be read.
  /*  50 */ grub_uint16_t requiredWriteVersion;			// If this is greater than the driver's version, then the filesystem cannot be written.
	
  /*  52 */ grub_uint32_t checksum;					// CRC-32 checksum of Superblock.
  /*  56 */ grub_uint8_t mounted;					// Non-zero to indicate that the volume is mounted, or was not properly unmounted.
  /*  57 */ grub_uint8_t _unused2[7];
	
  /*  64 */ grub_uint64_t blockSize;					// The size of a block on the volume.
  /*  72 */ grub_uint64_t blockCount;					// The number of blocks on the volume.
  /*  80 */ grub_uint64_t blocksUsed;					// The number of blocks that are in use.
	
  /*  88 */ grub_uint32_t blocksPerGroup;				// The number of blocks in a group.
  /*  92 */ grub_uint8_t _unused3[4];
  /*  96 */ grub_uint64_t groupCount;					// The number of groups on the volume.
  /* 104 */ grub_uint64_t blocksPerGroupBlockBitmap;			// The number of blocks used to a store a group's block bitmap.
  /* 112 */ grub_uint64_t gdtFirstBlock;				// The first block in the group descriptor table.
  /* 120 */ grub_uint64_t directoryEntriesPerBlock;			// The number of directory entries in a block.
  /* 128 */ grub_uint64_t _unused0;					// Unused.
	
  /* 136 */ struct grub_esfs_unique_id identifier;			// The unique identifier for the volume.
  /* 152 */ struct grub_esfs_unique_id osInstallation;			// The unique identifier of the Essence installation this volume was made for. All zero for a non-installation volume.
  /* 168 */ struct grub_esfs_unique_id nextIdentifier;			// The identifier to give to the next created file.

  /* 184 */ struct grub_esfs_direntry_ref kernel;			// The kernel. For convenient access by the bootloader.
  /* 200 */ struct grub_esfs_direntry_ref root;				// The root directory.

  /* 216 */ grub_uint8_t _unused1[8192 - 216];				// Unused.
};

struct grub_esfs_direntry {
  /*  0 */ char signature[8];					// Must be ESFS_DIRECTORY_ENTRY_SIGNATURE.
  /*  8 */ struct grub_esfs_unique_id identifier;			// Identifier of the node.
  /* 24 */ grub_uint32_t checksum;					// CRC-32 checksum of DirectoryEntry.
  /* 28 */ grub_uint16_t attributeOffset;				// Offset to the first attribute.
  /* 30 */ grub_uint8_t nodeType;					// Node type.
  /* 31 */ grub_uint8_t attributeCount;				// The number of attributes in the list.
  /* 32 */ grub_uint64_t creationTime, accessTime, modificationTime;	// Timekeeping. In microseconds since 1st January 1970.
  /* 56 */ grub_uint64_t fileSize;					// The amount of data referenced by the data attribute in bytes.
  /* 64 */ struct grub_esfs_unique_id parent;			// Identifier of the parent directory.
  /* 80 */ struct grub_esfs_unique_id contentType;                // Identifier of the file content type.

#define ESFS_ATTRIBUTE_OFFSET (96)
  /* 96 */ grub_uint8_t attributes[1024 - ESFS_ATTRIBUTE_OFFSET];	// Attribute list.
};

struct grub_esfs_attribute {
  /*  0 */ grub_uint16_t type;
  /*  2 */ grub_uint16_t size;						// The size in bytes. Must be 8 byte aligned.
};

struct grub_esfs_attribute_filename {
  /*  0 */ grub_uint16_t type;						// ESFS_ATTRIBUTE_FILENAME.
  /*  2 */ grub_uint16_t size;						// The size in bytes. Must be 8 byte aligned.
  /*  4 */ grub_uint16_t length;					// The length of the filename in bytes.
  /*  6 */ grub_uint16_t _unused;					// Unused.
  /*  8 */ char filename[0];					// The UTF-8 filename.
};

struct grub_esfs_attribute_data {
  /*  0 */ grub_uint16_t type;						// ESFS_ATTRIBUTE_DATA.
  /*  2 */ grub_uint16_t size;						// The size in bytes. Must be 8 byte aligned.
  /*  4 */ grub_uint8_t indirection;					// The indirection used to access the data.
  /*  5 */ grub_uint8_t dataOffset;					// The offset into the attribute where the data or extent list can be found.
  /*  6 */ grub_uint16_t count;					// The number of data bytes in the attribute, or extents in the list.
  /*  8 */ grub_uint16_t _unused[12];					// Unused.
  /* 32 */ grub_uint8_t data[0];					// The data or extent list.
};

struct grub_fshelp_node
{
  struct grub_esfs_data *data;
  struct grub_esfs_direntry direntry;
};

/* Information about a "mounted" ext2 filesystem.  */
struct grub_esfs_data
{
  struct grub_esfs_superblock sblock;
  grub_uint64_t bsize;
  grub_disk_t disk;
  struct grub_esfs_direntry *direntry;
  struct grub_fshelp_node diropen;
};

static grub_dl_t my_mod;



static struct grub_esfs_attribute *
get_direntry_attribute(struct grub_esfs_direntry *direntry, grub_uint16_t attrid, grub_size_t minsize)
{
  grub_uint32_t off = grub_le_to_cpu16 (direntry->attributeOffset);
  while (off <= sizeof(*direntry) - sizeof (struct grub_esfs_attribute))
    {
      // Check alignment
      if (off & 7)
	return NULL;
      struct grub_esfs_attribute *attr = (struct grub_esfs_attribute *) ((grub_uint64_t *) direntry + (off / 8));
      
      if (grub_le_to_cpu16 (attr->size) < sizeof (*attr) || grub_le_to_cpu16 (attr->size) + off > sizeof (*direntry))
	return NULL;

      if (grub_le_to_cpu16 (attr->type) == attrid && grub_le_to_cpu16(attr->size) >= minsize)
	return attr;

      off += grub_le_to_cpu16(attr->size);
    }

  return NULL;
}

/* Read LEN bytes from the file described by DATA starting with byte
   POS.  Return the amount of read bytes in READ.  */
static grub_ssize_t
grub_esfs_read_file (grub_fshelp_node_t node,
		     grub_disk_read_hook_t read_hook, void *read_hook_data,
		     grub_off_t pos, grub_size_t len, char *buf)
{
  struct grub_esfs_attribute_data *d = (struct grub_esfs_attribute_data *) get_direntry_attribute (&node->direntry, ESFS_ATTRIBUTE_DATA, sizeof(struct grub_esfs_attribute_data));
  if (!d)
    {
      grub_error(GRUB_ERR_BAD_FS, "extents are missing");
      return -1;
    }

  if (d->dataOffset > grub_le_to_cpu16(d->size))
    {
      grub_error(GRUB_ERR_BAD_FS, "data offset is too large");
      return -1;
    }

  if (pos > grub_le_to_cpu64(node->direntry.fileSize))
    return -1;
  
  if (len + pos > grub_le_to_cpu64(node->direntry.fileSize))
    len = grub_le_to_cpu64(node->direntry.fileSize) - pos;

  grub_uint32_t data_size = grub_le_to_cpu16(d->size) - d->dataOffset;

  if (d->indirection == ESFS_INDIRECTION_DIRECT)
    {
      grub_uint32_t max_size = grub_le_to_cpu16(d->count);
      if (max_size < data_size)
	max_size = data_size;
      if (pos > max_size)
	return -1;
      if (len > max_size - pos)
	len = max_size - pos;
      // TODO: hook?
      grub_memcpy(buf, (grub_uint8_t *) d + d->dataOffset + pos, len);
      return len;
    }
  if (d->indirection != ESFS_INDIRECTION_L1)
    {
      grub_error(GRUB_ERR_BAD_FS, "unknown redirection");
      return -1;
    }

  grub_uint32_t ext_off = d->dataOffset;
  grub_off_t cur_pos = 0;
  grub_size_t already_read = 0;
  grub_uint64_t cur_start = 0;
  int extnum = 0;

  for (; already_read < len && extnum < grub_le_to_cpu16(d->count); extnum++)
    {
      grub_uint8_t header = ((grub_uint8_t *) d)[ext_off++];
      grub_uint8_t startBytes = ((header >> 0) & 7) + 1;
      grub_uint8_t countBytes = ((header >> 3) & 7) + 1;
      unsigned i;
      if (ext_off + startBytes + countBytes > data_size)
	return already_read;
      grub_uint64_t start = 0, count = 0;
      if (((grub_uint8_t *) d)[ext_off] & 0x80)
	start = (grub_uint64_t) -1;
      for (i = 0; i < startBytes; i++)
	start = (start << 8) | ((grub_uint8_t *) d)[ext_off++];
      for (i = 0; i < countBytes; i++)
	count = (count << 8) | ((grub_uint8_t *) d)[ext_off++];
      cur_start += start;
      grub_uint64_t count_bytes = count * node->data->bsize;
      if (cur_pos + count_bytes < pos)
	{
	  cur_pos += count_bytes;
	  continue;
	}
      grub_off_t add_off = cur_pos < pos ? pos - cur_pos : 0;
      grub_size_t to_read = len - already_read;
      if (to_read > count_bytes - add_off)
	to_read = count_bytes - add_off;

      node->data->disk->read_hook = read_hook;
      node->data->disk->read_hook_data = read_hook_data;
      if (grub_disk_read (node->data->disk,
			  cur_start * (node->data->bsize >> 9) + (add_off >> 9),
			  add_off & 0x1ff, to_read, buf))
	return -1;
      node->data->disk->read_hook = 0;
      already_read += to_read;
      cur_pos += count_bytes;
    }

  return already_read;

}

static int
esfs_check_direntry (struct grub_esfs_direntry *entry)
{
  if (grub_memcmp(entry->signature, ESFS_DIRECTORY_ENTRY_SIGNATURE, sizeof(entry->signature)) != 0)
    return 0;

  // TODO: checksum?

  return 1;
}

static struct grub_esfs_data *
grub_esfs_mount (grub_disk_t disk)
{
  struct grub_esfs_data *data;

  data = grub_malloc (sizeof (struct grub_esfs_data));
  if (!data)
    return 0;

  /* Read the superblock.  */
  grub_disk_read (disk, 16, 0, sizeof (struct grub_esfs_superblock),
                  &data->sblock);
  if (grub_errno)
    goto fail;

  // TODO: checksum?
  /* Make sure this is an ext2 filesystem.  */
  if (grub_memcmp(data->sblock.signature, ESFS_SIGNATURE_STRING, sizeof(data->sblock.signature)) != 0
      || grub_le_to_cpu16 (data->sblock.requiredReadVersion) > ESFS_DRIVER_VERSION
      || data->sblock.blockSize == 0
      /* We don't want to deal with blocks overflowing int32 or not being divisible by 512. */
      || data->sblock.blockSize & grub_cpu_to_le64_compile_time(~0xffffe00)
      || data->sblock.blockCount == 0)
    {
      grub_error (GRUB_ERR_BAD_FS, "not an esfs filesystem");
      goto fail;
    }

  data->bsize = grub_le_to_cpu64(data->sblock.blockSize);

  data->disk = disk;

  data->diropen.data = data;

  data->direntry = &data->diropen.direntry;

  if (grub_disk_read (data->disk,
		      grub_le_to_cpu64(data->sblock.root.block) * (data->bsize >> 9),
		      grub_le_to_cpu32(data->sblock.root.offset_into_block),
		      sizeof (struct grub_esfs_direntry), data->direntry))
    goto fail;

  if (!esfs_check_direntry(data->direntry)) {
    grub_error(GRUB_ERR_BAD_FS, "incorrect directory signature");
    goto fail;
  }

  return data;

 fail:
  if (grub_errno == GRUB_ERR_OUT_OF_RANGE)
    grub_error (GRUB_ERR_BAD_FS, "not an esfs filesystem");

  grub_free (data);
  return 0;
}

static int
grub_esfs_iterate_dir (grub_fshelp_node_t dir,
		       grub_fshelp_iterate_dir_hook_t hook, void *hook_data)
{
  grub_uint64_t fpos = 0;
  struct grub_fshelp_node *diro = (struct grub_fshelp_node *) dir;

  if (diro->direntry.nodeType != ESFS_NODE_TYPE_DIRECTORY)
    return grub_error (GRUB_ERR_BAD_FILE_TYPE, N_("not a directory"));

  grub_uint64_t dirSize = grub_le_to_cpu64 (diro->direntry.fileSize);

  if (dirSize >= 0x80000000ULL)
    return grub_error (GRUB_ERR_BAD_FS, "directory too large");

  /* Search the file.  */
  while (fpos < dirSize)
    {
      struct grub_fshelp_node *fdiro;

      fdiro = grub_malloc (sizeof (struct grub_fshelp_node));
      if (! fdiro)
	return 0;

      fdiro->data = diro->data;

      grub_esfs_read_file (diro, 0, 0, fpos, sizeof (struct grub_esfs_direntry),
			   (char *) &fdiro->direntry);
      if (grub_errno) {
	grub_free (fdiro);
	return 0;
      }
      fpos += sizeof (struct grub_esfs_direntry);

      if (!esfs_check_direntry(&fdiro->direntry))
	{
	  grub_free (fdiro);
	  continue;
	}

      struct grub_esfs_attribute_filename *filename_attr
	= (struct grub_esfs_attribute_filename *)
	get_direntry_attribute(&fdiro->direntry,
			       ESFS_ATTRIBUTE_FILENAME,
			       sizeof(struct grub_esfs_attribute_filename));
      if (!filename_attr || grub_cpu_to_le16(filename_attr->size) < 8
	  || grub_cpu_to_le16(filename_attr->length)
	  > grub_cpu_to_le16(filename_attr->size) - 8) {
	grub_free (fdiro);
	continue;
      }

      enum grub_fshelp_filetype type = GRUB_FSHELP_UNKNOWN;
      switch (fdiro->direntry.nodeType) {
      case ESFS_NODE_TYPE_DIRECTORY:
	type = GRUB_FSHELP_DIR;
	break;
      case ESFS_NODE_TYPE_FILE:
	type = GRUB_FSHELP_REG;
	break;
      default:
	grub_free (fdiro);
	continue;
      }

      char *filename = grub_strndup(filename_attr->filename, filename_attr->length);
      if (!filename) {
	grub_free (fdiro);
	continue;
      }

      if (hook (filename, type, fdiro, hook_data))
	return 1;

      grub_free (filename);
    }

  return 0;
}

/* Open a file named NAME and initialize FILE.  */
static grub_err_t
grub_esfs_open (struct grub_file *file, const char *name)
{
  struct grub_esfs_data *data;
  struct grub_fshelp_node *fdiro = 0;
  grub_err_t err;

  grub_dl_ref (my_mod);

  data = grub_esfs_mount (file->device->disk);
  if (! data)
    {
      err = grub_errno;
      goto fail;
    }

  err = grub_fshelp_find_file (name, &data->diropen, &fdiro,
			       grub_esfs_iterate_dir, NULL, GRUB_FSHELP_REG);
  if (err)
    goto fail;

  grub_memcpy (data->direntry, &fdiro->direntry, sizeof (struct grub_esfs_direntry));
  grub_free (fdiro);

  file->size = grub_le_to_cpu64 (data->direntry->fileSize);
  file->data = data;
  file->offset = 0;

  return 0;

 fail:
  if (fdiro != &data->diropen)
    grub_free (fdiro);
  grub_free (data);

  grub_dl_unref (my_mod);

  return err;
}

static grub_err_t
grub_esfs_close (grub_file_t file)
{
  grub_free (file->data);

  grub_dl_unref (my_mod);

  return GRUB_ERR_NONE;
}

/* Read LEN bytes data from FILE into BUF.  */
static grub_ssize_t
grub_esfs_read (grub_file_t file, char *buf, grub_size_t len)
{
  struct grub_esfs_data *data = (struct grub_esfs_data *) file->data;

  return grub_esfs_read_file (&data->diropen,
			      file->read_hook, file->read_hook_data,
			      file->offset, len, buf);
}

/* Context for grub_ext2_dir.  */
struct grub_esfs_dir_ctx
{
  grub_fs_dir_hook_t hook;
  void *hook_data;
  struct grub_esfs_data *data;
};

/* Helper for grub_esfs_dir.  */
static int
grub_esfs_dir_iter (const char *filename, enum grub_fshelp_filetype filetype __attribute__((unused)),
		    grub_fshelp_node_t node, void *data)
{
  struct grub_esfs_dir_ctx *ctx = data;
  struct grub_dirhook_info info;

  grub_memset (&info, 0, sizeof (info));
  info.mtimeset = 1;
  info.mtime = grub_divmod64 (grub_le_to_cpu64 (node->direntry.modificationTime),
			      1000000, NULL);

  info.dir = node->direntry.nodeType == ESFS_NODE_TYPE_DIRECTORY;
  grub_free (node);
  return ctx->hook (filename, &info, ctx->hook_data);
}

static grub_err_t
grub_esfs_dir (grub_device_t device, const char *path, grub_fs_dir_hook_t hook,
	       void *hook_data)
{
  struct grub_esfs_dir_ctx ctx = {
    .hook = hook,
    .hook_data = hook_data
  };
  struct grub_fshelp_node *fdiro = 0;

  grub_dl_ref (my_mod);

  ctx.data = grub_esfs_mount (device->disk);
  if (! ctx.data)
    goto fail;

  fdiro = &ctx.data->diropen;

  grub_fshelp_find_file (path, &ctx.data->diropen, &fdiro,
			 grub_esfs_iterate_dir, NULL,
			 GRUB_FSHELP_DIR);
  if (grub_errno)
    goto fail;

  grub_esfs_iterate_dir (fdiro, grub_esfs_dir_iter, &ctx);

 fail:
  if (fdiro != &ctx.data->diropen)
    grub_free (fdiro);
  grub_free (ctx.data);

  grub_dl_unref (my_mod);

  return grub_errno;
}

static grub_err_t
grub_esfs_label (grub_device_t device, char **label)
{
  struct grub_esfs_data *data;
  grub_disk_t disk = device->disk;

  grub_dl_ref (my_mod);

  data = grub_esfs_mount (disk);
  if (data)
    *label = grub_strndup (data->sblock.volume_name,
			   sizeof (data->sblock.volume_name));
  else
    *label = NULL;

  grub_dl_unref (my_mod);

  grub_free (data);

  return grub_errno;
}

static grub_err_t
grub_esfs_uuid (grub_device_t device, char **uuid)
{
  struct grub_esfs_data *data;
  grub_disk_t disk = device->disk;

  grub_dl_ref (my_mod);

  data = grub_esfs_mount (disk);
  if (data)
    {
      *uuid = grub_malloc(40);
      if (*uuid) {
	int i;
	for (i = 0; i < 16; i++) {
	  grub_snprintf(*uuid + 2 * i, 3, "%02x", data->sblock.identifier.d[i] & 0xff);
	}
	(*uuid)[32] = '\0';
      }
    }
  else
    *uuid = NULL;

  grub_dl_unref (my_mod);

  grub_free (data);

  return grub_errno;
}


static struct grub_fs grub_esfs_fs =
  {
    .name = "esfs",
    .fs_dir = grub_esfs_dir,
    .fs_open = grub_esfs_open,
    .fs_read = grub_esfs_read,
    .fs_close = grub_esfs_close,
    .fs_label = grub_esfs_label,
    .fs_uuid = grub_esfs_uuid,
#ifdef GRUB_UTIL
    .reserved_first_sector = 1,
    .blocklist_install = 1,
#endif
    .next = 0
  };

GRUB_MOD_INIT(esfs)
{
  grub_fs_register (&grub_esfs_fs);
  my_mod = mod;
}

GRUB_MOD_FINI(esfs)
{
  grub_fs_unregister (&grub_esfs_fs);
}
