/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 1999,2000,2001,2002,2003,2004,2009,2010,2011  Free Software Foundation, Inc.
 *  Copyright 2010  Sun Microsystems, Inc.
 *  Copyright (c) 2012 by Delphix. All rights reserved.
 *
 *  GRUB is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
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
/*
 * The zfs plug-in routines for GRUB are:
 *
 * zfs_mount() - locates a valid uberblock of the root pool and reads
 *		in its MOS at the memory address MOS.
 *
 * zfs_open() - locates a plain file object by following the MOS
 *		and places its dnode at the memory address DNODE.
 *
 * zfs_read() - read in the data blocks pointed by the DNODE.
 *
 */

#include <grub/err.h>
#include <grub/file.h>
#include <grub/mm.h>
#include <grub/misc.h>
#include <grub/disk.h>
#include <grub/partition.h>
#include <grub/dl.h>
#include <grub/types.h>
#include <grub/zfs/zfs.h>
#include <grub/zfs/zio.h>
#include <grub/zfs/dnode.h>
#include <grub/zfs/uberblock_impl.h>
#include <grub/zfs/vdev_impl.h>
#include <grub/zfs/zio_checksum.h>
#include <grub/zfs/zap_impl.h>
#include <grub/zfs/zap_leaf.h>
#include <grub/zfs/zfs_znode.h>
#include <grub/zfs/dmu.h>
#include <grub/zfs/dmu_objset.h>
#include <grub/zfs/sa_impl.h>
#include <grub/zfs/dsl_dir.h>
#include <grub/zfs/dsl_dataset.h>
#include <grub/deflate.h>
#include <grub/crypto.h>
#include <grub/i18n.h>
#include <grub/safemath.h>

#include <zstd.h>

GRUB_MOD_LICENSE ("GPLv3+");

#define	ZPOOL_PROP_BOOTFS		"bootfs"

/*
 * For nvlist manipulation. (from nvpair.h)
 */
#define	NV_ENCODE_NATIVE	0
#define	NV_ENCODE_XDR		1
#define	NV_BIG_ENDIAN	        0
#define	NV_LITTLE_ENDIAN	1
#define	DATA_TYPE_UINT64	8
#define	DATA_TYPE_STRING	9
#define	DATA_TYPE_NVLIST	19
#define	DATA_TYPE_NVLIST_ARRAY	20

#define DNODE_NUM_MASK 0xffffffffffffULL

#ifndef GRUB_UTIL
static grub_dl_t my_mod;
#endif

#define	P2PHASE(x, align)		((x) & ((align) - 1))

static inline grub_disk_addr_t
DVA_OFFSET_TO_PHYS_SECTOR (grub_disk_addr_t offset)
{
  return ((offset + VDEV_LABEL_START_SIZE) >> SPA_MINBLOCKSHIFT);
}

/*
 * FAT ZAP data structures
 */
#define	ZFS_CRC64_POLY 0xC96C5795D7870F42ULL	/* ECMA-182, reflected form */
static inline grub_uint64_t
ZAP_HASH_IDX (grub_uint64_t hash, grub_uint64_t n)
{
  return (((n) == 0) ? 0 : ((hash) >> (64 - (n))));
}

#define	CHAIN_END	0xffff	/* end of the chunk chain */

/*
 * The amount of space within the chunk available for the array is:
 * chunk size - space for type (1) - space for next pointer (2)
 */
#define	ZAP_LEAF_ARRAY_BYTES (ZAP_LEAF_CHUNKSIZE - 3)

static inline int
ZAP_LEAF_HASH_SHIFT (int bs)
{
  return bs - 5;
}

static inline int
ZAP_LEAF_HASH_NUMENTRIES (int bs)
{
  return 1 << ZAP_LEAF_HASH_SHIFT(bs);
}

static inline grub_size_t
LEAF_HASH (int bs, grub_uint64_t h, zap_leaf_phys_t *l)
{
  return ((ZAP_LEAF_HASH_NUMENTRIES (bs)-1)
	  & ((h) >> (64 - ZAP_LEAF_HASH_SHIFT (bs) - l->l_hdr.lh_prefix_len)));
}

/*
 * The amount of space available for chunks is:
 * block size shift - hash entry size (2) * number of hash
 * entries - header space (2*chunksize)
 */
static inline int
ZAP_LEAF_NUMCHUNKS (int bs)
{
  return (((1U << bs) - 2 * ZAP_LEAF_HASH_NUMENTRIES (bs)) /
	  ZAP_LEAF_CHUNKSIZE - 2);
}

/*
 * The chunks start immediately after the hash table.  The end of the
 * hash table is at l_hash + HASH_NUMENTRIES, which we simply cast to a
 * chunk_t.
 */
static inline zap_leaf_chunk_t *
ZAP_LEAF_CHUNK (zap_leaf_phys_t *l, int bs, int idx)
{
  grub_properly_aligned_t *l_entries;

  l_entries = (grub_properly_aligned_t *) ALIGN_UP((grub_addr_t)l->l_hash, sizeof (grub_properly_aligned_t));
  return &((zap_leaf_chunk_t *) (l_entries
				 + (ZAP_LEAF_HASH_NUMENTRIES(bs) * 2)
				 / sizeof (grub_properly_aligned_t)))[idx];
}

static inline struct zap_leaf_entry *
ZAP_LEAF_ENTRY(zap_leaf_phys_t *l, int bs, int idx)
{
  return &ZAP_LEAF_CHUNK(l, bs, idx)->l_entry;
}


/*
 * Decompression Entry - lzjb & lz4
 */

extern grub_err_t lzjb_decompress (void *, void *, grub_size_t, grub_size_t);

extern grub_err_t lz4_decompress (void *, void *, grub_size_t, grub_size_t);

typedef grub_err_t zfs_decomp_func_t (void *s_start, void *d_start,
				      grub_size_t s_len, grub_size_t d_len);
typedef struct decomp_entry
{
  const char *name;
  zfs_decomp_func_t *decomp_func;
} decomp_entry_t;

/*
 * Signature for checksum functions.
 */
typedef void zio_checksum_t(const void *data, grub_uint64_t size,
			    grub_zfs_endian_t endian, zio_cksum_t *zcp);

/*
 * Information about each checksum function.
 */
typedef struct zio_checksum_info {
  zio_checksum_t	*ci_func; /* checksum function for each byteorder */
  int		ci_correctable;	/* number of correctable bits	*/
  int		ci_eck;		/* uses zio embedded checksum? */
  const char		*ci_name;	/* descriptive name */
} zio_checksum_info_t;

struct grub_zfs_device_desc
{
  enum { DEVICE_LEAF, DEVICE_MIRROR, DEVICE_RAIDZ } type;
  grub_uint64_t id;
  grub_uint64_t guid;
  unsigned ashift;
  unsigned max_children_ashift;

  /* Valid only for non-leafs.  */
  unsigned n_children;
  struct grub_zfs_device_desc *children;

  /* Valid only for RAIDZ.  */
  unsigned nparity;

  /* Valid only for leaf devices.  */
  grub_device_t dev;
  grub_disk_addr_t vdev_phys_sector;
  uberblock_t current_uberblock;
  int original;
};

struct subvolume
{
  dnode_phys_t mdn;
  grub_uint64_t obj;
  grub_uint64_t case_insensitive;
  grub_size_t nkeys;
  struct
  {
    grub_crypto_cipher_handle_t cipher;
    grub_uint64_t txg;
    grub_uint64_t algo;
  } *keyring;

  struct grub_zfs_datto_key key_datto;

  int is_datto_encrypted;
};

struct grub_zfs_data
{
  /* cache for a file block of the currently zfs_open()-ed file */
  char *file_buf;
  grub_uint64_t file_start;
  grub_uint64_t file_end;

  /* cache for a dnode block */
  dnode_phys_t *dnode_buf;
  dnode_phys_t *dnode_mdn;
  grub_uint64_t dnode_start;
  grub_uint64_t dnode_end;

  dnode_phys_t mos;
  dnode_phys_t dnode;
  struct subvolume subvol;

  struct grub_zfs_device_desc *devices_attached;
  unsigned n_devices_attached;
  unsigned n_devices_allocated;
  struct grub_zfs_device_desc *device_original;

  uberblock_t current_uberblock;

  grub_uint64_t guid;
};

/* Context for grub_zfs_dir.  */
struct grub_zfs_dir_ctx
{
  grub_fs_dir_hook_t hook;
  void *hook_data;
  struct grub_zfs_data *data;
};

struct grub_zfs_decryptor *grub_zfs_decrypt = NULL;

struct grub_zfs_crypt_datto_data
{
  grub_uint8_t *iv;
  grub_size_t ivlen;
  grub_uint8_t *mac;
  grub_size_t maclen;
  grub_uint8_t *master;
  grub_size_t masterlen;
  grub_uint8_t *hmac;
  grub_size_t hmaclen;
};
/*
 * List of pool features that the grub implementation of ZFS supports for
 * read. Note that features that are only required for write do not need
 * to be listed here since grub opens pools in read-only mode.
 */
#define MAX_SUPPORTED_FEATURE_STRLEN 50
static const char *spa_feature_names[] = {
  "org.illumos:lz4_compress",
  "com.delphix:hole_birth",
  "com.delphix:embedded_data",
  "com.delphix:extensible_dataset",
  "org.open-zfs:large_blocks",
  "com.klarasystems:vdev_zaps_v2",
  "com.delphix:head_errlog",
  "org.freebsd:zstd_compress",
  "com.datto:encryption",
  NULL
};

static int
check_feature(const char *name, grub_uint64_t val, struct grub_zfs_dir_ctx *ctx);
static grub_err_t
check_mos_features(dnode_phys_t *mosmdn_phys, struct grub_zfs_data* data );

static grub_err_t
zlib_decompress (void *s, void *d,
		 grub_size_t slen, grub_size_t dlen)
{
  if (grub_zlib_decompress (s, slen, 0, d, dlen) == (grub_ssize_t) dlen)
    return GRUB_ERR_NONE;

  if (!grub_errno)
    grub_error (GRUB_ERR_BAD_COMPRESSED_DATA,
		"premature end of compressed");
  return grub_errno;
}

static grub_err_t
zstd_decompress (void *ibuf, void *obuf, grub_size_t isize,
		 grub_size_t osize)
{
  grub_size_t zstd_ret;
  grub_uint8_t *byte_buf = (grub_uint8_t *) ibuf;

  if (isize < 8)
      return grub_error (GRUB_ERR_BAD_COMPRESSED_DATA, "zstd data too short");

  grub_uint32_t c_len = grub_be_to_cpu32(grub_get_unaligned32(byte_buf));

  if (c_len > isize - 8)
      return grub_error (GRUB_ERR_BAD_COMPRESSED_DATA, "zstd data announced size overflow");

  /* Fix magic number.  */
  byte_buf[4] = 0x28;
  byte_buf[5] = 0xb5;
  byte_buf[6] = 0x2f;
  byte_buf[7] = 0xfd;
  zstd_ret = ZSTD_decompress (obuf, osize, byte_buf + 4, c_len + 4);

  if (ZSTD_isError (zstd_ret))
    return grub_error (GRUB_ERR_BAD_COMPRESSED_DATA, "zstd data corrupted (error %d)", (int) zstd_ret);

  return GRUB_ERR_NONE;
}

static grub_err_t
zle_decompress (void *s, void *d,
		grub_size_t slen, grub_size_t dlen)
{
  grub_uint8_t *iptr, *optr;
  grub_size_t clen;
  for (iptr = s, optr = d; iptr < (grub_uint8_t *) s + slen
	 && optr < (grub_uint8_t *) d + dlen;)
    {
      if (*iptr & 0x80)
	clen = ((*iptr) & 0x7f) + 0x41;
      else
	clen = ((*iptr) & 0x3f) + 1;
      if ((grub_ssize_t) clen > (grub_uint8_t *) d + dlen - optr)
	clen = (grub_uint8_t *) d + dlen - optr;
      if (*iptr & 0x40 || *iptr & 0x80)
	{
	  grub_memset (optr, 0, clen);
	  iptr++;
	  optr += clen;
	  continue;
	}
      if ((grub_ssize_t) clen > (grub_uint8_t *) s + slen - iptr - 1)
	clen = (grub_uint8_t *) s + slen - iptr - 1;
      grub_memcpy (optr, iptr + 1, clen);
      optr += clen;
      iptr += clen + 1;
    }
  if (optr < (grub_uint8_t *) d + dlen)
    grub_memset (optr, 0, (grub_uint8_t *) d + dlen - optr);
  return GRUB_ERR_NONE;
}

static decomp_entry_t decomp_table[ZIO_COMPRESS_FUNCTIONS] = {
  {"inherit", NULL},		/* ZIO_COMPRESS_INHERIT */
  {"on", lzjb_decompress},	/* ZIO_COMPRESS_ON */
  {"off", NULL},		/* ZIO_COMPRESS_OFF */
  {"lzjb", lzjb_decompress},	/* ZIO_COMPRESS_LZJB */
  {"empty", NULL},		/* ZIO_COMPRESS_EMPTY */
  {"gzip-1", zlib_decompress},  /* ZIO_COMPRESS_GZIP1 */
  {"gzip-2", zlib_decompress},  /* ZIO_COMPRESS_GZIP2 */
  {"gzip-3", zlib_decompress},  /* ZIO_COMPRESS_GZIP3 */
  {"gzip-4", zlib_decompress},  /* ZIO_COMPRESS_GZIP4 */
  {"gzip-5", zlib_decompress},  /* ZIO_COMPRESS_GZIP5 */
  {"gzip-6", zlib_decompress},  /* ZIO_COMPRESS_GZIP6 */
  {"gzip-7", zlib_decompress},  /* ZIO_COMPRESS_GZIP7 */
  {"gzip-8", zlib_decompress},  /* ZIO_COMPRESS_GZIP8 */
  {"gzip-9", zlib_decompress},  /* ZIO_COMPRESS_GZIP9 */
  {"zle", zle_decompress},      /* ZIO_COMPRESS_ZLE   */
  {"lz4", lz4_decompress},      /* ZIO_COMPRESS_LZ4   */
  {"zstd", zstd_decompress},    /* ZIO_COMPRESS_ZSTD   */
};

static grub_err_t zio_read_data (const blkptr_t * bp,
				 void *buf, struct grub_zfs_data *data);

static int
fill_crypt_datto_data (const void *name,
		    grub_size_t namelen __attribute__ ((unused)),
		    const void *val_in,
		    grub_size_t nelem,
		    grub_size_t elemsize,
		    void *data_in)
{
  struct grub_zfs_crypt_datto_data *data = data_in;
  if (grub_strcmp(name, "DSL_CRYPTO_IV") == 0 && elemsize == 1)
    {
      data->ivlen = nelem;
      data->iv = grub_malloc(nelem);
      if (data->iv)
	grub_memcpy(data->iv, val_in, nelem);
    }
  else if (grub_strcmp(name, "DSL_CRYPTO_MAC") == 0 && elemsize == 1)
    {
      data->maclen = nelem;
      data->mac = grub_malloc(nelem);
      if (data->mac)
	grub_memcpy(data->mac, val_in, nelem);
    }
  else if (grub_strcmp(name, "DSL_CRYPTO_MASTER_KEY_1") == 0 && elemsize == 1)
    {
      data->masterlen = nelem;
      data->master = grub_malloc(nelem);
      if (data->master)
	grub_memcpy(data->master, val_in, nelem);
    }
  else if (grub_strcmp(name, "DSL_CRYPTO_HMAC_KEY_1") == 0 && elemsize == 1)
    {
      data->hmaclen = nelem;
      data->hmac = grub_malloc(nelem);
      if (data->hmac)
	grub_memcpy(data->hmac, val_in, nelem);
    }

  return 0;
}

/*
 * Our own version of log2().  Same thing as highbit()-1.
 */
static int
zfs_log2 (grub_uint64_t num)
{
  int i = 0;

  while (num > 1)
    {
      i++;
      num = num >> 1;
    }

  return i;
}

/* Checksum Functions */
static void
zio_checksum_off (const void *buf __attribute__ ((unused)),
		  grub_uint64_t size __attribute__ ((unused)),
		  grub_zfs_endian_t endian __attribute__ ((unused)),
		  zio_cksum_t * zcp)
{
  ZIO_SET_CHECKSUM (zcp, 0, 0, 0, 0);
}

/* Checksum Table and Values */
static zio_checksum_info_t zio_checksum_table[ZIO_CHECKSUM_FUNCTIONS] = {
  {NULL, 0, 0, "inherit"},
  {NULL, 0, 0, "on"},
  {zio_checksum_off, 0, 0, "off"},
  {zio_checksum_SHA256, 1, 1, "label"},
  {zio_checksum_SHA256, 1, 1, "gang_header"},
  {NULL, 0, 0, "zilog"},
  {fletcher_2, 0, 0, "fletcher2"},
  {fletcher_4, 1, 0, "fletcher4"},
  {zio_checksum_SHA256, 1, 0, "SHA256"},
  {NULL, 0, 0, "zilog2"},
  {zio_checksum_SHA256, 1, 0, "SHA256+MAC"},
};

static inline void
grub_zfs_byteswap_u64(grub_uint64_t *val)
{
  *val = grub_swap_bytes64(*val);
}

static inline void
grub_zfs_byteswap_u32(grub_uint32_t *val)
{
  *val = grub_swap_bytes32(*val);
}

static inline void
grub_zfs_byteswap_u16(grub_uint16_t *val)
{
  *val = grub_swap_bytes16(*val);
}

static void
grub_zfs_byteswap_checksum(zio_cksum_t *csum)
{
  for (int i = 0; i < 4; i++)
    grub_zfs_byteswap_u64(&csum->zc_word[i]);
}

static void
grub_zfs_byteswap_blkptr(blkptr_t *bp)
{
  for (int i = 0; i < SPA_DVAS_PER_BP; i++)
    {
      grub_zfs_byteswap_u64(&bp->blk_dva[i].dva_word[0]);
      grub_zfs_byteswap_u64(&bp->blk_dva[i].dva_word[1]);
    }
  grub_zfs_byteswap_u64(&bp->blk_prop);
  grub_zfs_byteswap_u64(&bp->blk_pad[0]);
  grub_zfs_byteswap_u64(&bp->blk_pad[1]);
  grub_zfs_byteswap_u64(&bp->blk_phys_birth);
  grub_zfs_byteswap_u64(&bp->blk_birth);
  grub_zfs_byteswap_u64(&bp->blk_fill);

  grub_zfs_byteswap_checksum(&bp->blk_cksum);
}

static void
grub_zfs_byteswap_uberblock(uberblock_t *ub)
{
  grub_zfs_byteswap_u64(&ub->ub_magic);
  grub_zfs_byteswap_u64(&ub->ub_version);
  grub_zfs_byteswap_u64(&ub->ub_txg);
  grub_zfs_byteswap_u64(&ub->ub_guid_sum);
  grub_zfs_byteswap_u64(&ub->ub_timestamp);
  grub_zfs_byteswap_blkptr(&ub->ub_rootbp);
}

static void
grub_zfs_byteswap_type(void *buf, grub_size_t len, grub_uint8_t type);

static void
grub_zfs_byteswap_dnode_proper(dnode_phys_t *dn)
{
  grub_zfs_byteswap_u16(&dn->dn_datablkszsec);
  grub_zfs_byteswap_u16(&dn->dn_bonuslen);
  grub_zfs_byteswap_u64(&dn->dn_maxblkid);
  grub_zfs_byteswap_u64(&dn->dn_used);
  grub_zfs_byteswap_u64(&dn->dn_pad3[0]);
  grub_zfs_byteswap_u64(&dn->dn_pad3[1]);
  grub_zfs_byteswap_u64(&dn->dn_pad3[2]);
  grub_zfs_byteswap_u64(&dn->dn_pad3[3]);

  for (unsigned i = 0; i < dn->dn_nblkptr; i++)
    grub_zfs_byteswap_blkptr(&dn->dn_blkptr[i]);

  if (dn->dn_flags & DNODE_FLAG_SPILL_BLKPTR)
    grub_zfs_byteswap_blkptr(&dn->dn_spill);
}

static void
grub_zfs_byteswap_dnode(dnode_phys_t *dn)
{
  grub_zfs_byteswap_dnode_proper(dn);
  grub_zfs_byteswap_type(dn->dn_bonus, dn->dn_bonuslen, dn->dn_bonustype);
}

static void
grub_zfs_byteswap_objset(objset_phys_t *os)
{
  grub_zfs_byteswap_dnode(&os->os_meta_dnode);
  /* We ignore ZIL header.  */
  grub_zfs_byteswap_u64(&os->os_type);
  grub_zfs_byteswap_u64(&os->os_flags);
  grub_zfs_byteswap_dnode(&os->os_userused_dnode);
  grub_zfs_byteswap_dnode(&os->os_groupused_dnode);
}

static void
grub_zfs_byteswap_u64_array(grub_uint64_t *arr, grub_size_t len)
{
  for (unsigned i = 0; i < len / 8; i++)
    grub_zfs_byteswap_u64(arr + i);
}

static void
grub_zfs_byteswap_mzap(mzap_phys_t *mz, grub_size_t objsize)
{
  grub_zfs_byteswap_u64(&mz->mz_block_type);
  grub_zfs_byteswap_u64(&mz->mz_salt);
  for (unsigned i = 0; i < 6; i++)
    grub_zfs_byteswap_u64(&mz->mz_pad[i]);
  for (unsigned i = 0; i < objsize / MZAP_ENT_LEN - 1; i++)
    {
      grub_zfs_byteswap_u64(&mz->mz_chunk[i].mze_value);
      grub_zfs_byteswap_u32(&mz->mz_chunk[i].mze_cd);
      grub_zfs_byteswap_u16(&mz->mz_chunk[i].mze_pad);
    }
}

static void
grub_zfs_byteswap_zap_leaf(zap_leaf_phys_t *lf, grub_size_t len)
{
  int blksft = zfs_log2 (len);

  grub_zfs_byteswap_u64(&lf->l_hdr.lh_block_type);
  grub_zfs_byteswap_u64(&lf->l_hdr.lh_prefix);
  grub_zfs_byteswap_u32(&lf->l_hdr.lh_magic);
  grub_zfs_byteswap_u16(&lf->l_hdr.lh_nfree);
  grub_zfs_byteswap_u16(&lf->l_hdr.lh_nentries);
  grub_zfs_byteswap_u16(&lf->l_hdr.lh_prefix_len);
  grub_zfs_byteswap_u16(&lf->l_hdr.lh_freelist);

  for (int i = 0; i < ZAP_LEAF_HASH_NUMENTRIES(blksft); i++)
    grub_zfs_byteswap_u16(&lf->l_hash[i]);

  for (int i = 0; i < ZAP_LEAF_NUMCHUNKS (blksft); i++) {
    zap_leaf_chunk_t *lc = ZAP_LEAF_CHUNK(lf, blksft, i);
    struct zap_leaf_entry *le;

    switch (lc->l_free.lf_type) {
    case ZAP_CHUNK_ENTRY:
      le = &lc->l_entry;

      grub_zfs_byteswap_u16(&le->le_next);
      grub_zfs_byteswap_u16(&le->le_name_chunk);
      grub_zfs_byteswap_u16(&le->le_name_length);
      grub_zfs_byteswap_u16(&le->le_value_chunk);
      grub_zfs_byteswap_u16(&le->le_value_length);
      grub_zfs_byteswap_u32(&le->le_cd);
      grub_zfs_byteswap_u64(&le->le_hash);
      break;
    case ZAP_CHUNK_FREE:
      grub_zfs_byteswap_u16(&lc->l_free.lf_next);
      break;
    case ZAP_CHUNK_ARRAY:
      grub_zfs_byteswap_u16(&lc->l_array.la_next);
      break;
    default:
      grub_dprintf("zfs", "Unknown leaf type %d\n", lc->l_free.lf_type);
    }
  }
}

static void
grub_zfs_byteswap_zap(void *zp, grub_size_t len)
{
  grub_uint64_t block_type = *(grub_uint64_t *) zp;
  if (block_type == grub_swap_bytes64_compile_time(ZBT_MICRO))
    grub_zfs_byteswap_mzap(zp, len);
  else if (block_type == grub_swap_bytes64_compile_time(ZBT_HEADER))
    grub_zfs_byteswap_u64_array(zp, len);
  else if (block_type == grub_swap_bytes64_compile_time(ZBT_LEAF))
    grub_zfs_byteswap_zap_leaf(zp, len);
  else
    grub_dprintf ("zfs", "unknown ZAP type");
}

static const dmu_object_byteswap_t byteswap_table[] = {
  DMU_BSWAP_UINT8,  DMU_BSWAP_ZAP,    DMU_BSWAP_UINT64, DMU_BSWAP_UINT8,
  DMU_BSWAP_UINT64, DMU_BSWAP_UINT64, DMU_BSWAP_UINT64, DMU_BSWAP_UINT64,
  DMU_BSWAP_UINT64, DMU_BSWAP_UINT64, DMU_BSWAP_DNODE,  DMU_BSWAP_OBJSET,
  DMU_BSWAP_UINT64, DMU_BSWAP_ZAP,    DMU_BSWAP_ZAP,    DMU_BSWAP_ZAP,

  DMU_BSWAP_UINT64, DMU_BSWAP_ZNODE,  DMU_BSWAP_OLDACL, DMU_BSWAP_UINT8,
  DMU_BSWAP_ZAP,    DMU_BSWAP_ZAP,    DMU_BSWAP_ZAP,    DMU_BSWAP_UINT8,
  DMU_BSWAP_ZAP,    DMU_BSWAP_UINT8,  DMU_BSWAP_UINT64, DMU_BSWAP_ZAP,
  DMU_BSWAP_ZAP,    DMU_BSWAP_UINT8,  DMU_BSWAP_UINT64, DMU_BSWAP_ZAP,

  DMU_BSWAP_ZAP,    DMU_BSWAP_ACL,    DMU_BSWAP_UINT8,  DMU_BSWAP_UINT8,
  DMU_BSWAP_UINT64, DMU_BSWAP_ZAP,    DMU_BSWAP_ZAP,    DMU_BSWAP_ZAP,
  DMU_BSWAP_ZAP,    DMU_BSWAP_ZAP,    DMU_BSWAP_ZAP,    DMU_BSWAP_ZAP,
  DMU_BSWAP_UINT8,  DMU_BSWAP_ZAP,    DMU_BSWAP_ZAP,    DMU_BSWAP_ZAP,

  DMU_BSWAP_ZAP,    DMU_BSWAP_UINT8,  DMU_BSWAP_ZAP,    DMU_BSWAP_UINT64,
  DMU_BSWAP_ZAP,    DMU_BSWAP_UINT64,
};

static dmu_object_byteswap_t
get_byteswap_type(grub_uint8_t type)
{
  if (type & 0x80)
    return type & DMU_OT_BYTESWAP_MASK;

  if (type < ARRAY_SIZE (byteswap_table))
    return byteswap_table[type];
  grub_fatal("Unknown type %d", type);
}

static void
grub_zfs_byteswap_type(void *buf, grub_size_t len, grub_uint8_t type)
{
  /*
	DMU_BSWAP_UINT16,
	DMU_BSWAP_UINT32,
	DMU_BSWAP_ZNODE,
	DMU_BSWAP_OLDACL,
	DMU_BSWAP_ACL,

   */
  switch (get_byteswap_type(type))
    {
    case DMU_BSWAP_UINT8:
      break;
    case DMU_BSWAP_DNODE:
      for (unsigned i = 0; i < len / sizeof(dnode_phys_t); i++)
	grub_zfs_byteswap_dnode((dnode_phys_t *)buf + i);
      break;
    case DMU_BSWAP_OBJSET:
      for (unsigned i = 0; i < len / sizeof(objset_phys_t); i++)
	grub_zfs_byteswap_objset((objset_phys_t *)buf + i);
      break;
    case DMU_BSWAP_ZAP:
      grub_zfs_byteswap_zap(buf, len);
      break;
    case DMU_BSWAP_UINT64:
      grub_zfs_byteswap_u64_array(buf, len);
      break;
    default:
      grub_fatal("Unknown type %d", type);
    }
}

/*
 * zio_checksum_verify: Provides support for checksum verification.
 *
 * Fletcher2, Fletcher4, and SHA256 are supported.
 *
 */
static grub_err_t
zio_checksum_verify (zio_cksum_t zc, grub_uint32_t checksum,
		     grub_zfs_endian_t endian,
		     char *buf, grub_size_t size, int datto_crypt)
{
  zio_eck_t *zec = (zio_eck_t *) (buf + size) - 1;
  zio_checksum_info_t *ci = &zio_checksum_table[checksum];
  zio_cksum_t actual_cksum, expected_cksum;

  if (checksum >= ZIO_CHECKSUM_FUNCTIONS || ci->ci_func == NULL)
    {
      grub_dprintf ("zfs", "unknown checksum function %d\n", checksum);
      return grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET,
			 "unknown checksum function %d", checksum);
    }

  if (ci->ci_eck)
    {
      expected_cksum = zec->zec_cksum;
      zec->zec_cksum = zc;
      ci->ci_func (buf, size, endian, &actual_cksum);
      zec->zec_cksum = expected_cksum;
      zc = expected_cksum;
    }
  else
    ci->ci_func (buf, size, endian, &actual_cksum);

  int cksumlen = checksum != ZIO_CHECKSUM_SHA256_MAC ? 32 : 20;
  if (datto_crypt)
    {
      actual_cksum.zc_word[0] ^= actual_cksum.zc_word[2];
      actual_cksum.zc_word[1] ^= actual_cksum.zc_word[3];
      cksumlen = 16;
    }

  if (ci->ci_eck && !GRUB_ZFS_IS_NATIVE_BYTEORDER(endian))
    grub_zfs_byteswap_checksum(&actual_cksum);

  if (grub_memcmp (&actual_cksum, &zc, cksumlen) != 0)
    {
      grub_dprintf ("zfs", "checksum %s verification failed\n", ci->ci_name);
      grub_dprintf ("zfs", "actual checksum %016llx %016llx %016llx %016llx\n",
		    (unsigned long long) actual_cksum.zc_word[0],
		    (unsigned long long) actual_cksum.zc_word[1],
		    (unsigned long long) actual_cksum.zc_word[2],
		    (unsigned long long) actual_cksum.zc_word[3]);
      grub_dprintf ("zfs", "expected checksum %016llx %016llx %016llx %016llx\n",
		    (unsigned long long) zc.zc_word[0],
		    (unsigned long long) zc.zc_word[1],
		    (unsigned long long) zc.zc_word[2],
		    (unsigned long long) zc.zc_word[3]);
      return grub_error (GRUB_ERR_BAD_FS, N_("checksum verification failed"));
    }

  return GRUB_ERR_NONE;
}

/*
 * vdev_uberblock_compare takes two uberblock structures and returns an integer
 * indicating the more recent of the two.
 * 	Return Value = 1 if ub2 is more recent
 * 	Return Value = -1 if ub1 is more recent
 * The most recent uberblock is determined using its transaction number and
 * timestamp.  The uberblock with the highest transaction number is
 * considered "newer".  If the transaction numbers of the two blocks match, the
 * timestamps are compared to determine the "newer" of the two.
 */
static int
vdev_uberblock_compare (uberblock_t * ub1, uberblock_t * ub2)
{
  if (ub1->ub_txg < ub2->ub_txg)
    return -1;
  if (ub1->ub_txg > ub2->ub_txg)
    return 1;

  if (ub1->ub_timestamp < ub2->ub_timestamp)
    return -1;
  if (ub1->ub_timestamp > ub2->ub_timestamp)
    return 1;

  return 0;
}

/*
 * Three pieces of information are needed to verify an uberblock: the magic
 * number, the version number, and the checksum.
 *
 * Currently Implemented: version number, magic number, checksum
 *
 */
static grub_err_t
uberblock_verify (uberblock_phys_t * ub, grub_uint64_t offset,
		  grub_size_t s)
{
  uberblock_t *uber = &ub->ubp_uberblock;
  grub_err_t err;
  grub_zfs_endian_t endian = GRUB_ZFS_UNKNOWN_ENDIAN;
  zio_cksum_t zc;

  if (grub_zfs_to_cpu64 (uber->ub_magic, GRUB_ZFS_LITTLE_ENDIAN)
      == UBERBLOCK_MAGIC
      && SPA_VERSION_IS_SUPPORTED(grub_zfs_to_cpu64 (uber->ub_version, GRUB_ZFS_LITTLE_ENDIAN)))
    endian = GRUB_ZFS_LITTLE_ENDIAN;

  if (grub_zfs_to_cpu64 (uber->ub_magic, GRUB_ZFS_BIG_ENDIAN) == UBERBLOCK_MAGIC
      && SPA_VERSION_IS_SUPPORTED(grub_zfs_to_cpu64 (uber->ub_version, GRUB_ZFS_BIG_ENDIAN)))
    endian = GRUB_ZFS_BIG_ENDIAN;

  if (endian == GRUB_ZFS_UNKNOWN_ENDIAN)
    return grub_error (GRUB_ERR_BAD_FS, "invalid uberblock magic");

  grub_memset (&zc, 0, sizeof (zc));

  zc.zc_word[0] = grub_cpu_to_zfs64 (offset, endian);
  err = zio_checksum_verify (zc, ZIO_CHECKSUM_LABEL, endian,
			     (char *) ub, s, 0);

  return err;
}

/*
 * Find the best uberblock.
 * Return:
 *    Success - Pointer to the best uberblock.
 *    Failure - NULL
 */
static uberblock_phys_t *
find_bestub (uberblock_phys_t * ub_array,
	     const struct grub_zfs_device_desc *desc)
{
  uberblock_phys_t *ubbest = NULL, *ubptr;
  int i;
  grub_disk_addr_t offset;
  grub_err_t err = GRUB_ERR_NONE;
  int ub_shift;

  ub_shift = desc->ashift;
  if (ub_shift < VDEV_UBERBLOCK_SHIFT)
    ub_shift = VDEV_UBERBLOCK_SHIFT;

  for (i = 0; i < (VDEV_UBERBLOCK_RING >> ub_shift); i++)
    {
      offset = (desc->vdev_phys_sector << SPA_MINBLOCKSHIFT) + VDEV_PHYS_SIZE
	+ (i << ub_shift);

      ubptr = (uberblock_phys_t *) ((grub_properly_aligned_t *) ub_array
				    + ((i << ub_shift)
				       / sizeof (grub_properly_aligned_t)));
      err = uberblock_verify (ubptr, offset, (grub_size_t) 1 << ub_shift);
      if (err)
	{
	  grub_errno = GRUB_ERR_NONE;
	  continue;
	}

      if (ubptr->ubp_uberblock.ub_magic == grub_swap_bytes64_compile_time(UBERBLOCK_MAGIC))
	grub_zfs_byteswap_uberblock(&ubptr->ubp_uberblock);

      if (ubbest == NULL
	  || vdev_uberblock_compare (&(ubptr->ubp_uberblock),
				     &(ubbest->ubp_uberblock)) > 0)
	ubbest = ubptr;
    }
  if (!ubbest) {
    grub_errno = err;
    grub_dprintf("zfs", "no valid ub found. vdev_pys_sector=%lld\n",
		 (long long)desc->vdev_phys_sector);
  } else {
    grub_dprintf("zfs", "valid ub found. vdev_pys_sector=%lld\n",
		 (long long)desc->vdev_phys_sector);
  }

  return ubbest;
}

static grub_err_t
zfs_fetch_nvlist (struct grub_zfs_device_desc *diskdesc, char **nvlist)
{
  grub_err_t err;

  *nvlist = 0;

  if (!diskdesc->dev)
    return grub_error (GRUB_ERR_BUG, "member drive unknown");

  *nvlist = grub_malloc (VDEV_PHYS_SIZE);

  /* Read in the vdev name-value pair list (112K). */
  err = grub_disk_read (diskdesc->dev->disk, diskdesc->vdev_phys_sector, 0,
			VDEV_PHYS_SIZE, *nvlist);
  if (err)
    {
      grub_free (*nvlist);
      *nvlist = 0;
      return err;
    }
  return GRUB_ERR_NONE;
}

static grub_err_t
fill_vdev_info_real (struct grub_zfs_data *data,
		     const char *nvlist,
		     struct grub_zfs_device_desc *fill,
		     struct grub_zfs_device_desc *insert,
		     int *inserted,
		     unsigned ashift)
{
  char *type;

  type = grub_zfs_nvlist_lookup_string (nvlist, ZPOOL_CONFIG_TYPE);

  if (!type)
    return grub_errno;

  if (!grub_zfs_nvlist_lookup_uint64 (nvlist, "id", &(fill->id)))
    {
      grub_free (type);
      return grub_error (GRUB_ERR_BAD_FS, "couldn't find vdev id");
    }

  if (!grub_zfs_nvlist_lookup_uint64 (nvlist, "guid", &(fill->guid)))
    {
      grub_free (type);
      return grub_error (GRUB_ERR_BAD_FS, "couldn't find vdev id");
    }

  {
    grub_uint64_t par;
    if (grub_zfs_nvlist_lookup_uint64 (nvlist, "ashift", &par))
      fill->ashift = par;
    else if (ashift != 0xffffffff)
      fill->ashift = ashift;
    else
      {
	grub_free (type);
	return grub_error (GRUB_ERR_BAD_FS, "couldn't find ashift");
      }
  }

  fill->max_children_ashift = 0;

  if (grub_strcmp (type, VDEV_TYPE_DISK) == 0
      || grub_strcmp (type, VDEV_TYPE_FILE) == 0)
    {
      fill->type = DEVICE_LEAF;

      if (!fill->dev && fill->guid == insert->guid)
	{
	  fill->dev = insert->dev;
	  fill->vdev_phys_sector = insert->vdev_phys_sector;
	  fill->current_uberblock = insert->current_uberblock;
	  fill->original = insert->original;
	  if (!data->device_original)
	    data->device_original = fill;
	  insert->ashift = fill->ashift;
	  *inserted = 1;
	}

      grub_free (type);

      return GRUB_ERR_NONE;
    }

  if (grub_strcmp (type, VDEV_TYPE_MIRROR) == 0
      || grub_strcmp (type, VDEV_TYPE_RAIDZ) == 0)
    {
      int nelm, i;

      if (grub_strcmp (type, VDEV_TYPE_MIRROR) == 0)
	fill->type = DEVICE_MIRROR;
      else
	{
	  grub_uint64_t par;
	  fill->type = DEVICE_RAIDZ;
	  if (!grub_zfs_nvlist_lookup_uint64 (nvlist, "nparity", &par))
	    {
	      grub_free (type);
	      return grub_error (GRUB_ERR_BAD_FS, "couldn't find raidz parity");
	    }
	  fill->nparity = par;
	}

      nelm = grub_zfs_nvlist_lookup_nvlist_array_get_nelm (nvlist,
							   ZPOOL_CONFIG_CHILDREN);

      if (nelm <= 0)
	{
	  grub_free (type);
	  return grub_error (GRUB_ERR_BAD_FS, "incorrect mirror VDEV");
	}

      if (!fill->children)
	{
	  fill->n_children = nelm;

	  fill->children = grub_zalloc (fill->n_children
					* sizeof (fill->children[0]));
	}

      for (i = 0; i < nelm; i++)
	{
	  char *child;
	  grub_err_t err;

	  child = grub_zfs_nvlist_lookup_nvlist_array
	    (nvlist, ZPOOL_CONFIG_CHILDREN, i);

	  err = fill_vdev_info_real (data, child, &fill->children[i], insert,
				     inserted, fill->ashift);

	  grub_free (child);

	  if (err)
	    {
	      grub_free (type);
	      return err;
	    }
	  if (fill->children[i].ashift > fill->max_children_ashift)
	    fill->max_children_ashift = fill->children[i].ashift;
	}
      grub_free (type);
      return GRUB_ERR_NONE;
    }

  grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET, "vdev %s isn't supported", type);
  grub_free (type);
  return grub_errno;
}

static grub_err_t
fill_vdev_info (struct grub_zfs_data *data,
		char *nvlist, struct grub_zfs_device_desc *diskdesc,
		int *inserted)
{
  grub_uint64_t id;
  unsigned i;

  *inserted = 0;

  if (!grub_zfs_nvlist_lookup_uint64 (nvlist, "id", &id))
    return grub_error (GRUB_ERR_BAD_FS, "couldn't find vdev id");

  for (i = 0; i < data->n_devices_attached; i++)
    if (data->devices_attached[i].id == id)
      return fill_vdev_info_real (data, nvlist, &data->devices_attached[i],
				  diskdesc, inserted, 0xffffffff);

  data->n_devices_attached++;
  if (data->n_devices_attached > data->n_devices_allocated)
    {
      void *tmp;
      grub_size_t sz;

      if (grub_mul (data->n_devices_attached, 2, &data->n_devices_allocated) ||
	  grub_add (data->n_devices_allocated, 1, &data->n_devices_allocated) ||
	  grub_mul (data->n_devices_allocated, sizeof (data->devices_attached[0]), &sz))
	return GRUB_ERR_OUT_OF_RANGE;

      data->devices_attached = grub_realloc (tmp = data->devices_attached, sz);
      if (!data->devices_attached)
	{
	  data->devices_attached = tmp;
	  return grub_errno;
	}
    }

  grub_memset (&data->devices_attached[data->n_devices_attached - 1],
	       0, sizeof (data->devices_attached[data->n_devices_attached - 1]));

  return fill_vdev_info_real (data, nvlist,
			      &data->devices_attached[data->n_devices_attached - 1],
			      diskdesc, inserted, 0xffffffff);
}

/*
 * For a given XDR packed nvlist, verify the first 4 bytes and move on.
 *
 * An XDR packed nvlist is encoded as (comments from nvs_xdr_create) :
 *
 *      encoding method/host endian     (4 bytes)
 *      nvl_version                     (4 bytes)
 *      nvl_nvflag                      (4 bytes)
 *	encoded nvpairs:
 *		encoded size of the nvpair      (4 bytes)
 *		decoded size of the nvpair      (4 bytes)
 *		name string size                (4 bytes)
 *		name string data                (sizeof(NV_ALIGN4(string))
 *		data type                       (4 bytes)
 *		# of elements in the nvpair     (4 bytes)
 *		data
 *      2 zero's for the last nvpair
 *		(end of the entire list)	(8 bytes)
 *
 */

/*
 * The nvlist_next_nvpair() function returns a handle to the next nvpair in the
 * list following nvpair. If nvpair is NULL, the first pair is returned. If
 * nvpair is the last pair in the nvlist, NULL is returned.
 */
static const char *
nvlist_next_nvpair (const char *nvl, const char *nvpair)
{
  const char *nvp;
  int encode_size;
  int name_len;
  if (nvl == NULL)
    return NULL;

  if (nvpair == NULL)
    {
      /* skip over header, nvl_version and nvl_nvflag */
      nvpair = nvl + 4 * 3;
    }
  else
    {
      /* skip to the next nvpair */
      encode_size = grub_be_to_cpu32 (grub_get_unaligned32(nvpair));
      nvpair += encode_size;
      /*If encode_size equals 0 nvlist_next_nvpair would return
       * the same pair received in input, leading to an infinite loop.
       * If encode_size is less than 0, this will move the pointer
       * backwards, *possibly* examinining two times the same nvpair
       * and potentially getting into an infinite loop. */
      if(encode_size <= 0)
	{
	  grub_dprintf ("zfs", "nvpair with size <= 0\n");
	  grub_error (GRUB_ERR_BAD_FS, "incorrect nvlist");
	  return NULL;
	}
    }
  /* 8 bytes of 0 marks the end of the list */
  if (grub_get_unaligned64 (nvpair) == 0)
    return NULL;
  /*consistency checks*/
  if (nvpair + 4 * 3 >= nvl + VDEV_PHYS_SIZE)
    {
      grub_dprintf ("zfs", "nvlist overflow\n");
      grub_error (GRUB_ERR_BAD_FS, "incorrect nvlist");
      return NULL;
    }
  encode_size = grub_be_to_cpu32 (grub_get_unaligned32(nvpair));

  nvp = nvpair + 4*2;
  name_len = grub_be_to_cpu32 (grub_get_unaligned32 (nvp));
  nvp += 4;

  nvp = nvp + ((name_len + 3) & ~3); // align
  if (nvp + 4 >= nvl + VDEV_PHYS_SIZE
      || encode_size < 0
      || nvp + 4 + encode_size > nvl + VDEV_PHYS_SIZE)
    {
      grub_dprintf ("zfs", "nvlist overflow\n");
      grub_error (GRUB_ERR_BAD_FS, "incorrect nvlist");
      return NULL;
    }
  /* end consistency checks */

  return nvpair;
}

/*
 * This function returns 0 on success and 1 on failure. On success, a string
 * containing the name of nvpair is saved in buf.
 */
static int
nvpair_name (const char *nvp, char **buf, grub_size_t *buflen)
{
  /* skip over encode/decode size */
  nvp += 4 * 2;

  *buf = (char *) (nvp + 4);
  *buflen = grub_be_to_cpu32 (grub_get_unaligned32 (nvp));

  return 0;
}

/*
 * This function retrieves the value of the nvpair in the form of enumerated
 * type data_type_t.
 */
static int
nvpair_type (const char *nvp)
{
  int name_len, type;

  /* skip over encode/decode size */
  nvp += 4 * 2;

  /* skip over name_len */
  name_len = grub_be_to_cpu32 (grub_get_unaligned32 (nvp));
  nvp += 4;

  /* skip over name */
  nvp = nvp + ((name_len + 3) & ~3); /* align */

  type = grub_be_to_cpu32 (grub_get_unaligned32 (nvp));

  return type;
}

static int
nvpair_value (const char *nvp,char **val,
	      grub_size_t *size_out, grub_size_t *nelm_out)
{
  int name_len,nelm,encode_size;

  /* skip over encode/decode size */
  encode_size = grub_be_to_cpu32 (grub_get_unaligned32(nvp));
  nvp += 8;

  /* skip over name_len */
  name_len = grub_be_to_cpu32 (grub_get_unaligned32 (nvp));
  nvp += 4;

  /* skip over name */
  nvp = nvp + ((name_len + 3) & ~3); /* align */

  /* skip over type */
  nvp += 4;
  nelm = grub_be_to_cpu32 (grub_get_unaligned32 (nvp));
  nvp +=4;
  if (nelm < 1)
    {
      grub_error (GRUB_ERR_BAD_FS, "empty nvpair");
      return 0;
    }
  *val = (char *) nvp;
  *size_out = encode_size;
  if (nelm_out)
    *nelm_out = nelm;

  return 1;
}

/*
 * Check the disk label information and retrieve needed vdev name-value pairs.
 *
 */
static grub_err_t
check_pool_label (struct grub_zfs_data *data,
		  struct grub_zfs_device_desc *diskdesc,
		  int *inserted, int original)
{
  grub_uint64_t pool_state, txg = 0;
  char *nvlist,*features;
#if 0
  char *nv;
#endif
  grub_uint64_t poolguid;
  grub_uint64_t version;
  int found;
  grub_err_t err;
  grub_zfs_endian_t endian;
  vdev_phys_t *phys;
  zio_cksum_t emptycksum;

  *inserted = 0;

  err = zfs_fetch_nvlist (diskdesc, &nvlist);
  if (err)
    return err;

  phys = (vdev_phys_t*) nvlist;
  if (grub_zfs_to_cpu64 (phys->vp_zbt.zec_magic,
			 GRUB_ZFS_LITTLE_ENDIAN)
      == ZEC_MAGIC)
    endian = GRUB_ZFS_LITTLE_ENDIAN;
  else if (grub_zfs_to_cpu64 (phys->vp_zbt.zec_magic,
			      GRUB_ZFS_BIG_ENDIAN)
	   == ZEC_MAGIC)
    endian = GRUB_ZFS_BIG_ENDIAN;
  else
    {
      grub_free (nvlist);
      return grub_error (GRUB_ERR_BAD_FS,
			 "bad vdev_phys_t.vp_zbt.zec_magic number");
    }
  /* Now check the integrity of the vdev_phys_t structure though checksum.  */
  ZIO_SET_CHECKSUM(&emptycksum, grub_cpu_to_zfs64(diskdesc->vdev_phys_sector << 9, endian), 0, 0, 0);
  err = zio_checksum_verify (emptycksum, ZIO_CHECKSUM_LABEL, endian,
			     nvlist, VDEV_PHYS_SIZE, 0);
  if (err)
    return err;

  grub_dprintf ("zfs", "check 2 passed\n");

  found = grub_zfs_nvlist_lookup_uint64 (nvlist, ZPOOL_CONFIG_POOL_STATE,
					 &pool_state);
  if (! found)
    {
      grub_free (nvlist);
      if (! grub_errno)
	grub_error (GRUB_ERR_BAD_FS, ZPOOL_CONFIG_POOL_STATE " not found");
      return grub_errno;
    }
  grub_dprintf ("zfs", "check 3 passed\n");

  if (pool_state == POOL_STATE_DESTROYED)
    {
      grub_free (nvlist);
      return grub_error (GRUB_ERR_BAD_FS, "zpool is marked as destroyed");
    }
  grub_dprintf ("zfs", "check 4 passed\n");

  found = grub_zfs_nvlist_lookup_uint64 (nvlist, ZPOOL_CONFIG_POOL_TXG, &txg);
  if (!found)
    {
      grub_free (nvlist);
      if (! grub_errno)
	grub_error (GRUB_ERR_BAD_FS, ZPOOL_CONFIG_POOL_TXG " not found");
      return grub_errno;
    }
  grub_dprintf ("zfs", "check 6 passed\n");

  /* not an active device */
  if (txg == 0)
    {
      grub_free (nvlist);
      return grub_error (GRUB_ERR_BAD_FS, "zpool isn't active");
    }
  grub_dprintf ("zfs", "check 7 passed\n");

  found = grub_zfs_nvlist_lookup_uint64 (nvlist, ZPOOL_CONFIG_VERSION,
					 &version);
  if (! found)
    {
      grub_free (nvlist);
      if (! grub_errno)
	grub_error (GRUB_ERR_BAD_FS, ZPOOL_CONFIG_VERSION " not found");
      return grub_errno;
    }
  grub_dprintf ("zfs", "check 8 passed\n");

  if (!SPA_VERSION_IS_SUPPORTED(version))
    {
      grub_free (nvlist);
      return grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET,
			 "too new version %llu > %llu",
			 (unsigned long long) version,
			 (unsigned long long) SPA_VERSION_BEFORE_FEATURES);
    }
  grub_dprintf ("zfs", "check 9 passed\n");

  found = grub_zfs_nvlist_lookup_uint64 (nvlist, ZPOOL_CONFIG_GUID,
					 &(diskdesc->guid));
  if (! found)
    {
      grub_free (nvlist);
      if (! grub_errno)
	grub_error (GRUB_ERR_BAD_FS, ZPOOL_CONFIG_GUID " not found");
      return grub_errno;
    }

  found = grub_zfs_nvlist_lookup_uint64 (nvlist, ZPOOL_CONFIG_POOL_GUID,
					 &poolguid);
  if (! found)
    {
      grub_free (nvlist);
      if (! grub_errno)
	grub_error (GRUB_ERR_BAD_FS, ZPOOL_CONFIG_POOL_GUID " not found");
      return grub_errno;
    }

  grub_dprintf ("zfs", "check 11 passed\n");

  if (original)
    data->guid = poolguid;

  if (data->guid != poolguid)
    return grub_error (GRUB_ERR_BAD_FS, "another zpool");

  {
    char *nv;
    nv = grub_zfs_nvlist_lookup_nvlist (nvlist, ZPOOL_CONFIG_VDEV_TREE);

    if (!nv)
      {
	grub_free (nvlist);
	return grub_error (GRUB_ERR_BAD_FS, "couldn't find vdev tree");
      }
    err = fill_vdev_info (data, nv, diskdesc, inserted);
    if (err)
      {
	grub_free (nv);
	grub_free (nvlist);
	return err;
      }
    grub_free (nv);
  }
  grub_dprintf ("zfs", "check 10 passed\n");
  features = grub_zfs_nvlist_lookup_nvlist(nvlist,
					   ZPOOL_CONFIG_FEATURES_FOR_READ);
  if (features)
    {
      const char *nvp=NULL;
      char name[MAX_SUPPORTED_FEATURE_STRLEN + 1];
      char *nameptr;
      grub_size_t namelen;
      while ((nvp = nvlist_next_nvpair(features, nvp)) != NULL)
	{
	  nvpair_name (nvp, &nameptr, &namelen);
	  if(namelen > MAX_SUPPORTED_FEATURE_STRLEN)
	    namelen = MAX_SUPPORTED_FEATURE_STRLEN;
	  grub_memcpy (name, nameptr, namelen);
	  name[namelen] = '\0';
	  grub_dprintf("zfs","str=%s\n",name);
	  if (check_feature(name,1, NULL) != 0)
	    {
	      grub_dprintf("zfs","feature missing in check_pool_label:%s\n",name);
	      err= grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET," check_pool_label missing feature '%s' for read",name);
	      return err;
	    }
	}
    }
  grub_dprintf ("zfs", "check 12 passed (feature flags)\n");
  grub_free (nvlist);

  return GRUB_ERR_NONE;
}

static grub_err_t
scan_disk (grub_device_t dev, struct grub_zfs_data *data,
	   int original, int *inserted)
{
  int label = 0;
  uberblock_phys_t *ub_array, *ubbest = NULL;
  vdev_boot_header_t *bh;
  grub_err_t err;
  int vdevnum;
  struct grub_zfs_device_desc desc;

  ub_array = grub_malloc (VDEV_UBERBLOCK_RING);
  if (!ub_array)
    return grub_errno;

  bh = grub_malloc (VDEV_BOOT_HEADER_SIZE);
  if (!bh)
    {
      grub_free (ub_array);
      return grub_errno;
    }

  vdevnum = VDEV_LABELS;

  desc.dev = dev;
  desc.original = original;

  /* Don't check back labels on CDROM.  */
  if (grub_disk_native_sectors (dev->disk) == GRUB_DISK_SIZE_UNKNOWN)
    vdevnum = VDEV_LABELS / 2;

  for (label = 0; ubbest == NULL && label < vdevnum; label++)
    {
      desc.vdev_phys_sector
	= label * (sizeof (vdev_label_t) >> SPA_MINBLOCKSHIFT)
	+ ((VDEV_SKIP_SIZE + VDEV_BOOT_HEADER_SIZE) >> SPA_MINBLOCKSHIFT)
	+ (label < VDEV_LABELS / 2 ? 0 :
	   ALIGN_DOWN (grub_disk_native_sectors (dev->disk), sizeof (vdev_label_t))
	   - VDEV_LABELS * (sizeof (vdev_label_t) >> SPA_MINBLOCKSHIFT));

      /* Read in the uberblock ring (128K). */
      err = grub_disk_read (dev->disk, desc.vdev_phys_sector
			    + (VDEV_PHYS_SIZE >> SPA_MINBLOCKSHIFT),
			    0, VDEV_UBERBLOCK_RING, (char *) ub_array);
      if (err)
	{
	  grub_errno = GRUB_ERR_NONE;
	  continue;
	}
      grub_dprintf ("zfs", "label ok %d\n", label);

      err = check_pool_label (data, &desc, inserted, original);
      if (err || !*inserted)
	{
	  grub_errno = GRUB_ERR_NONE;
	  continue;
	}

      ubbest = find_bestub (ub_array, &desc);
      if (!ubbest)
	{
	  grub_dprintf ("zfs", "No uberblock found\n");
	  grub_errno = GRUB_ERR_NONE;
	  continue;
	}

      grub_memmove (&(desc.current_uberblock),
		    &ubbest->ubp_uberblock, sizeof (uberblock_t));
      if (original)
	grub_memmove (&(data->current_uberblock),
		      &ubbest->ubp_uberblock, sizeof (uberblock_t));

#if 0
      if (find_best_root &&
	  vdev_uberblock_compare (&ubbest->ubp_uberblock,
				  &(current_uberblock)) <= 0)
	continue;
#endif
      grub_free (ub_array);
      grub_free (bh);
      return GRUB_ERR_NONE;
    }

  grub_free (ub_array);
  grub_free (bh);

  return grub_error (GRUB_ERR_BAD_FS, "couldn't find a valid label");
}

/* Helper for scan_devices.  */
static int
scan_devices_iter (const char *name, void *hook_data)
{
  struct grub_zfs_data *data = hook_data;
  grub_device_t dev;
  grub_err_t err;
  int inserted;

  dev = grub_device_open (name);
  if (!dev)
    return 0;
  if (!dev->disk)
    {
      grub_device_close (dev);
      return 0;
    }
  err = scan_disk (dev, data, 0, &inserted);
  if (err == GRUB_ERR_BAD_FS)
    {
      grub_device_close (dev);
      grub_errno = GRUB_ERR_NONE;
      return 0;
    }
  if (err)
    {
      grub_device_close (dev);
      grub_print_error ();
      return 0;
    }

  if (!inserted)
    grub_device_close (dev);

  return 0;
}

static grub_err_t
scan_devices (struct grub_zfs_data *data)
{
  grub_device_iterate (scan_devices_iter, data);
  return GRUB_ERR_NONE;
}

/* x**y.  */
static grub_uint8_t powx[255 * 2];
/* Such an s that x**s = y */
static int powx_inv[256];
static const grub_uint8_t poly = 0x1d;

/* perform the operation a ^= b * (x ** (known_idx * recovery_pow) ) */
static inline void
xor_out (grub_uint8_t *a, const grub_uint8_t *b, grub_size_t s,
	 unsigned known_idx, unsigned recovery_pow)
{
  unsigned add;

  /* Simple xor.  */
  if (known_idx == 0 || recovery_pow == 0)
    {
      grub_crypto_xor (a, a, b, s);
      return;
    }
  add = (known_idx * recovery_pow) % 255;
  for (;s--; b++, a++)
    if (*b)
      *a ^= powx[powx_inv[*b] + add];
}

static inline grub_uint8_t
gf_mul (grub_uint8_t a, grub_uint8_t b)
{
  if (a == 0 || b == 0)
    return 0;
  return powx[powx_inv[a] + powx_inv[b]];
}

#define MAX_NBUFS 4

static grub_err_t
recovery (grub_uint8_t *bufs[4], grub_size_t s, const int nbufs,
	  const unsigned *powers,
	  const unsigned *idx)
{
  grub_dprintf ("zfs", "recovering %u buffers\n", nbufs);
  /* Now we have */
  /* b_i = sum (r_j* (x ** (powers[i] * idx[j])))*/
  /* Let's invert the matrix in question. */
  switch (nbufs)
    {
      /* Easy: r_0 = bufs[0] / (x << (powers[i] * idx[j])).  */
    case 1:
      {
	int add;
	grub_uint8_t *a;
	if (powers[0] == 0 || idx[0] == 0)
	  return GRUB_ERR_NONE;
	add = 255 - ((powers[0] * idx[0]) % 255);
	for (a = bufs[0]; s--; a++)
	  if (*a)
	    *a = powx[powx_inv[*a] + add];
	return GRUB_ERR_NONE;
      }
      /* Case 2x2: Let's use the determinant formula.  */
    case 2:
      {
	grub_uint8_t det, det_inv;
	grub_uint8_t matrixinv[2][2];
	unsigned i;
	/* The determinant is: */
	det = (powx[(powers[0] * idx[0] + powers[1] * idx[1]) % 255]
	       ^ powx[(powers[0] * idx[1] + powers[1] * idx[0]) % 255]);
	if (det == 0)
	  return grub_error (GRUB_ERR_BAD_FS, "singular recovery matrix");
	det_inv = powx[255 - powx_inv[det]];
	matrixinv[0][0] = gf_mul (powx[(powers[1] * idx[1]) % 255], det_inv);
	matrixinv[1][1] = gf_mul (powx[(powers[0] * idx[0]) % 255], det_inv);
	matrixinv[0][1] = gf_mul (powx[(powers[0] * idx[1]) % 255], det_inv);
	matrixinv[1][0] = gf_mul (powx[(powers[1] * idx[0]) % 255], det_inv);
	for (i = 0; i < s; i++)
	  {
	    grub_uint8_t b0, b1;
	    b0 = bufs[0][i];
	    b1 = bufs[1][i];

	    bufs[0][i] = (gf_mul (b0, matrixinv[0][0])
			  ^ gf_mul (b1, matrixinv[0][1]));
	    bufs[1][i] = (gf_mul (b0, matrixinv[1][0])
			  ^ gf_mul (b1, matrixinv[1][1]));
	  }
	return GRUB_ERR_NONE;
      }
      /* Otherwise use Gauss.  */
    case 3:
      {
	grub_uint8_t matrix1[MAX_NBUFS][MAX_NBUFS], matrix2[MAX_NBUFS][MAX_NBUFS];
	int i, j, k;

	for (i = 0; i < nbufs; i++)
	  for (j = 0; j < nbufs; j++)
	    matrix1[i][j] = powx[(powers[i] * idx[j]) % 255];
	for (i = 0; i < nbufs; i++)
	  for (j = 0; j < nbufs; j++)
	    matrix2[i][j] = 0;
	for (i = 0; i < nbufs; i++)
	  matrix2[i][i] = 1;

	for (i = 0; i < nbufs; i++)
	  {
	    grub_uint8_t mul;
	    for (j = i; j < nbufs; j++)
	      if (matrix1[i][j])
		break;
	    if (j == nbufs)
	      return grub_error (GRUB_ERR_BAD_FS, "singular recovery matrix");
	    if (j != i)
	      {
		int xchng;
		xchng = j;
		for (j = 0; j < nbufs; j++)
		  {
		    grub_uint8_t t;
		    t = matrix1[xchng][j];
		    matrix1[xchng][j] = matrix1[i][j];
		    matrix1[i][j] = t;
		  }
		for (j = 0; j < nbufs; j++)
		  {
		    grub_uint8_t t;
		    t = matrix2[xchng][j];
		    matrix2[xchng][j] = matrix2[i][j];
		    matrix2[i][j] = t;
		  }
	      }
	    mul = powx[255 - powx_inv[matrix1[i][i]]];
	    for (j = 0; j < nbufs; j++)
	      matrix1[i][j] = gf_mul (matrix1[i][j], mul);
	    for (j = 0; j < nbufs; j++)
	      matrix2[i][j] = gf_mul (matrix2[i][j], mul);
	    for (j = i + 1; j < nbufs; j++)
	      {
		mul = matrix1[j][i];
		for (k = 0; k < nbufs; k++)
		  matrix1[j][k] ^= gf_mul (matrix1[i][k], mul);
		for (k = 0; k < nbufs; k++)
		  matrix2[j][k] ^= gf_mul (matrix2[i][k], mul);
	      }
	  }
	for (i = nbufs - 1; i >= 0; i--)
	  {
	    for (j = 0; j < i; j++)
	      {
		grub_uint8_t mul;
		mul = matrix1[j][i];
		for (k = 0; k < nbufs; k++)
		  matrix1[j][k] ^= gf_mul (matrix1[i][k], mul);
		for (k = 0; k < nbufs; k++)
		  matrix2[j][k] ^= gf_mul (matrix2[i][k], mul);
	      }
	  }

	for (i = 0; i < (int) s; i++)
	  {
	    grub_uint8_t b[MAX_NBUFS];
	    for (j = 0; j < nbufs; j++)
	      b[j] = bufs[j][i];
	    for (j = 0; j < nbufs; j++)
	      {
		bufs[j][i] = 0;
		for (k = 0; k < nbufs; k++)
		  bufs[j][i] ^= gf_mul (matrix2[j][k], b[k]);
	      }
	  }
	return GRUB_ERR_NONE;
      }
    default:
      return grub_error (GRUB_ERR_BUG, "too big matrix");
    }
}

static grub_err_t
read_device (grub_uint64_t offset, struct grub_zfs_device_desc *desc,
	     grub_size_t len, void *buf)
{
  switch (desc->type)
    {
    case DEVICE_LEAF:
      {
	grub_uint64_t sector;
	sector = DVA_OFFSET_TO_PHYS_SECTOR (offset);
	if (!desc->dev)
	  {
	    return grub_error (GRUB_ERR_BAD_FS,
			       N_("couldn't find a necessary member device "
				  "of multi-device filesystem"));
	  }
	/* read in a data block */
	return grub_disk_read (desc->dev->disk, sector, 0, len, buf);
      }
    case DEVICE_MIRROR:
      {
	grub_err_t err = GRUB_ERR_NONE;
	unsigned i;
	if (desc->n_children <= 0)
	  return grub_error (GRUB_ERR_BAD_FS,
			     "non-positive number of mirror children");
	for (i = 0; i < desc->n_children; i++)
	  {
	    err = read_device (offset, &desc->children[i],
			       len, buf);
	    if (!err)
	      break;
	    grub_errno = GRUB_ERR_NONE;
	  }
	grub_errno = err;

	return err;
      }
    case DEVICE_RAIDZ:
      {
	unsigned c = 0;
	grub_uint64_t high;
	grub_uint64_t devn;
	grub_uint64_t m;
	grub_uint32_t s, orig_s;
	void *orig_buf = buf;
	grub_size_t orig_len = len;
	grub_uint8_t *recovery_buf[4];
	grub_size_t recovery_len[4];
	unsigned recovery_idx[4];
	unsigned failed_devices = 0;
	int idx, orig_idx;

	if (desc->nparity < 1 || desc->nparity > 3)
	  return grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET,
			     "raidz%d is not supported", desc->nparity);

	if (desc->n_children <= desc->nparity || desc->n_children < 1)
	  return grub_error(GRUB_ERR_BAD_FS, "too little devices for given parity");

	orig_s = (((len + (1 << desc->ashift) - 1) >> desc->ashift)
		  + (desc->n_children - desc->nparity) - 1);
	s = orig_s;

	high = grub_divmod64 ((offset >> desc->ashift),
			      desc->n_children, &m);
	if (desc->nparity == 2)
	  c = 2;
	if (desc->nparity == 3)
	  c = 3;
	if (((len + (1 << desc->ashift) - 1) >> desc->ashift)
	    >= (desc->n_children - desc->nparity))
	  idx = (desc->n_children - desc->nparity - 1);
	else
	  idx = ((len + (1 << desc->ashift) - 1) >> desc->ashift) - 1;
	orig_idx = idx;
	while (len > 0)
	  {
	    grub_size_t csize;
	    grub_uint32_t bsize;
	    grub_err_t err;
	    bsize = s / (desc->n_children - desc->nparity);

	    if (desc->nparity == 1
		&& ((offset >> (desc->ashift + 20 - desc->max_children_ashift))
		    & 1) == c)
	      c++;

	    high = grub_divmod64 ((offset >> desc->ashift) + c,
				  desc->n_children, &devn);
	    csize = (grub_size_t) bsize << desc->ashift;
	    if (csize > len)
	      csize = len;

	    grub_dprintf ("zfs", "RAIDZ mapping 0x%" PRIxGRUB_UINT64_T
			  "+%u (%" PRIxGRUB_SIZE ", %" PRIxGRUB_UINT32_T
			  ") -> (0x%" PRIxGRUB_UINT64_T ", 0x%"
			  PRIxGRUB_UINT64_T ")\n",
			  offset >> desc->ashift, c, len, bsize, high,
			  devn);
	    err = read_device ((high << desc->ashift)
			       | (offset & ((1 << desc->ashift) - 1)),
			       &desc->children[devn],
			       csize, buf);
	    if (err && failed_devices < desc->nparity)
	      {
		recovery_buf[failed_devices] = buf;
		recovery_len[failed_devices] = csize;
		recovery_idx[failed_devices] = idx;
		failed_devices++;
		grub_errno = err = 0;
	      }
	    if (err)
	      return err;

	    c++;
	    idx--;
	    s--;
	    buf = (char *) buf + csize;
	    len -= csize;
	  }
	if (failed_devices)
	  {
	    unsigned redundancy_pow[4];
	    unsigned cur_redundancy_pow = 0;
	    unsigned n_redundancy = 0;
	    unsigned i, j;
	    grub_err_t err;

	    /* Compute mul. x**s has a period of 255.  */
	    if (powx[0] == 0)
	      {
		grub_uint8_t cur = 1;
		for (i = 0; i < 255; i++)
		  {
		    powx[i] = cur;
		    powx[i + 255] = cur;
		    powx_inv[cur] = i;
		    if (cur & 0x80)
		      cur = (cur << 1) ^ poly;
		    else
		      cur <<= 1;
		  }
	      }

	    /* Read redundancy data.  */
	    for (n_redundancy = 0, cur_redundancy_pow = 0;
		 n_redundancy < failed_devices;
		 cur_redundancy_pow++)
	      {
		high = grub_divmod64 ((offset >> desc->ashift)
				      + cur_redundancy_pow
				      + ((desc->nparity == 1)
					 && ((offset >> (desc->ashift + 20
							 - desc->max_children_ashift))
					     & 1)),
				      desc->n_children, &devn);
		err = read_device ((high << desc->ashift)
				   | (offset & ((1 << desc->ashift) - 1)),
				   &desc->children[devn],
				   recovery_len[n_redundancy],
				   recovery_buf[n_redundancy]);
		/* Ignore error if we may still have enough devices.  */
		if (err && n_redundancy + desc->nparity - cur_redundancy_pow - 1
		    >= failed_devices)
		  {
		    grub_errno = GRUB_ERR_NONE;
		    continue;
		  }
		if (err)
		  return err;
		redundancy_pow[n_redundancy] = cur_redundancy_pow;
		n_redundancy++;
	      }
	    /* Now xor-our the parts we already know.  */
	    buf = orig_buf;
	    len = orig_len;
	    s = orig_s;
	    idx = orig_idx;

	    while (len > 0)
	      {
		grub_size_t csize = s;
		csize = ((csize / (desc->n_children - desc->nparity))
			 << desc->ashift);
		if (csize > len)
		  csize = len;

		for (j = 0; j < failed_devices; j++)
		  if (buf == recovery_buf[j])
		    break;

		if (j == failed_devices)
		  for (j = 0; j < failed_devices; j++)
		    xor_out (recovery_buf[j], buf,
			     csize < recovery_len[j] ? csize : recovery_len[j],
			     idx, redundancy_pow[j]);

		s--;
		buf = (char *) buf + csize;
		len -= csize;
		idx--;
	      }
	    for (i = 0; i < failed_devices
		   && recovery_len[i] == recovery_len[0];
		 i++);
	    /* Since the chunks have variable length handle the last block
	       separately.  */
	    if (i != failed_devices)
	      {
		grub_uint8_t *tmp_recovery_buf[4];
		for (j = 0; j < i; j++)
		  tmp_recovery_buf[j] = recovery_buf[j] + recovery_len[failed_devices - 1];
		err = recovery (tmp_recovery_buf, recovery_len[0] - recovery_len[failed_devices - 1], i, redundancy_pow,
				recovery_idx);
		if (err)
		  return err;
	      }
	    err = recovery (recovery_buf, recovery_len[failed_devices - 1],
			    failed_devices, redundancy_pow, recovery_idx);
	    if (err)
	      return err;
	  }
	return GRUB_ERR_NONE;
      }
    }
  return grub_error (GRUB_ERR_BAD_FS, "unsupported device type");
}

static grub_err_t
read_dva (const dva_t *dva,
	  struct grub_zfs_data *data,
	  void *buf, grub_size_t len)
{
  grub_uint64_t offset;
  unsigned i;
  grub_err_t err = 0;
  int try = 0;
  offset = DVA_GET_OFFSET(dva);

  for (try = 0; try < 2; try++)
    {
      for (i = 0; i < data->n_devices_attached; i++)
	if (data->devices_attached[i].id == DVA_GET_VDEV (dva))
	  {
	    err = read_device (offset, &data->devices_attached[i], len, buf);
	    if (!err)
	      return GRUB_ERR_NONE;
	    break;
	  }
      if (try == 1)
	break;
      err = scan_devices (data);
      if (err)
	return err;
    }
  if (!err)
    return grub_error (GRUB_ERR_BAD_FS, "unknown device %d",
		       (int) DVA_GET_VDEV (dva));
  return err;
}

/*
 * Read a block of data based on the gang block address dva,
 * and put its data in buf.
 *
 */
static grub_err_t
zio_read_gang (const blkptr_t * bp, const dva_t * dva, void *buf,
	       struct grub_zfs_data *data)
{
  zio_gbh_phys_t *zio_gb;
  unsigned i;
  grub_err_t err;
  zio_cksum_t zc;

  grub_memset (&zc, 0, sizeof (zc));

  zio_gb = grub_malloc (SPA_GANGBLOCKSIZE);
  if (!zio_gb)
    return grub_errno;

  err = read_dva (dva, data, zio_gb, SPA_GANGBLOCKSIZE);
  if (err)
    {
      grub_free (zio_gb);
      return err;
    }

  /* XXX */
  /* self checksuming the gang block header */
  ZIO_SET_CHECKSUM (&zc, DVA_GET_VDEV (dva),
		    DVA_GET_OFFSET (dva), bp->blk_birth, 0);
  err = zio_checksum_verify (zc, ZIO_CHECKSUM_GANG_HEADER, BP_GET_BYTEORDER(bp),
			     (char *) zio_gb, SPA_GANGBLOCKSIZE, 0);
  if (err)
    {
      grub_free (zio_gb);
      return err;
    }

  if (!GRUB_ZFS_IS_NATIVE_BYTEORDER(BP_GET_BYTEORDER(bp)))
    for (i = 0; i < SPA_GBH_NBLKPTRS; i++)
      grub_zfs_byteswap_blkptr(&zio_gb->zg_blkptr[i]);

  for (i = 0; i < SPA_GBH_NBLKPTRS; i++)
    {
      if (BP_IS_HOLE(&zio_gb->zg_blkptr[i]))
	continue;

      err = zio_read_data (&zio_gb->zg_blkptr[i], buf, data);
      if (err)
	{
	  grub_free (zio_gb);
	  return err;
	}
      buf = (char *) buf + BP_GET_PSIZE(&zio_gb->zg_blkptr[i]);
    }
  grub_free (zio_gb);
  return GRUB_ERR_NONE;
}

/*
 * Read in a block of raw data to buf.
 */
static grub_err_t
zio_read_data (const blkptr_t * bp, void *buf, struct grub_zfs_data *data)
{
  int i, psize;
  grub_err_t err = GRUB_ERR_NONE;

  psize = BP_GET_PSIZE (bp);

  /* pick a good dva from the block pointer */
  for (i = 0; i < SPA_DVAS_PER_BP; i++)
    {
      if (bp->blk_dva[i].dva_word[0] == 0 && bp->blk_dva[i].dva_word[1] == 0)
	continue;

      if (DVA_GET_GANG(&bp->blk_dva[i]))
	err = zio_read_gang (bp, &bp->blk_dva[i], buf, data);
      else
	err = read_dva (&bp->blk_dva[i], data, buf, psize);
      if (!err)
	return GRUB_ERR_NONE;
      grub_errno = GRUB_ERR_NONE;
    }

  if (!err)
    err = grub_error (GRUB_ERR_BAD_FS, "couldn't find a valid DVA");
  grub_errno = err;

  return err;
}

/*
 * buf must be at least BPE_GET_PSIZE(bp) bytes long (which will never be
 * more than BPE_PAYLOAD_SIZE bytes).
 */
static grub_err_t
decode_embedded_bp_compressed(const blkptr_t *bp, void *buf)
{
  grub_size_t psize, i;
  grub_uint8_t *buf8 = buf;
  grub_uint64_t w = 0;
  const grub_uint64_t *bp64 = (const grub_uint64_t *)bp;

  psize = BPE_GET_PSIZE(bp);

  /*
   * Decode the words of the block pointer into the byte array.
   * Low bits of first word are the first byte (little endian).
   */
  for (i = 0; i < psize; i++)
    {
      if (i % sizeof (w) == 0)
       {
         /* beginning of a word */
         w = *bp64;
         bp64++;
         if (!BPE_IS_PAYLOADWORD(bp, bp64))
         bp64++;
       }
      buf8[i] = BF64_GET(w, (i % sizeof (w)) * 8, 8);
    }
  return GRUB_ERR_NONE;
}

static int
datto_is_encrypted_type(grub_uint8_t dn_type)
{
  /* New structred types.  */
  if (dn_type & 0x80)
    return !!(dn_type & 0x20);
  switch(dn_type) {
  case DMU_OT_NONE ... DMU_OT_SPACE_MAP:
  case DMU_OT_OBJSET ... DMU_OT_ZNODE:
  case DMU_OT_MASTER_NODE:
  case DMU_OT_ZVOL_PROP:
  case DMU_OT_ZAP_OTHER ... DMU_OT_DSL_PERMS:
  case DMU_OT_FUID_SIZE ... DMU_OT_SCRUB_QUEUE:
  case DMU_OT_USERREFS ... DMU_OT_DDT_STATS:
    return 0;
  default:
    return 1;
  }
}

static void
add_blkptr_to_aad (char *aad, grub_size_t *aad_offset, blkptr_t bp)
{
  grub_uint64_t blk_prop = BP_IS_HOLE(&bp) ? 0 : bp.blk_prop;

  if (BP_GET_LEVEL(&bp) != 0) {
    blk_prop &= ~0x8000007fffff0000ULL;
  }

  blk_prop &= ~0x4000ff0000000000ULL;

  grub_set_unaligned64(&aad[*aad_offset], grub_cpu_to_le64(blk_prop));
  *aad_offset += 8;

  grub_memcpy(&aad[*aad_offset], &bp.blk_cksum.zc_word[2], 16);
  *aad_offset += 16;

  grub_memset(&aad[*aad_offset], 0, 8);
  *aad_offset += 8;
}

/*
 * Read in a block of data, verify its checksum, decompress if needed,
 * and put the uncompressed data in buf.
 */
static grub_err_t
zio_read (const blkptr_t *bp, void **buf,
	  grub_size_t *size, struct grub_zfs_data *data)
{
  grub_size_t lsize, psize;
  unsigned int comp;
  char *compbuf = NULL;
  grub_err_t err;
  zio_cksum_t zc = bp->blk_cksum;
  grub_uint32_t checksum;
  int datto_encrypted = 0, datto_authenticated = 0, datto_dnode_encryption = 0, oracle_encrypted = 0;

  *buf = NULL;

  checksum = BP_GET_CHECKSUM(bp);
  comp = BP_GET_COMPRESS(bp);
  if (BP_GET_PROP_BIT_61(bp))
    {
      if (data->subvol.is_datto_encrypted)
	{
	  grub_uint8_t type = BP_GET_TYPE(bp);
	  if (BP_GET_LEVEL(bp) > 0)
	    datto_authenticated = 1;
	  else if (type == DMU_OT_DNODE)
	    {
	      datto_encrypted = 1;
	      datto_authenticated = 0;
	      datto_dnode_encryption = 1;
	    }
	  else if (type == DMU_OT_OBJSET)
	    {
	      /* Objset uses inner hmacs that we don't suport yet.
		 Normal checksum is unaffected . */
	    }
	  else
	    {
	      datto_encrypted = datto_is_encrypted_type(type);
	      datto_authenticated = !datto_encrypted;
	    }
	}
      else
	  oracle_encrypted = 1;
    }
  if (BP_IS_EMBEDDED(bp))
    {
      if (BPE_GET_ETYPE(bp) != BP_EMBEDDED_TYPE_DATA)
	return grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET,
			   "unsupported embedded BP (type=%"
			   PRIuGRUB_UINT64_T ")\n",
			   BPE_GET_ETYPE (bp));
      lsize = BPE_GET_LSIZE(bp);
      psize = BPE_GET_PSIZE(bp);
    }
  else
    {
      lsize = (BP_IS_HOLE(bp) ? 0 : BP_GET_LSIZE(bp));
      psize = BP_GET_PSIZE(bp);
    }
  grub_dprintf("zfs", "zio_read: E %d: size %" PRIdGRUB_SSIZE "/%"
	       PRIdGRUB_SSIZE "\n", (int)BP_IS_EMBEDDED(bp), lsize, psize);

  if (size)
    *size = lsize;

  if (comp >= ZIO_COMPRESS_FUNCTIONS)
    return grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET,
		       "compression algorithm %u not supported\n", (unsigned int) comp);

  if (comp != ZIO_COMPRESS_OFF && decomp_table[comp].decomp_func == NULL)
    return grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET,
		       "compression algorithm %s not supported\n", decomp_table[comp].name);

  if (comp != ZIO_COMPRESS_OFF)
    /* It's not really necessary to align to 16, just for safety.  */
    compbuf = grub_malloc (ALIGN_UP (psize, 16));
  else
    compbuf = *buf = grub_malloc (lsize);
  if (! compbuf)
    return grub_errno;

  if (BP_IS_EMBEDDED(bp))
    err = decode_embedded_bp_compressed(bp, compbuf);
  else
    {
      err = zio_read_data (bp, compbuf, data);
      /* FIXME is it really necessary? */
      if (comp != ZIO_COMPRESS_OFF)
	grub_memset (compbuf + psize, 0, ALIGN_UP (psize, 16) - psize);
    }
  if (err)
    {
      grub_free (compbuf);
      *buf = NULL;
      return err;
    }

  if (!BP_IS_EMBEDDED(bp) && !datto_encrypted)
    {
      err = zio_checksum_verify (zc, checksum, BP_GET_BYTEORDER(bp),
			         compbuf, psize,
				 datto_authenticated);
      if (err)
        {
          grub_dprintf ("zfs", "incorrect checksum\n");
          grub_free (compbuf);
          *buf = NULL;
          return err;
        }
    }

  if (!BP_IS_EMBEDDED(bp) && datto_authenticated && data->subvol.key_datto.hmac_key && BP_GET_LEVEL(bp) == 0)
    {
      grub_uint8_t hmac[64];
      gcry_error_t err_gcry;
      err_gcry = grub_crypto_hmac_buffer (GRUB_MD_SHA512,
					  data->subvol.key_datto.hmac_key, 64,
					  compbuf, psize,
					  hmac);
      if (err_gcry)
	{
	  grub_free (compbuf);
          *buf = NULL;
	  return grub_crypto_gcry_error (err_gcry);
	}
      if (grub_crypto_memcmp(&zc.zc_word[2], hmac, 8) != 0)
	{
	  grub_free (compbuf);
          *buf = NULL;
	  grub_dprintf ("zfs", "actual hmac "
			"%02x %02x %02x %02x %02x %02x %02x %02x "
			"%02x %02x %02x %02x %02x %02x %02x %02x "
			"%02x %02x %02x %02x %02x %02x %02x %02x "
			"%02x %02x %02x %02x %02x %02x %02x %02x "
			"%02x %02x %02x %02x %02x %02x %02x %02x "
			"%02x %02x %02x %02x %02x %02x %02x %02x "
			"%02x %02x %02x %02x %02x %02x %02x %02x "
			"%02x %02x %02x %02x %02x %02x %02x %02x\n",
			hmac[ 0], hmac[ 1], hmac[ 2], hmac[ 3], hmac[ 4], hmac[ 5], hmac[ 6], hmac[ 7],
			hmac[ 8], hmac[ 9], hmac[10], hmac[11], hmac[12], hmac[13], hmac[14], hmac[15],
			hmac[16], hmac[17], hmac[18], hmac[19], hmac[20], hmac[21], hmac[22], hmac[23],
			hmac[24], hmac[25], hmac[26], hmac[27], hmac[28], hmac[29], hmac[30], hmac[31],
			hmac[32], hmac[33], hmac[34], hmac[35], hmac[36], hmac[37], hmac[38], hmac[39],
			hmac[40], hmac[41], hmac[42], hmac[43], hmac[44], hmac[45], hmac[46], hmac[47],
			hmac[48], hmac[49], hmac[50], hmac[51], hmac[52], hmac[53], hmac[54], hmac[55],
			hmac[56], hmac[57], hmac[58], hmac[59], hmac[60], hmac[61], hmac[62], hmac[63]);
	  grub_dprintf ("zfs", "expected hmac %016llx %016llx %016llx %016llx\n",
			(unsigned long long) zc.zc_word[0],
			(unsigned long long) zc.zc_word[1],
			(unsigned long long) zc.zc_word[2],
			(unsigned long long) zc.zc_word[3]);
	  return grub_error (GRUB_ERR_BAD_FS, N_("HMAC verification failed"));
	}
    }

  if (datto_dnode_encryption && (!grub_zfs_decrypt || !data->subvol.key_datto.master_key))
    {
      grub_dprintf("zfs", "Skipping decrypt of bonus because of missing zfscrypt module or key\n");
      datto_encrypted = 0;
      datto_dnode_encryption = 0;
    }
  if ((oracle_encrypted || datto_encrypted) && !grub_zfs_decrypt)
    err = grub_error (GRUB_ERR_BAD_FS,
		      N_("module `%s' isn't loaded"),
		      "zfscrypt");
  else if (datto_encrypted)
    {
      grub_uint32_t iv[3];

      if (!data->subvol.key_datto.master_key)
	{
	  grub_free (compbuf);
	  *buf = NULL;
	  grub_dprintf ("zfs", "no key for txg %" PRIxGRUB_UINT64_T "\n",
			bp->blk_birth);
	  return grub_error (GRUB_ERR_BAD_FS, "no key found in keychain");
	}

      iv[0] = (bp)->blk_dva[2].dva_word[1];
      iv[1] = (bp)->blk_dva[2].dva_word[1] >> 32;
      iv[2] = (bp)->blk_fill >> 32;

      if (datto_dnode_encryption)
	{
	  grub_size_t offset = 0, crypt_offset = 0, aad_offset = 0;
	  char *crypt = grub_malloc(psize), *aad = grub_malloc(psize);
	  if (!crypt || !aad)
	    {
	      grub_free (compbuf);
	      grub_free (crypt);
	      grub_free (aad);
	      *buf = NULL;
	      return grub_errno;
	    }
	  for (offset = 0; offset + sizeof (dnode_phys_t) <= psize; offset += sizeof(dnode_phys_t))
	    {
	      dnode_phys_t *dnp = (dnode_phys_t *) (void *) (compbuf + offset);
	      grub_memcpy(aad + aad_offset, dnp, 64);
	      ((dnode_phys_t *)(void *)(aad + aad_offset))->dn_used = 0;
	      ((dnode_phys_t *)(void *)(aad + aad_offset))->dn_flags &= DNODE_FLAG_SPILL_BLKPTR;
	      int has_spill = dnp->dn_flags & DNODE_FLAG_SPILL_BLKPTR;
	      unsigned i;

	      aad_offset += 64;

	      for (i = 0; i < dnp->dn_nblkptr; i++)
		add_blkptr_to_aad (aad, &aad_offset, dnp->dn_blkptr[i]);

	      if (has_spill)
		add_blkptr_to_aad (aad, &aad_offset, dnp->dn_spill);

	      char *bonus = DN_BONUS(dnp);
	      char *bonusmaxptr = (char *) (dnp + 1);
	      if (has_spill)
		bonusmaxptr -= sizeof(blkptr_t);
	      grub_size_t bonusmaxlen = bonusmaxptr - bonus;
	      if (datto_is_encrypted_type(dnp->dn_bonustype))
		{
		  grub_memcpy(crypt + crypt_offset, bonus, bonusmaxlen);
		  crypt_offset += bonusmaxlen;
		}
	      else
		{
		  grub_memcpy(aad + aad_offset, bonus, bonusmaxlen);
		  aad_offset += bonusmaxlen;
		}
	    }
	  err = grub_zfs_decrypt->decrypt_datto (&data->subvol.key_datto,
						 iv, (bp)->blk_dva[2].dva_word[0],
						 crypt, crypt_offset, aad, aad_offset,
						 &zc.zc_word[2],
						 BP_GET_BYTEORDER(bp));
	  grub_size_t out_offset = 0;
	  for (offset = 0; offset + sizeof (dnode_phys_t) <= psize; offset += sizeof(dnode_phys_t))
	    {
	      dnode_phys_t *dnp = (dnode_phys_t *) (void *) (compbuf + offset);
	      int has_spill = dnp->dn_flags & DNODE_FLAG_SPILL_BLKPTR;
	      if (!datto_is_encrypted_type(dnp->dn_bonustype))
		continue;
	      char * bonus = DN_BONUS(dnp);
	      char *bonusmaxptr = (char *) (dnp + 1);
	      if (has_spill)
		bonusmaxptr -= sizeof(blkptr_t);
	      grub_size_t bonusmaxlen = bonusmaxptr - bonus;
	      grub_memcpy(bonus, crypt + out_offset, bonusmaxlen);
	      out_offset += bonusmaxlen;
	    }

	}
      else
	err = grub_zfs_decrypt->decrypt_datto (&data->subvol.key_datto,
					       iv, (bp)->blk_dva[2].dva_word[0],
					       compbuf, psize, NULL, 0,
					       &zc.zc_word[2],
					       BP_GET_BYTEORDER(bp));
    }
  else if (oracle_encrypted)
    {
      unsigned i, besti = 0;
      grub_uint64_t bestval = 0;
      grub_dprintf("zfs", "Subvol has %d keys\n", (int) data->subvol.nkeys);
      for (i = 0; i < data->subvol.nkeys; i++)
	if (data->subvol.keyring[i].txg <= bp->blk_birth
	    && data->subvol.keyring[i].txg > bestval)
	  {
	    besti = i;
	    bestval = data->subvol.keyring[i].txg;
	  }
      if (bestval == 0)
	{
	  grub_free (compbuf);
	  *buf = NULL;
	  grub_dprintf ("zfs", "no key for txg %" PRIxGRUB_UINT64_T "\n",
			bp->blk_birth);
	  return grub_error (GRUB_ERR_BAD_FS, "no key found in keychain");
	}
      grub_dprintf ("zfs", "using key %u (%" PRIxGRUB_UINT64_T
		    ", %p) for txg %" PRIxGRUB_UINT64_T "\n",
		    besti, data->subvol.keyring[besti].txg,
		    data->subvol.keyring[besti].cipher,
		    bp->blk_birth);
      err = grub_zfs_decrypt->decrypt_oracle (data->subvol.keyring[besti].cipher,
					      data->subvol.keyring[besti].algo,
					      &(bp)->blk_dva[2],
					      compbuf, psize, zc.zc_mac,
					      BP_GET_BYTEORDER(bp));
    }
  if (err)
    {
      grub_free (compbuf);
      *buf = NULL;
      return err;
    }

  if (comp != ZIO_COMPRESS_OFF)
    {
      *buf = grub_malloc (lsize);
      if (!*buf)
	{
	  grub_free (compbuf);
	  return grub_errno;
	}

      err = decomp_table[comp].decomp_func (compbuf, *buf, psize, lsize);
      grub_free (compbuf);
      if (err)
	{
	  grub_free (*buf);
	  *buf = NULL;
	  return err;
	}
    }

  if (!GRUB_ZFS_IS_NATIVE_BYTEORDER(BP_GET_BYTEORDER(bp)))
    {
      if (BP_GET_LEVEL(bp) > 0)
	{
	  for (unsigned i = 0; i < lsize / sizeof(blkptr_t); i++)
	    grub_zfs_byteswap_blkptr((blkptr_t *)*buf + i);
	}
      else
	grub_zfs_byteswap_type(*buf, lsize, BP_GET_TYPE(bp));
    }

  if (!BP_IS_EMBEDDED(bp) && datto_authenticated && BP_GET_LEVEL(bp) > 0)
    {
      grub_uint8_t hash[64];
      char *aad = grub_malloc(lsize);
      grub_size_t aad_offset = 0;
      if (!aad)
	{
	  grub_free (*buf);
	  *buf = NULL;
	  return grub_errno;
	}
      for (unsigned i = 0; i < lsize / sizeof(blkptr_t); i++)
	add_blkptr_to_aad (aad, &aad_offset, ((blkptr_t *)*buf)[i]);
      grub_crypto_hash(GRUB_MD_SHA512, hash, aad, aad_offset);
      grub_free(aad);
      if (grub_crypto_memcmp(&zc.zc_word[2], hash, 8) != 0)
	{
	  grub_free (compbuf);
          *buf = NULL;
	  grub_dprintf ("zfs", "actual hash "
			"%02x %02x %02x %02x %02x %02x %02x %02x "
			"%02x %02x %02x %02x %02x %02x %02x %02x "
			"%02x %02x %02x %02x %02x %02x %02x %02x "
			"%02x %02x %02x %02x %02x %02x %02x %02x "
			"%02x %02x %02x %02x %02x %02x %02x %02x "
			"%02x %02x %02x %02x %02x %02x %02x %02x "
			"%02x %02x %02x %02x %02x %02x %02x %02x "
			"%02x %02x %02x %02x %02x %02x %02x %02x\n",
			hash[ 0], hash[ 1], hash[ 2], hash[ 3], hash[ 4], hash[ 5], hash[ 6], hash[ 7],
			hash[ 8], hash[ 9], hash[10], hash[11], hash[12], hash[13], hash[14], hash[15],
			hash[16], hash[17], hash[18], hash[19], hash[20], hash[21], hash[22], hash[23],
			hash[24], hash[25], hash[26], hash[27], hash[28], hash[29], hash[30], hash[31],
			hash[32], hash[33], hash[34], hash[35], hash[36], hash[37], hash[38], hash[39],
			hash[40], hash[41], hash[42], hash[43], hash[44], hash[45], hash[46], hash[47],
			hash[48], hash[49], hash[50], hash[51], hash[52], hash[53], hash[54], hash[55],
			hash[56], hash[57], hash[58], hash[59], hash[60], hash[61], hash[62], hash[63]);
	  grub_dprintf ("zfs", "expected hash %016llx %016llx %016llx %016llx\n",
			(unsigned long long) zc.zc_word[0],
			(unsigned long long) zc.zc_word[1],
			(unsigned long long) zc.zc_word[2],
			(unsigned long long) zc.zc_word[3]);
	  return grub_error (GRUB_ERR_BAD_FS, N_("hash verification failed"));
	}
    }

  return GRUB_ERR_NONE;
}

/*
 * Get the block from a block id.
 * push the block onto the stack.
 *
 */
static grub_err_t
dmu_read (dnode_phys_t * dn, grub_uint64_t blkid, void **buf,
	  struct grub_zfs_data *data)
{
  int level;
  grub_off_t idx;
  blkptr_t *bp_array = dn->dn_blkptr;
  int epbs = dn->dn_indblkshift - SPA_BLKPTRSHIFT;
  blkptr_t *bp;
  void *tmpbuf = 0;
  grub_err_t err = GRUB_ERR_NONE;

  bp = grub_malloc (sizeof (blkptr_t));
  if (!bp)
    return grub_errno;

  *buf = NULL;

  for (level = dn->dn_nlevels - 1; level >= 0; level--)
    {
      idx = (blkid >> (epbs * level)) & ((1 << epbs) - 1);
      *bp = bp_array[idx];
      if (bp_array != dn->dn_blkptr)
	{
	  grub_free (bp_array);
	  bp_array = 0;
	}

      if (BP_IS_HOLE (bp))
	{
	  grub_size_t size = dn->dn_datablkszsec
	    << SPA_MINBLOCKSHIFT;
	  *buf = grub_malloc (size);
	  if (!*buf)
	    {
	      err = grub_errno;
	      break;
	    }
	  grub_memset (*buf, 0, size);
	  break;
	}
      if (level == 0)
	{
	  err = zio_read (bp, buf, 0, data);
	  break;
	}
      err = zio_read (bp, &tmpbuf, 0, data);
      if (err)
	break;
      bp_array = tmpbuf;
    }
  if (bp_array != dn->dn_blkptr)
    grub_free (bp_array);

  grub_free (bp);
  return err;
}

/*
 * mzap_lookup: Looks up property described by "name" and returns the value
 * in "value".
 */
static grub_err_t
mzap_lookup (mzap_phys_t * zapobj,
	     grub_uint32_t objsize, const char *name, grub_uint64_t * value,
	     int case_insensitive)
{
  grub_uint32_t i, chunks;
  mzap_ent_phys_t *mzap_ent = zapobj->mz_chunk;

  if (objsize < MZAP_ENT_LEN)
    return grub_error (GRUB_ERR_FILE_NOT_FOUND, N_("file `%s' not found"), name);
  chunks = objsize / MZAP_ENT_LEN - 1;
  for (i = 0; i < chunks; i++)
    {
      if (case_insensitive ? (grub_strcasecmp (mzap_ent[i].mze_name, name) == 0)
	  : (grub_strcmp (mzap_ent[i].mze_name, name) == 0))
	{
	  *value = mzap_ent[i].mze_value;
	  return GRUB_ERR_NONE;
	}
    }

  return grub_error (GRUB_ERR_FILE_NOT_FOUND, N_("file `%s' not found"), name);
}

static int
mzap_iterate (mzap_phys_t * zapobj, int objsize,
	      int (*hook) (const char *name, grub_uint64_t val,
			   struct grub_zfs_dir_ctx *ctx),
	      struct grub_zfs_dir_ctx *ctx)
{
  int i, chunks;
  mzap_ent_phys_t *mzap_ent = zapobj->mz_chunk;

  chunks = objsize / MZAP_ENT_LEN - 1;
  for (i = 0; i < chunks; i++)
    {
      grub_dprintf ("zfs", "zap: name = %s, value = %llx, cd = %x\n",
		    mzap_ent[i].mze_name, (long long)mzap_ent[i].mze_value,
		    (int)mzap_ent[i].mze_cd);
      if (hook (mzap_ent[i].mze_name, mzap_ent[i].mze_value, ctx))
	return 1;
    }

  return 0;
}

static grub_uint64_t
zap_hash (grub_uint64_t salt, const char *name,
	  int case_insensitive)
{
  static grub_uint64_t table[256];
  const grub_uint8_t *cp;
  grub_uint8_t c;
  grub_uint64_t crc = salt;

  if (table[128] == 0)
    {
      grub_uint64_t *ct;
      int i, j;
      for (i = 0; i < 256; i++)
	{
	  for (ct = table + i, *ct = i, j = 8; j > 0; j--)
	    *ct = (*ct >> 1) ^ (-(*ct & 1) & ZFS_CRC64_POLY);
	}
    }

  if (case_insensitive)
    for (cp = (const grub_uint8_t *) name; (c = *cp) != '\0'; cp++)
      crc = (crc >> 8) ^ table[(crc ^ grub_toupper (c)) & 0xFF];
  else
    for (cp = (const grub_uint8_t *) name; (c = *cp) != '\0'; cp++)
      crc = (crc >> 8) ^ table[(crc ^ c) & 0xFF];

  /*
   * Only use 28 bits, since we need 4 bits in the cookie for the
   * collision differentiator.  We MUST use the high bits, since
   * those are the onces that we first pay attention to when
   * chosing the bucket.
   */
  crc &= ~((1ULL << (64 - ZAP_HASHBITS)) - 1);

  return crc;
}

/*
 * Only to be used on 8-bit arrays.
 * array_len is actual len in bytes (not encoded le_value_length).
 * buf is null-terminated.
 */

static inline int
name_cmp (const char *s1, const char *s2, grub_size_t n,
	  int case_insensitive)
{
  const char *t1 = (const char *) s1;
  const char *t2 = (const char *) s2;

  if (!case_insensitive)
    return grub_memcmp (t1, t2, n);

  while (n--)
    {
      if (grub_toupper (*t1) != grub_toupper (*t2))
	return (int) grub_toupper (*t1) - (int) grub_toupper (*t2);

      t1++;
      t2++;
    }

  return 0;
}

/* XXX */
static int
zap_leaf_array_equal (zap_leaf_phys_t * l,
		      int blksft, int chunk, grub_size_t array_len,
		      const char *buf, int case_insensitive)
{
  grub_size_t bseen = 0;

  while (bseen < array_len)
    {
      struct zap_leaf_array *la = &ZAP_LEAF_CHUNK (l, blksft, chunk)->l_array;
      grub_size_t toread = array_len - bseen;

      if (toread > ZAP_LEAF_ARRAY_BYTES)
	toread = ZAP_LEAF_ARRAY_BYTES;

      if (chunk >= ZAP_LEAF_NUMCHUNKS (blksft))
	return 0;

      if (name_cmp ((char *) la->la_array, buf + bseen, toread,
		    case_insensitive) != 0)
	break;
      chunk = la->la_next;
      bseen += toread;
    }
  return (bseen == array_len);
}

/* XXX */
static grub_err_t
zap_leaf_array_get (zap_leaf_phys_t * l, int blksft,
		    int chunk, grub_size_t array_len, char *buf)
{
  grub_size_t bseen = 0;

  while (bseen < array_len)
    {
      struct zap_leaf_array *la;
      grub_size_t toread = array_len - bseen;

      if (toread > ZAP_LEAF_ARRAY_BYTES)
	toread = ZAP_LEAF_ARRAY_BYTES;

      if (chunk >= ZAP_LEAF_NUMCHUNKS (blksft))
	/* Don't use grub_error because this error is to be ignored.  */
	return GRUB_ERR_BAD_FS;

      la = &ZAP_LEAF_CHUNK (l, blksft, chunk)->l_array;
      grub_memcpy (buf + bseen,la->la_array,  toread);
      chunk = la->la_next;
      bseen += toread;
    }
  return GRUB_ERR_NONE;
}


/*
 * Given a zap_leaf_phys_t, walk thru the zap leaf chunks to get the
 * value for the property "name".
 *
 */
/* XXX */
static grub_err_t
zap_leaf_lookup (zap_leaf_phys_t * l,
		 int blksft, grub_uint64_t h,
		 const char *name, grub_uint64_t * value,
		 int case_insensitive)
{
  grub_uint16_t chunk;
  struct zap_leaf_entry *le;

  /* Verify if this is a valid leaf block */
  if (l->l_hdr.lh_block_type != ZBT_LEAF)
    return grub_error (GRUB_ERR_BAD_FS, "invalid leaf type");
  if (l->l_hdr.lh_magic != ZAP_LEAF_MAGIC)
    return grub_error (GRUB_ERR_BAD_FS, "invalid leaf magic");

  for (chunk = l->l_hash[LEAF_HASH (blksft, h, l)];
       chunk != CHAIN_END; chunk = le->le_next)
    {

      if (chunk >= ZAP_LEAF_NUMCHUNKS (blksft))
	return grub_error (GRUB_ERR_BAD_FS, "invalid chunk number");

      le = ZAP_LEAF_ENTRY (l, blksft, chunk);

      /* Verify the chunk entry */
      if (le->le_type != ZAP_CHUNK_ENTRY)
	return grub_error (GRUB_ERR_BAD_FS, "invalid chunk entry");

      if (le->le_hash != h)
	continue;

      grub_dprintf ("zfs", "fzap: length %d\n", (int) le->le_name_length);

      if (zap_leaf_array_equal (l, blksft,
				le->le_name_chunk,
				le->le_name_length,
				name, case_insensitive))
	{
	  struct zap_leaf_array *la;

	  if (le->le_int_size != 8 || le->le_value_length != 1)
	    return grub_error (GRUB_ERR_BAD_FS, "invalid leaf chunk entry");

	  /* get the uint64_t property value */
	  la = &ZAP_LEAF_CHUNK (l, blksft, le->le_value_chunk)->l_array;

	  *value = grub_be_to_cpu64 (la->la_array64);

	  return GRUB_ERR_NONE;
	}
    }

  return grub_error (GRUB_ERR_FILE_NOT_FOUND, N_("file `%s' not found"), name);
}


/* Verify if this is a fat zap header block */
static grub_err_t
zap_verify (zap_phys_t *zap)
{
  if (zap->zap_magic != (grub_uint64_t) ZAP_MAGIC)
    return grub_error (GRUB_ERR_BAD_FS, "bad ZAP magic");

  if (zap->zap_salt == 0)
    return grub_error (GRUB_ERR_BAD_FS, "bad ZAP salt");

  return GRUB_ERR_NONE;
}

/*
 * Fat ZAP lookup
 *
 */
/* XXX */
static grub_err_t
fzap_lookup (dnode_phys_t * zap_dnode, zap_phys_t * zap,
	     const char *name, grub_uint64_t * value,
	     struct grub_zfs_data *data, int case_insensitive)
{
  void *l;
  grub_uint64_t hash, idx, blkid;
  int blksft = zfs_log2 (zap_dnode->dn_datablkszsec) + DNODE_SHIFT;
  grub_err_t err;

  err = zap_verify (zap);
  if (err)
    return err;

  hash = zap_hash (zap->zap_salt, name, case_insensitive);

  /* get block id from index */
  if (zap->zap_ptrtbl.zt_numblks != 0)
    return grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET,
		       "external pointer tables not supported");
  idx = ZAP_HASH_IDX (hash, zap->zap_ptrtbl.zt_shift);
  blkid = ((grub_uint64_t *) zap)[idx + (1 << (blksft - 3 - 1))];

  /* Get the leaf block */
  if ((1U << blksft) < sizeof (zap_leaf_phys_t))
    return grub_error (GRUB_ERR_BAD_FS, "ZAP leaf is too small");
  err = dmu_read (zap_dnode, blkid, &l, data);
  if (err)
    return err;

  err = zap_leaf_lookup (l, blksft, hash, name, value,
			 case_insensitive);
  grub_free (l);
  return err;
}

/* XXX */
static int
fzap_iterate (dnode_phys_t * zap_dnode, zap_phys_t * zap,
	      grub_size_t name_elem_length,
	      int (*hook) (const void *name, grub_size_t name_length,
			   const void *val_in,
			   grub_size_t nelem, grub_size_t elemsize,
			   void *data),
	      void *hook_data, struct grub_zfs_data *data)
{
  zap_leaf_phys_t *l;
  void *l_in;
  grub_uint64_t idx, idx2, blkid;
  grub_uint16_t chunk;
  int blksft = zfs_log2 (zap_dnode->dn_datablkszsec) + DNODE_SHIFT;
  grub_err_t err;

  if (zap_verify (zap))
    return 0;

  /* get block id from index */
  if (zap->zap_ptrtbl.zt_numblks != 0)
    {
      grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET,
		  "external pointer tables not supported");
      return 0;
    }
  /* Get the leaf block */
  if ((1U << blksft) < sizeof (zap_leaf_phys_t))
    {
      grub_error (GRUB_ERR_BAD_FS, "ZAP leaf is too small");
      return 0;
    }
  for (idx = 0; idx < (1ULL << zap->zap_ptrtbl.zt_shift); idx++)
    {
      blkid = ((grub_uint64_t *) zap)[idx + (1 << (blksft - 3 - 1))];

      for (idx2 = 0; idx2 < idx; idx2++)
	if (blkid == ((grub_uint64_t *) zap)[idx2 + (1 << (blksft - 3 - 1))])
	  break;
      if (idx2 != idx)
	continue;

      err = dmu_read (zap_dnode, blkid, &l_in, data);
      l = l_in;
      if (err)
	{
	  grub_errno = GRUB_ERR_NONE;
	  continue;
	}

      /* Verify if this is a valid leaf block */
      if (l->l_hdr.lh_block_type != ZBT_LEAF)
	{
	  grub_free (l);
	  continue;
	}
      if (l->l_hdr.lh_magic != ZAP_LEAF_MAGIC)
	{
	  grub_free (l);
	  continue;
	}

      for (chunk = 0; chunk < ZAP_LEAF_NUMCHUNKS (blksft); chunk++)
	{
	  char *buf;
	  struct zap_leaf_entry *le;
	  char *val;
	  grub_size_t val_length;
	  le = ZAP_LEAF_ENTRY (l, blksft, chunk);

	  /* Verify the chunk entry */
	  if (le->le_type != ZAP_CHUNK_ENTRY)
	    continue;

	  buf = grub_malloc (le->le_name_length
			     * name_elem_length + 1);
	  if (zap_leaf_array_get (l, blksft,
				  le->le_name_chunk,
				  le->le_name_length
				  * name_elem_length, buf))
	    {
	      grub_free (buf);
	      continue;
	    }
	  buf[le->le_name_length * name_elem_length] = 0;

	  val_length = ((int) le->le_value_length
			* (int) le->le_int_size);
	  val = grub_malloc (val_length);
	  if (zap_leaf_array_get (l, blksft,
				  le->le_value_chunk,
				  val_length, val))
	    {
	      grub_free (buf);
	      grub_free (val);
	      continue;
	    }

	  if (hook (buf, le->le_name_length,
		    val, le->le_value_length, le->le_int_size, hook_data))
	    {
	      grub_free (l);
	      return 1;
	    }
	  grub_free (buf);
	  grub_free (val);
	}
      grub_free (l);
    }
  return 0;
}

/*
 * Read in the data of a zap object and find the value for a matching
 * property name.
 *
 */
static grub_err_t
zap_lookup (dnode_phys_t * zap_dnode, const char *name, grub_uint64_t *val,
	    struct grub_zfs_data *data, int case_insensitive)
{
  grub_uint64_t block_type;
  grub_uint32_t size;
  void *zapbuf;
  grub_err_t err;

  grub_dprintf ("zfs", "looking for '%s'\n", name);

  /* Read in the first block of the zap object data. */
  size = (grub_uint32_t) zap_dnode->dn_datablkszsec << SPA_MINBLOCKSHIFT;
  err = dmu_read (zap_dnode, 0, &zapbuf, data);
  if (err)
    return err;
  block_type = *((grub_uint64_t *) zapbuf);

  grub_dprintf ("zfs", "zap read\n");

  if (block_type == ZBT_MICRO)
    {
      grub_dprintf ("zfs", "micro zap\n");
      err = mzap_lookup (zapbuf, size, name, val, case_insensitive);
      grub_dprintf ("zfs", "returned %d\n", err);
      grub_free (zapbuf);
      return err;
    }
  else if (block_type == ZBT_HEADER)
    {
      grub_dprintf ("zfs", "fat zap\n");
      /* this is a fat zap */
      err = fzap_lookup (zap_dnode, zapbuf, name, val, data,
			 case_insensitive);
      grub_dprintf ("zfs", "returned %d\n", err);
      grub_free (zapbuf);
      return err;
    }

  return grub_error (GRUB_ERR_BAD_FS, "unknown ZAP type");
}

/* Context for zap_iterate_u64.  */
struct zap_iterate_u64_ctx
{
  int (*hook) (const char *, grub_uint64_t, struct grub_zfs_dir_ctx *);
  struct grub_zfs_dir_ctx *dir_ctx;
};

/* Helper for zap_iterate_u64.  */
static int
zap_iterate_u64_transform (const void *name,
			   grub_size_t namelen __attribute__ ((unused)),
			   const void *val_in,
			   grub_size_t nelem,
			   grub_size_t elemsize,
			   void *data)
{
  struct zap_iterate_u64_ctx *ctx = data;

  if (elemsize != sizeof (grub_uint64_t) || nelem != 1)
    return 0;
  return ctx->hook (name, grub_be_to_cpu64 (*(const grub_uint64_t *) val_in),
		    ctx->dir_ctx);
}

static int
zap_iterate_u64 (dnode_phys_t * zap_dnode,
		 int (*hook) (const char *name, grub_uint64_t val,
			      struct grub_zfs_dir_ctx *ctx),
		 struct grub_zfs_data *data, struct grub_zfs_dir_ctx *ctx)
{
  grub_uint64_t block_type;
  int size;
  void *zapbuf;
  grub_err_t err;
  int ret;

  /* Read in the first block of the zap object data. */
  size = zap_dnode->dn_datablkszsec << SPA_MINBLOCKSHIFT;
  err = dmu_read (zap_dnode, 0, &zapbuf, data);
  if (err)
    return 0;
  block_type = *((grub_uint64_t *) zapbuf);

  grub_dprintf ("zfs", "zap iterate\n");

  if (block_type == ZBT_MICRO)
    {
      grub_dprintf ("zfs", "micro zap\n");
      ret = mzap_iterate (zapbuf, size, hook, ctx);
      grub_free (zapbuf);
      return ret;
    }
  else if (block_type == ZBT_HEADER)
    {
      struct zap_iterate_u64_ctx transform_ctx = {
	.hook = hook,
	.dir_ctx = ctx
      };

      grub_dprintf ("zfs", "fat zap\n");
      /* this is a fat zap */
      ret = fzap_iterate (zap_dnode, zapbuf, 1,
			  zap_iterate_u64_transform, &transform_ctx, data);
      grub_free (zapbuf);
      return ret;
    }
  grub_error (GRUB_ERR_BAD_FS, "unknown ZAP type");
  return 0;
}

static int
zap_iterate (dnode_phys_t * zap_dnode,
	     grub_size_t nameelemlen,
	     int (*hook) (const void *name, grub_size_t namelen,
			  const void *val_in,
			  grub_size_t nelem, grub_size_t elemsize,
			  void *data),
	     void *hook_data, struct grub_zfs_data *data)
{
  grub_uint64_t block_type;
  void *zapbuf;
  grub_err_t err;
  int ret;

  /* Read in the first block of the zap object data. */
  err = dmu_read (zap_dnode, 0, &zapbuf, data);
  if (err)
    return 0;
  block_type = *((grub_uint64_t *) zapbuf);

  grub_dprintf ("zfs", "zap iterate\n");

  if (block_type == ZBT_MICRO)
    {
      grub_error (GRUB_ERR_BAD_FS, "micro ZAP where FAT ZAP expected");
      return 0;
    }
  if (block_type == ZBT_HEADER)
    {
      grub_dprintf ("zfs", "fat zap\n");
      /* this is a fat zap */
      ret = fzap_iterate (zap_dnode, zapbuf, nameelemlen, hook, hook_data,
			  data);
      grub_free (zapbuf);
      return ret;
    }
  grub_error (GRUB_ERR_BAD_FS, "unknown ZAP type");
  return 0;
}


/*
 * Get the dnode of an object number from the metadnode of an object set.
 *
 * Input
 *	mdn - metadnode to get the object dnode
 *	objnum - object number for the object dnode
 *	buf - data buffer that holds the returning dnode
 */
static grub_err_t
dnode_get (dnode_phys_t * mdn, grub_uint64_t objnum, grub_uint8_t type,
	   dnode_phys_t * buf, struct grub_zfs_data *data)
{
  grub_uint64_t blkid, blksz;	/* the block id this object dnode is in */
  int epbs;			/* shift of number of dnodes in a block */
  int idx;			/* index within a block */
  void *dnbuf;
  grub_err_t err;

  objnum &= DNODE_NUM_MASK;

  blksz = mdn->dn_datablkszsec << SPA_MINBLOCKSHIFT;
  epbs = zfs_log2 (blksz) - DNODE_SHIFT;

  /* While this should never happen, we should check that epbs is not negative. */
  if (epbs < 0)
    epbs = 0;

  blkid = objnum >> epbs;
  idx = objnum & ((1 << epbs) - 1);

  if (data->dnode_buf != NULL && grub_memcmp (data->dnode_mdn, mdn,
					      sizeof (*mdn)) == 0
      && objnum >= data->dnode_start && objnum < data->dnode_end)
    {
      *buf = (data->dnode_buf)[idx];
      if (type && buf->dn_type != type)
	return grub_error(GRUB_ERR_BAD_FS, "incorrect dnode type");
      return GRUB_ERR_NONE;
    }

  grub_dprintf ("zfs", "blkid=%llx\n",
		(unsigned long long) blkid);
  err = dmu_read (mdn, blkid, &dnbuf, data);
  if (err)
    return err;
  if (dnbuf == NULL)
    return grub_error(GRUB_ERR_BAD_FS, "invalid or unavailable dnode");
  grub_dprintf ("zfs", "alive\n");

  grub_free (data->dnode_buf);
  grub_free (data->dnode_mdn);
  data->dnode_mdn = grub_malloc (sizeof (*mdn));
  if (! data->dnode_mdn)
    {
      grub_errno = GRUB_ERR_NONE;
      data->dnode_buf = 0;
    }
  else
    {
      grub_memcpy (data->dnode_mdn, mdn, sizeof (*mdn));
      data->dnode_buf = dnbuf;
      data->dnode_start = blkid << epbs;
      data->dnode_end = (blkid + 1) << epbs;
    }

  *buf = ((dnode_phys_t *) dnbuf)[idx];
  if (type && buf->dn_type != type)
    return grub_error(GRUB_ERR_BAD_FS, "incorrect dnode type");

  return GRUB_ERR_NONE;
}

#pragma GCC diagnostic ignored "-Wstrict-aliasing"

/*
 * Get the file dnode for a given file name where mdn is the meta dnode
 * for this ZFS object set. When found, place the file dnode in dn.
 * The 'path' argument will be mangled.
 *
 */
static grub_err_t
dnode_get_path (struct subvolume *subvol, const char *path_in, dnode_phys_t *dn,
		struct grub_zfs_data *data)
{
  grub_uint64_t objnum, version;
  char *cname, ch;
  grub_err_t err = GRUB_ERR_NONE;
  char *path, *path_buf;
  struct dnode_chain
  {
    struct dnode_chain *next;
    dnode_phys_t dn;
  };
  struct dnode_chain *dnode_path = 0, *dn_new, *root;

  dn_new = grub_malloc (sizeof (*dn_new));
  if (! dn_new)
    return grub_errno;
  dn_new->next = 0;
  dnode_path = root = dn_new;

  err = dnode_get (&subvol->mdn, MASTER_NODE_OBJ, DMU_OT_MASTER_NODE,
		   &(dnode_path->dn), data);
  if (err)
    {
      grub_free (dn_new);
      return err;
    }

  err = zap_lookup (&(dnode_path->dn), ZPL_VERSION_STR, &version,
		    data, 0);
  if (err)
    {
      grub_free (dn_new);
      return err;
    }

  if (version > ZPL_VERSION)
    {
      grub_free (dn_new);
      return grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET, "too new ZPL version");
    }

  err = zap_lookup (&(dnode_path->dn), "casesensitivity",
		    &subvol->case_insensitive,
		    data, 0);
  if (err == GRUB_ERR_FILE_NOT_FOUND)
    {
      grub_errno = GRUB_ERR_NONE;
      subvol->case_insensitive = 0;
    }

  err = zap_lookup (&(dnode_path->dn), ZFS_ROOT_OBJ, &objnum, data, 0);
  if (err)
    {
      grub_free (dn_new);
      return err;
    }

  err = dnode_get (&subvol->mdn, objnum, 0, &(dnode_path->dn), data);
  if (err)
    {
      grub_free (dn_new);
      return err;
    }

  path = path_buf = grub_strdup (path_in);
  if (!path_buf)
    {
      grub_free (dn_new);
      return grub_errno;
    }

  while (1)
    {
      /* skip leading slashes */
      while (*path == '/')
	path++;
      if (!*path)
	break;
      /* get the next component name */
      cname = path;
      while (*path && *path != '/')
	path++;
      /* Skip dot.  */
      if (cname + 1 == path && cname[0] == '.')
	continue;
      /* Handle double dot.  */
      if (cname + 2 == path && cname[0] == '.' && cname[1] == '.')
	{
	  if (dn_new->next)
	    {
	      dn_new = dnode_path;
	      dnode_path = dn_new->next;
	      grub_free (dn_new);
	    }
	  else
	    {
	      err = grub_error (GRUB_ERR_FILE_NOT_FOUND,
				"can't resolve ..");
	      break;
	    }
	  continue;
	}

      ch = *path;
      *path = 0;		/* ensure null termination */

      if (dnode_path->dn.dn_type != DMU_OT_DIRECTORY_CONTENTS)
	{
	  err = grub_error (GRUB_ERR_BAD_FILE_TYPE, N_("not a directory"));
	  break;
	}
      err = zap_lookup (&(dnode_path->dn), cname, &objnum,
			data, subvol->case_insensitive);
      if (err)
	break;

      dn_new = grub_malloc (sizeof (*dn_new));
      if (! dn_new)
	{
	  err = grub_errno;
	  break;
	}
      dn_new->next = dnode_path;
      dnode_path = dn_new;

      objnum = ZFS_DIRENT_OBJ (objnum);
      err = dnode_get (&subvol->mdn, objnum, 0, &(dnode_path->dn), data);
      if (err)
	break;

      *path = ch;
      if (dnode_path->dn.dn_bonustype == DMU_OT_ZNODE
	  && (((((znode_phys_t *) DN_BONUS (&dnode_path->dn))->zp_mode) >> 12) & 0xf) == 0xa)
	{
	  char *sym_value;
	  grub_size_t sym_sz;
	  int free_symval = 0;
	  char *oldpath = path, *oldpathbuf = path_buf;
	  sym_value = ((char *) DN_BONUS (&dnode_path->dn) + sizeof (struct znode_phys));

	  sym_sz = ((znode_phys_t *) DN_BONUS (&dnode_path->dn))->zp_size;

	  if (dnode_path->dn.dn_flags & 1)
	    {
	      grub_size_t block;
	      grub_size_t blksz;
	      blksz = dnode_path->dn.dn_datablkszsec << SPA_MINBLOCKSHIFT;

	      if (blksz == 0)
                {
                  err = grub_error (GRUB_ERR_BAD_FS, "0-sized block");
                  break;
                }

	      sym_value = grub_malloc (sym_sz);
	      if (!sym_value)
		{
		  err = grub_errno;
		  break;
		}

	      for (block = 0; block < (sym_sz + blksz - 1) / blksz; block++)
		{
		  void *t;
		  grub_size_t movesize;

		  err = dmu_read (&(dnode_path->dn), block, &t, data);
		  if (err)
		    {
		      grub_free (sym_value);
		      break;
		    }

		  movesize = sym_sz - block * blksz;
		  if (movesize > blksz)
		    movesize = blksz;

		  grub_memcpy (sym_value + block * blksz, t, movesize);
		  grub_free (t);
		}
		if (err)
		  break;
	      free_symval = 1;
	    }
	  path = path_buf = grub_malloc (sym_sz + grub_strlen (oldpath) + 1);
	  if (!path_buf)
	    {
	      grub_free (oldpathbuf);
	      if (free_symval)
		grub_free (sym_value);
	      err = grub_errno;
	      break;
	    }
	  grub_memcpy (path, sym_value, sym_sz);
	  if (free_symval)
	    grub_free (sym_value);
	  path [sym_sz] = 0;
	  grub_memcpy (path + grub_strlen (path), oldpath,
		       grub_strlen (oldpath) + 1);

	  grub_free (oldpathbuf);

	  /* Restart dnode walk using path of symlink. */
	  if (path[0] != '/')
	    {
	      dn_new = dnode_path;
	      dnode_path = dn_new->next;
	      grub_free (dn_new);
	    }
	  else while (dnode_path != root)
		 {
		   dn_new = dnode_path;
		   dnode_path = dn_new->next;
		   grub_free (dn_new);
		 }
	  dn_new = dnode_path;
	}
      if (dnode_path->dn.dn_bonustype == DMU_OT_SA)
	{
	  void *sahdrp;
	  int hdrsize;

	  if (dnode_path->dn.dn_bonuslen != 0)
	    {
	      sahdrp = DN_BONUS (&dnode_path->dn);
	    }
	  else if (dnode_path->dn.dn_flags & DNODE_FLAG_SPILL_BLKPTR)
	    {
	      blkptr_t *bp = &dnode_path->dn.dn_spill;

	      err = zio_read (bp, &sahdrp, NULL, data);
	      if (err)
	        break;
	    }
	  else
	    {
	      err = grub_error (GRUB_ERR_BAD_FS, "filesystem is corrupt");
	      break;
	    }

	  hdrsize = SA_HDR_SIZE (((sa_hdr_phys_t *) sahdrp));

	  if (((grub_get_unaligned64 ((char *) sahdrp
				      + hdrsize
				      + SA_TYPE_OFFSET) >> 12) & 0xf) == 0xa)
	    {
	      char *sym_value = (char *) sahdrp + hdrsize + SA_SYMLINK_OFFSET;
	      grub_size_t sym_sz = grub_get_unaligned64 ((char *) sahdrp
							 + hdrsize
							 + SA_SIZE_OFFSET);
	      char *oldpath = path, *oldpathbuf = path_buf;
	      path = path_buf = grub_malloc (sym_sz + grub_strlen (oldpath) + 1);
	      if (!path_buf)
		{
		  grub_free (oldpathbuf);
		  err = grub_errno;
		  break;
		}
	      grub_memcpy (path, sym_value, sym_sz);
	      path [sym_sz] = 0;
	      grub_memcpy (path + grub_strlen (path), oldpath,
			   grub_strlen (oldpath) + 1);

	      grub_free (oldpathbuf);

	      /* Restart dnode walk using path of symlink. */
	      if (path[0] != '/')
		{
		  dn_new = dnode_path;
		  dnode_path = dn_new->next;
		  grub_free (dn_new);
		}
	      else while (dnode_path != root)
		     {
		       dn_new = dnode_path;
		       dnode_path = dn_new->next;
		       grub_free (dn_new);
		     }
	      dn_new = dnode_path;
	    }
	}
    }

  if (!err)
    grub_memcpy (dn, &(dnode_path->dn), sizeof (*dn));

  while (dnode_path)
    {
      dn_new = dnode_path->next;
      grub_free (dnode_path);
      dnode_path = dn_new;
    }
  grub_free (path_buf);
  return err;
}

#if 0
/*
 * Get the default 'bootfs' property value from the rootpool.
 *
 */
static grub_err_t
get_default_bootfsobj (dnode_phys_t * mosmdn, grub_uint64_t * obj,
		       struct grub_zfs_data *data)
{
  grub_uint64_t objnum = 0;
  dnode_phys_t *dn;
  if (!dn)
    return grub_errno;

  if ((grub_errno = dnode_get (mosmdn, DMU_POOL_DIRECTORY_OBJECT,
			       DMU_OT_OBJECT_DIRECTORY, dn, data)))
    {
      grub_free (dn);
      return (grub_errno);
    }

  /*
   * find the object number for 'pool_props', and get the dnode
   * of the 'pool_props'.
   */
  if (zap_lookup (dn, DMU_POOL_PROPS, &objnum, data))
    {
      grub_free (dn);
      return (GRUB_ERR_BAD_FS);
    }
  if ((grub_errno = dnode_get (mosmdn, objnum, DMU_OT_POOL_PROPS, dn, data)))
    {
      grub_free (dn);
      return (grub_errno);
    }
  if (zap_lookup (dn, ZPOOL_PROP_BOOTFS, &objnum, data))
    {
      grub_free (dn);
      return (GRUB_ERR_BAD_FS);
    }

  if (!objnum)
    {
      grub_free (dn);
      return (GRUB_ERR_BAD_FS);
    }

  *obj = objnum;
  return (0);
}
#endif
/*
 * Given a MOS metadnode, get the metadnode of a given filesystem name (fsname),
 * e.g. pool/rootfs, or a given object number (obj), e.g. the object number
 * of pool/rootfs.
 *
 * If no fsname and no obj are given, return the DSL_DIR metadnode.
 * If fsname is given, return its metadnode and its matching object number.
 * If only obj is given, return the metadnode for this object number.
 *
 */
static grub_err_t
get_filesystem_dnode (dnode_phys_t * mosmdn, char *fsname,
		      dnode_phys_t * mdn, struct grub_zfs_data *data)
{
  grub_uint64_t objnum;
  grub_err_t err;

  err = dnode_get (mosmdn, DMU_POOL_DIRECTORY_OBJECT,
		   DMU_OT_OBJECT_DIRECTORY, mdn, data);
  if (err)
    return err;

  grub_dprintf ("zfs", "alive\n");

  err = zap_lookup (mdn, DMU_POOL_ROOT_DATASET, &objnum, data, 0);
  if (err)
    return err;

  grub_dprintf ("zfs", "alive\n");

  err = dnode_get (mosmdn, objnum, 0, mdn, data);
  if (err)
    return err;

  grub_dprintf ("zfs", "alive\n");

  while (*fsname)
    {
      grub_uint64_t childobj;
      char *cname, ch;

      while (*fsname == '/')
	fsname++;

      if (! *fsname || *fsname == '@')
	break;

      cname = fsname;
      while (*fsname && *fsname != '/')
	fsname++;
      ch = *fsname;
      *fsname = 0;

      childobj = (((dsl_dir_phys_t *) DN_BONUS (mdn)))->dd_child_dir_zapobj;
      err = dnode_get (mosmdn, childobj,
		       DMU_OT_DSL_DIR_CHILD_MAP, mdn, data);
      if (err)
	return err;

      err = zap_lookup (mdn, cname, &objnum, data, 0);
      if (err)
	return err;

      err = dnode_get (mosmdn, objnum, 0, mdn, data);
      if (err)
	return err;

      *fsname = ch;
    }
  return GRUB_ERR_NONE;
}

static grub_err_t
make_mdn (dnode_phys_t * mdn, struct grub_zfs_data *data)
{
  objset_phys_t *osp;
  blkptr_t *bp;
  grub_size_t ospsize = 0;
  grub_err_t err;

  bp = &(((dsl_dataset_phys_t *) DN_BONUS (mdn))->ds_bp);
  err = zio_read (bp, (void **) &osp, &ospsize, data);
  if (err)
    return err;
  if (ospsize < OBJSET_PHYS_SIZE_V14)
    {
      grub_free (osp);
      return grub_error (GRUB_ERR_BAD_FS, "too small osp");
    }

  *mdn = (osp)->os_meta_dnode;
  grub_free (osp);
  return GRUB_ERR_NONE;
}

/* Context for dnode_get_fullpath.  */
struct dnode_get_fullpath_ctx
{
  struct subvolume *subvol;
  grub_uint64_t salt;
  int keyn;
};

/* Helper for dnode_get_fullpath.  */
static int
count_zap_keys (const void *name __attribute__ ((unused)),
		grub_size_t namelen __attribute__ ((unused)),
		const void *val_in __attribute__ ((unused)),
		grub_size_t nelem __attribute__ ((unused)),
		grub_size_t elemsize __attribute__ ((unused)),
		void *data)
{
  struct dnode_get_fullpath_ctx *ctx = data;

  ctx->subvol->nkeys++;
  return 0;
}

/* Helper for dnode_get_fullpath.  */
static int
load_zap_key (const void *name, grub_size_t namelen, const void *val_in,
	      grub_size_t nelem, grub_size_t elemsize, void *data)
{
  struct dnode_get_fullpath_ctx *ctx = data;

  if (namelen != 1)
    {
      grub_dprintf ("zfs", "Unexpected key index size %" PRIuGRUB_SIZE "\n",
		    namelen);
      return 0;
    }

  if (elemsize != 1)
    {
      grub_dprintf ("zfs", "Unexpected key element size %" PRIuGRUB_SIZE "\n",
		    elemsize);
      return 0;
    }

  ctx->subvol->keyring[ctx->keyn].txg =
    grub_be_to_cpu64 (*(grub_uint64_t *) name);
  ctx->subvol->keyring[ctx->keyn].algo =
    grub_le_to_cpu64 (*(grub_uint64_t *) val_in);
  ctx->subvol->keyring[ctx->keyn].cipher =
    grub_zfs_decrypt->load_key_oracle (val_in, nelem, ctx->salt,
				       ctx->subvol->keyring[ctx->keyn].algo);
  ctx->keyn++;
  return 0;
}

static grub_err_t
dnode_get_fullpath (const char *fullpath, struct subvolume *subvol,
		    dnode_phys_t * dn, int *isfs,
		    struct grub_zfs_data *data)
{
  char *fsname, *snapname;
  const char *ptr_at, *filename;
  grub_uint64_t headobj;
  grub_uint64_t keychainobj;
  grub_err_t err;

  ptr_at = grub_strchr (fullpath, '@');
  if (! ptr_at)
    {
      *isfs = 1;
      filename = 0;
      snapname = 0;
      fsname = grub_strdup (fullpath);
    }
  else
    {
      const char *ptr_slash = grub_strchr (ptr_at, '/');

      *isfs = 0;
      fsname = grub_malloc (ptr_at - fullpath + 1);
      if (!fsname)
	return grub_errno;
      grub_memcpy (fsname, fullpath, ptr_at - fullpath);
      fsname[ptr_at - fullpath] = 0;
      if (ptr_at[1] && ptr_at[1] != '/')
	{
	  snapname = grub_malloc (ptr_slash - ptr_at);
	  if (!snapname)
	    {
	      grub_free (fsname);
	      return grub_errno;
	    }
	  grub_memcpy (snapname, ptr_at + 1, ptr_slash - ptr_at - 1);
	  snapname[ptr_slash - ptr_at - 1] = 0;
	}
      else
	snapname = 0;
      if (ptr_slash)
	filename = ptr_slash;
      else
	filename = "/";
      grub_dprintf ("zfs", "fsname = '%s' snapname='%s' filename = '%s'\n",
		    fsname, snapname, filename);
    }
  grub_dprintf ("zfs", "alive\n");
  err = get_filesystem_dnode (&(data->mos), fsname, dn, data);
  if (err)
    {
      grub_free (fsname);
      grub_free (snapname);
      return err;
    }

  grub_dprintf ("zfs", "alive\n");

  grub_uint64_t crypt_obj;

  err = zap_lookup (dn, "com.datto:crypto_key_obj", &crypt_obj, data, 0);
  if (err)
    {
      grub_errno = GRUB_ERR_NONE;
      crypt_obj = 0;
    }

  grub_dprintf("zfs", "crypt obj = %lld\n", (long long) crypt_obj);
  subvol->is_datto_encrypted = crypt_obj != 0;

  if (grub_zfs_decrypt && crypt_obj)
    {
      dnode_phys_t crypt_dn;
      err = dnode_get (&(data->mos), crypt_obj, 0xc4,
		       &crypt_dn, data);
      if (err)
	{
	  grub_free (fsname);
	  grub_free (snapname);
	  return err;
	}

      err = dnode_get (&(data->mos), crypt_obj, 0 /* ?? */,
		       &crypt_dn, data);
      struct grub_zfs_crypt_datto_data crypt_datto_data = { 0 };
      grub_uint64_t pbkdf2iters = 0, pbkdf2salt = 0, guid = 0, algo = 0, version = 0;
      zap_iterate (&crypt_dn, 1, fill_crypt_datto_data, &crypt_datto_data, data);

      err = zap_lookup (&crypt_dn, "pbkdf2iters", &pbkdf2iters, data, 0);
      if (err)
	{
	  grub_errno = GRUB_ERR_NONE;
	  pbkdf2iters = 0;
	}

      err = zap_lookup (&crypt_dn, "DSL_CRYPTO_GUID", &guid, data, 0);
      if (err)
	{
	  grub_errno = GRUB_ERR_NONE;
	  guid = 0;
	}

      err = zap_lookup (&crypt_dn, "DSL_CRYPTO_SUITE", &algo, data, 0);
      if (err)
	{
	  grub_errno = GRUB_ERR_NONE;
	  algo = 0;
	}

      err = zap_lookup (&crypt_dn, "DSL_CRYPTO_VERSION", &version, data, 0);
      if (err)
	{
	  grub_errno = GRUB_ERR_NONE;
	  version = 0;
	}

      err = zap_lookup (&crypt_dn, "pbkdf2salt", &pbkdf2salt, data, 0);
      if (err)
	{
	  grub_errno = GRUB_ERR_NONE;
	  pbkdf2salt = 0;
	}

      pbkdf2salt = grub_cpu_to_le64(pbkdf2salt);

      subvol->key_datto = grub_zfs_decrypt->load_key_datto(crypt_datto_data.iv, crypt_datto_data.ivlen,
							   crypt_datto_data.mac, crypt_datto_data.maclen,
							   crypt_datto_data.master, crypt_datto_data.masterlen,
							   crypt_datto_data.hmac, crypt_datto_data.hmaclen,
							   (const grub_uint8_t *) &pbkdf2salt, sizeof (pbkdf2salt),
							   pbkdf2iters, guid, algo, version);
    }


  headobj = ((dsl_dir_phys_t *) DN_BONUS (dn))->dd_head_dataset_obj;

  err = dnode_get (&(data->mos), headobj, 0, &subvol->mdn, data);
  if (err)
    {
      grub_free (fsname);
      grub_free (snapname);
      return err;
    }
  keychainobj = ((dsl_dir_phys_t *) DN_BONUS (dn))->keychain;

  grub_dprintf("zfs", "keychain obj = %lld\n", (long long) keychainobj);

  if (grub_zfs_decrypt && !subvol->is_datto_encrypted && keychainobj)
    {
      struct dnode_get_fullpath_ctx ctx = {
	.subvol = subvol,
	.keyn = 0
      };
      dnode_phys_t keychain_dn, props_dn;
      grub_uint64_t propsobj;
      propsobj = ((dsl_dir_phys_t *) DN_BONUS (dn))->dd_props_zapobj;

      err = dnode_get (&(data->mos), propsobj, DMU_OT_DSL_PROPS,
		       &props_dn, data);
      if (err)
	{
	  grub_free (fsname);
	  grub_free (snapname);
	  return err;
	}

      err = zap_lookup (&props_dn, "salt", &ctx.salt, data, 0);
      if (err == GRUB_ERR_FILE_NOT_FOUND)
	{
	  err = 0;
	  grub_errno = 0;
	  ctx.salt = 0;
	}
      if (err)
	{
	  grub_dprintf ("zfs", "failed here\n");
	  return err;
	}

      err = dnode_get (&(data->mos), keychainobj, DMU_OT_DSL_KEYCHAIN,
		       &keychain_dn, data);
      if (err)
	{
	  grub_free (fsname);
	  grub_free (snapname);
	  return err;
	}
      subvol->nkeys = 0;
      zap_iterate (&keychain_dn, 8, count_zap_keys, &ctx, data);
      subvol->keyring = grub_calloc (subvol->nkeys, sizeof (subvol->keyring[0]));
      if (!subvol->keyring)
	{
	  grub_free (fsname);
	  grub_free (snapname);
	  return err;
	}
      zap_iterate (&keychain_dn, 8, load_zap_key, &ctx, data);
    }

  if (snapname)
    {
      grub_uint64_t snapobj;

      snapobj = ((dsl_dataset_phys_t *) DN_BONUS (&subvol->mdn))->ds_snapnames_zapobj;

      err = dnode_get (&(data->mos), snapobj,
		       DMU_OT_DSL_DS_SNAP_MAP, &subvol->mdn, data);
      if (!err)
	err = zap_lookup (&subvol->mdn, snapname, &headobj, data, 0);
      if (!err)
	err = dnode_get (&(data->mos), headobj, 0,
			 &subvol->mdn, data);
      if (!err && subvol->mdn.dn_type != DMU_OT_DSL_DATASET && subvol->mdn.dn_bonustype != DMU_OT_DSL_DATASET)
	return grub_error(GRUB_ERR_BAD_FS, "incorrect dataset dnode type");

      if (err)
	{
	  grub_free (fsname);
	  grub_free (snapname);
	  return err;
	}
    }

  subvol->obj = headobj;

  make_mdn (&subvol->mdn, data);

  if (*isfs)
    {
      grub_free (fsname);
      grub_free (snapname);
      return GRUB_ERR_NONE;
    }
  err = dnode_get_path (subvol, filename, dn, data);
  grub_free (fsname);
  grub_free (snapname);
  return err;
}

static int
nvlist_find_value (const char *nvlist_in, const char *name,
		   int valtype, char **val,
		   grub_size_t *size_out, grub_size_t *nelm_out)
{
  grub_size_t nvp_name_len, name_len = grub_strlen(name);
  int type;
  const char *nvpair=NULL,*nvlist=nvlist_in;
  char *nvp_name;

  /* Verify if the 1st and 2nd byte in the nvlist are valid. */
  /* NOTE: independently of what endianness header announces all
     subsequent values are big-endian.  */
  if (nvlist[0] != NV_ENCODE_XDR || (nvlist[1] != NV_LITTLE_ENDIAN
				     && nvlist[1] != NV_BIG_ENDIAN))
    {
      grub_dprintf ("zfs", "incorrect nvlist header\n");
      grub_error (GRUB_ERR_BAD_FS, "incorrect nvlist");
      return 0;
    }

  /*
   * Loop thru the nvpair list
   * The XDR representation of an integer is in big-endian byte order.
   */
  while ((nvpair=nvlist_next_nvpair(nvlist,nvpair)))
    {
      nvpair_name(nvpair,&nvp_name, &nvp_name_len);
      type = nvpair_type(nvpair);
      if (type == valtype
	  && (nvp_name_len == name_len
	      || (nvp_name_len > name_len && nvp_name[name_len] == '\0'))
	  && grub_memcmp (nvp_name, name, name_len) == 0)
	{
	  return nvpair_value(nvpair,val,size_out,nelm_out);
	}
    }
  return 0;
}

int
grub_zfs_nvlist_lookup_uint64 (const char *nvlist, const char *name,
			       grub_uint64_t * out)
{
  char *nvpair;
  grub_size_t size;
  int found;

  found = nvlist_find_value (nvlist, name, DATA_TYPE_UINT64, &nvpair, &size, 0);
  if (!found)
    return 0;
  if (size < sizeof (grub_uint64_t))
    {
      grub_error (GRUB_ERR_BAD_FS, "invalid uint64");
      return 0;
    }

  *out = grub_be_to_cpu64 (grub_get_unaligned64 (nvpair));
  return 1;
}

char *
grub_zfs_nvlist_lookup_string (const char *nvlist, const char *name)
{
  char *nvpair;
  char *ret;
  grub_size_t slen;
  grub_size_t size;
  int found;

  found = nvlist_find_value (nvlist, name, DATA_TYPE_STRING, &nvpair, &size, 0);
  if (!found)
    return 0;
  if (size < 4)
    {
      grub_error (GRUB_ERR_BAD_FS, "invalid string");
      return 0;
    }
  slen = grub_be_to_cpu32 (grub_get_unaligned32 (nvpair));
  if (slen > size - 4)
    slen = size - 4;
  ret = grub_malloc (slen + 1);
  if (!ret)
    return 0;
  grub_memcpy (ret, nvpair + 4, slen);
  ret[slen] = 0;
  return ret;
}

char *
grub_zfs_nvlist_lookup_nvlist (const char *nvlist, const char *name)
{
  char *nvpair;
  char *ret;
  grub_size_t size, sz;
  int found;

  found = nvlist_find_value (nvlist, name, DATA_TYPE_NVLIST, &nvpair,
			     &size, 0);
  if (!found)
    return 0;

  if (grub_add (size, 3 * sizeof (grub_uint32_t), &sz))
      return 0;

  ret = grub_zalloc (sz);
  if (!ret)
    return 0;
  grub_memcpy (ret, nvlist, sizeof (grub_uint32_t));

  grub_memcpy (ret + sizeof (grub_uint32_t), nvpair, size);
  return ret;
}

int
grub_zfs_nvlist_lookup_nvlist_array_get_nelm (const char *nvlist,
					      const char *name)
{
  char *nvpair;
  grub_size_t nelm, size;
  int found;

  found = nvlist_find_value (nvlist, name, DATA_TYPE_NVLIST_ARRAY, &nvpair,
			     &size, &nelm);
  if (! found)
    return -1;
  return nelm;
}

static int
get_nvlist_size (const char *beg, const char *limit)
{
  const char *ptr;
  grub_uint32_t encode_size;

  ptr = beg + 8;

  while (ptr < limit
	 && (encode_size = grub_be_to_cpu32 (grub_get_unaligned32 (ptr))))
    ptr += encode_size;	/* goto the next nvpair */
  ptr += 8;
  return (ptr > limit) ? -1 : (ptr - beg);
}

char *
grub_zfs_nvlist_lookup_nvlist_array (const char *nvlist, const char *name,
				     grub_size_t index)
{
  char *nvpair, *nvpairptr;
  int found;
  char *ret;
  grub_size_t size;
  unsigned i;
  grub_size_t nelm;
  int elemsize = 0;

  found = nvlist_find_value (nvlist, name, DATA_TYPE_NVLIST_ARRAY, &nvpair,
			     &size, &nelm);
  if (!found)
    return 0;
  if (index >= nelm)
    {
      grub_error (GRUB_ERR_OUT_OF_RANGE, "trying to lookup past nvlist array");
      return 0;
    }

  nvpairptr = nvpair;

  for (i = 0; i < index; i++)
    {
      int r;
      r = get_nvlist_size (nvpairptr, nvpair + size);

      if (r < 0)
	{
	  grub_error (GRUB_ERR_BAD_FS, "incorrect nvlist array");
	  return NULL;
	}
      nvpairptr += r;
    }

  elemsize = get_nvlist_size (nvpairptr, nvpair + size);

  if (elemsize < 0)
    {
      grub_error (GRUB_ERR_BAD_FS, "incorrect nvlist array");
      return 0;
    }

  ret = grub_zalloc (elemsize + sizeof (grub_uint32_t));
  if (!ret)
    return 0;
  grub_memcpy (ret, nvlist, sizeof (grub_uint32_t));

  grub_memcpy (ret + sizeof (grub_uint32_t), nvpairptr, elemsize);
  return ret;
}

static void
unmount_device (struct grub_zfs_device_desc *desc)
{
  unsigned i;
  switch (desc->type)
    {
    case DEVICE_LEAF:
      if (!desc->original && desc->dev)
	grub_device_close (desc->dev);
      return;
    case DEVICE_RAIDZ:
    case DEVICE_MIRROR:
      for (i = 0; i < desc->n_children; i++)
	unmount_device (&desc->children[i]);
      grub_free (desc->children);
      return;
    }
}

static void
zfs_unmount (struct grub_zfs_data *data)
{
  unsigned i;
  for (i = 0; i < data->n_devices_attached; i++)
    unmount_device (&data->devices_attached[i]);
  grub_free (data->devices_attached);
  grub_free (data->dnode_buf);
  grub_free (data->dnode_mdn);
  grub_free (data->file_buf);
  for (i = 0; i < data->subvol.nkeys; i++)
    grub_crypto_cipher_close (data->subvol.keyring[i].cipher);
  grub_free (data->subvol.keyring);
  grub_free (data);
}

/*
 * zfs_mount() locates a valid uberblock of the root pool and read in its MOS
 * to the memory address MOS.
 *
 */
static struct grub_zfs_data *
zfs_mount (grub_device_t dev)
{
  struct grub_zfs_data *data = 0;
  grub_err_t err;
  objset_phys_t *osp = 0;
  grub_size_t ospsize;
  uberblock_t *ub;
  int inserted;

  if (! dev->disk)
    {
      grub_error (GRUB_ERR_BAD_DEVICE, "not a disk");
      return 0;
    }

  data = grub_zalloc (sizeof (*data));
  if (!data)
    return 0;
#if 0
  /* if it's our first time here, zero the best uberblock out */
  if (data->best_drive == 0 && data->best_part == 0 && find_best_root)
    grub_memset (&current_uberblock, 0, sizeof (uberblock_t));
#endif

  data->n_devices_allocated = 16;
  data->devices_attached = grub_malloc (sizeof (data->devices_attached[0])
					* data->n_devices_allocated);
  data->n_devices_attached = 0;
  err = scan_disk (dev, data, 1, &inserted);
  if (err)
    {
      zfs_unmount (data);
      return NULL;
    }

  ub = &(data->current_uberblock);

  err = zio_read (&ub->ub_rootbp,
		  (void **) &osp, &ospsize, data);
  if (err)
    {
      zfs_unmount (data);
      return NULL;
    }

  if (ospsize < OBJSET_PHYS_SIZE_V14)
    {
      grub_error (GRUB_ERR_BAD_FS, "OSP too small");
      grub_free (osp);
      zfs_unmount (data);
      return NULL;
    }

  if (ub->ub_version >= SPA_VERSION_FEATURES &&
      check_mos_features(&osp->os_meta_dnode, data) != 0)
    {
      grub_error (GRUB_ERR_BAD_FS, "Unsupported features in pool");
      grub_free (osp);
      zfs_unmount (data);
      return NULL;
    }

  /* Got the MOS. Save it at the memory addr MOS. */
  grub_memmove (&(data->mos), &osp->os_meta_dnode, DNODE_SIZE);
  grub_free (osp);

  return data;
}

grub_err_t
grub_zfs_fetch_nvlist (grub_device_t dev, char **nvlist)
{
  struct grub_zfs_data *zfs;
  grub_err_t err;

  zfs = zfs_mount (dev);
  if (!zfs)
    return grub_errno;
  err = zfs_fetch_nvlist (zfs->device_original, nvlist);
  zfs_unmount (zfs);
  return err;
}

static grub_err_t
zfs_label (grub_device_t device, char **label)
{
  char *nvlist;
  grub_err_t err;
  struct grub_zfs_data *data;

  data = zfs_mount (device);
  if (! data)
    return grub_errno;

  err = zfs_fetch_nvlist (data->device_original, &nvlist);
  if (err)
    {
      zfs_unmount (data);
      return err;
    }

  *label = grub_zfs_nvlist_lookup_string (nvlist, ZPOOL_CONFIG_POOL_NAME);
  grub_free (nvlist);
  zfs_unmount (data);
  return grub_errno;
}

static grub_err_t
zfs_uuid (grub_device_t device, char **uuid)
{
  struct grub_zfs_data *data;

  *uuid = 0;

  data = zfs_mount (device);
  if (! data)
    return grub_errno;

  *uuid = grub_xasprintf ("%016llx", (long long unsigned) data->guid);
  zfs_unmount (data);
  if (! *uuid)
    return grub_errno;
  return GRUB_ERR_NONE;
}

static grub_err_t
zfs_mtime (grub_device_t device, grub_int64_t *mt)
{
  struct grub_zfs_data *data;

  *mt = 0;

  data = zfs_mount (device);
  if (! data)
    return grub_errno;

  *mt = data->current_uberblock.ub_timestamp;
  zfs_unmount (data);
  return GRUB_ERR_NONE;
}

static grub_err_t
parse_sa(const dnode_phys_t *dn, grub_int64_t *mtime, grub_uint64_t *sz,
	 struct grub_zfs_data *data)
{
  void *sahdrp;
  int hdrsize;
  grub_zfs_endian_t sa_endian;

  if (dn->dn_bonuslen != 0)
    sahdrp = (sa_hdr_phys_t *) DN_BONUS (dn);
  else if (dn->dn_flags & DNODE_FLAG_SPILL_BLKPTR)
    {
      const blkptr_t *bp = &dn->dn_spill;

      grub_err_t err = zio_read (bp, &sahdrp, NULL, data);
      if (err)
	return err;
    }
  else
    return GRUB_ERR_NONE;

  if (grub_zfs_to_cpu32(((sa_hdr_phys_t *) sahdrp)->sa_magic, GRUB_ZFS_LITTLE_ENDIAN) == SA_MAGIC)
    sa_endian = GRUB_ZFS_LITTLE_ENDIAN;
  else if (grub_zfs_to_cpu32(((sa_hdr_phys_t *) sahdrp)->sa_magic, GRUB_ZFS_BIG_ENDIAN) == SA_MAGIC)
    sa_endian = GRUB_ZFS_BIG_ENDIAN;
  else
    return GRUB_ERR_NONE;

  hdrsize = (grub_zfs_to_cpu16(((sa_hdr_phys_t *) sahdrp)->sa_layout_info, sa_endian) >> 10) << 3;
  if (mtime)
    *mtime = grub_zfs_to_cpu64 (grub_get_unaligned64 ((char *) sahdrp + hdrsize + SA_MTIME_OFFSET), sa_endian);
  if (sz)
    *sz = grub_zfs_to_cpu64 (grub_get_unaligned64 ((char *) sahdrp + hdrsize + SA_SIZE_OFFSET), sa_endian);

  return GRUB_ERR_NONE;
}

/*
 * zfs_open() locates a file in the rootpool by following the
 * MOS and places the dnode of the file in the memory address DNODE.
 */
static grub_err_t
grub_zfs_open (struct grub_file *file, const char *fsfilename)
{
  struct grub_zfs_data *data;
  grub_err_t err;
  int isfs;

  data = zfs_mount (file->device);
  if (! data)
    return grub_errno;

  err = dnode_get_fullpath (fsfilename, &(data->subvol),
			    &(data->dnode), &isfs, data);
  if (err)
    {
      zfs_unmount (data);
      return err;
    }

  if (isfs)
    {
      zfs_unmount (data);
      return grub_error (GRUB_ERR_BAD_FILE_TYPE, N_("missing `%c' symbol"), '@');
    }

  /* We found the dnode for this file. Verify if it is a plain file. */
  if (data->dnode.dn_type != DMU_OT_PLAIN_FILE_CONTENTS)
    {
      zfs_unmount (data);
      return grub_error (GRUB_ERR_BAD_FILE_TYPE, N_("not a regular file"));
    }

  /* get the file size and set the file position to 0 */

  /*
   * For DMU_OT_SA we will need to locate the SIZE attribute
   * attribute, which could be either in the bonus buffer
   * or the "spill" block.
   */
  if (data->dnode.dn_bonustype == DMU_OT_SA)
    {
      grub_uint64_t fsize;
      err = parse_sa(&data->dnode, NULL, &fsize, data);
      if (err)
	return err;
      file->size = fsize;
    }
  else if (data->dnode.dn_bonustype == DMU_OT_ZNODE)
    {
      file->size = ((znode_phys_t *) DN_BONUS (&data->dnode))->zp_size;
    }
  else
    return grub_error (GRUB_ERR_BAD_FS, "bad bonus type");

  file->data = data;
  file->offset = 0;

#ifndef GRUB_UTIL
  grub_dl_ref (my_mod);
#endif

  return GRUB_ERR_NONE;
}

static grub_ssize_t
grub_zfs_read (grub_file_t file, char *buf, grub_size_t len)
{
  struct grub_zfs_data *data = (struct grub_zfs_data *) file->data;
  grub_size_t blksz, movesize;
  grub_size_t length;
  grub_size_t read;
  grub_err_t err;

  /*
   * If offset is in memory, move it into the buffer provided and return.
   */
  if (file->offset >= data->file_start
      && file->offset + len <= data->file_end)
    {
      grub_memmove (buf, data->file_buf + file->offset - data->file_start,
		    len);
      return len;
    }

  blksz = data->dnode.dn_datablkszsec << SPA_MINBLOCKSHIFT;

  if (blksz == 0)
    {
      grub_error (GRUB_ERR_BAD_FS, "0-sized block");
      return -1;
    }

  /*
   * Entire Dnode is too big to fit into the space available.  We
   * will need to read it in chunks.  This could be optimized to
   * read in as large a chunk as there is space available, but for
   * now, this only reads in one data block at a time.
   */
  length = len;
  read = 0;
  while (length)
    {
      void *t;
      /*
       * Find requested blkid and the offset within that block.
       */
      grub_uint64_t blkid = grub_divmod64 (file->offset + read, blksz, 0);
      grub_free (data->file_buf);
      data->file_buf = 0;

      err = dmu_read (&(data->dnode), blkid, &t, data);
      data->file_buf = t;
      if (err)
	{
	  data->file_buf = NULL;
	  data->file_start = data->file_end = 0;
	  return -1;
	}

      data->file_start = blkid * blksz;
      data->file_end = data->file_start + blksz;

      movesize = data->file_end - file->offset - read;
      if (movesize > length)
	movesize = length;

      grub_memmove (buf, data->file_buf + file->offset + read
		    - data->file_start, movesize);
      buf += movesize;
      length -= movesize;
      read += movesize;
    }

  return len;
}

static grub_err_t
grub_zfs_close (grub_file_t file)
{
  zfs_unmount ((struct grub_zfs_data *) file->data);

#ifndef GRUB_UTIL
  grub_dl_unref (my_mod);
#endif

  return GRUB_ERR_NONE;
}

grub_err_t
grub_zfs_getmdnobj (grub_device_t dev, const char *fsfilename,
		    grub_uint64_t *mdnobj)
{
  struct grub_zfs_data *data;
  grub_err_t err;
  int isfs;

  data = zfs_mount (dev);
  if (! data)
    return grub_errno;

  err = dnode_get_fullpath (fsfilename, &(data->subvol),
			    &(data->dnode), &isfs, data);
  *mdnobj = data->subvol.obj;
  zfs_unmount (data);
  return err;
}

/*
 * Note: fill_fs_info() uses functions such as make_mdn() that modify
 * the input dnode_phys_t parameter. However, we should not allow it.
 * Therefore, we are making mdn_in constant - fill_fs_info() makes
 * a local copy of it.
 */
static grub_err_t
fill_fs_info (struct grub_dirhook_info *info,
	      const dnode_phys_t *mdn_in, struct grub_zfs_data *data)
{
  grub_err_t err;
  dnode_phys_t dn;
  grub_uint64_t objnum;
  grub_uint64_t headobj;
  dnode_phys_t mdn;

  grub_memcpy (&mdn, mdn_in, sizeof (*mdn_in));

  grub_memset (info, 0, sizeof (*info));

  info->dir = 1;

  if (mdn.dn_type == DMU_OT_DSL_DIR || mdn.dn_bonustype == DMU_OT_DSL_DIR)
    {
      headobj = ((dsl_dir_phys_t *) DN_BONUS (&mdn))->dd_head_dataset_obj;

      err = dnode_get (&(data->mos), headobj, 0, &mdn, data);
      if (err)
	{
	  grub_dprintf ("zfs", "failed here\n");
	  return err;
	}
    }
  err = make_mdn (&mdn, data);
  if (err)
    return err;
  err = dnode_get (&mdn, MASTER_NODE_OBJ, DMU_OT_MASTER_NODE,
		   &dn, data);
  if (err)
    {
      grub_dprintf ("zfs", "failed here\n");
      return err;
    }

  err = zap_lookup (&dn, ZFS_ROOT_OBJ, &objnum, data, 0);
  if (err)
    {
      grub_dprintf ("zfs", "failed here\n");
      return err;
    }

  err = dnode_get (&mdn, objnum, 0, &dn, data);
  if (err)
    {
      grub_dprintf ("zfs", "failed here\n");
      return err;
    }

  if (dn.dn_bonustype == DMU_OT_SA)
    {
      err = parse_sa(&dn, &info->mtime, NULL, data);
      if (err)
	return err;
      info->mtimeset = 1;
    }

  if (dn.dn_bonustype == DMU_OT_ZNODE)
    {
      info->mtimeset = 1;
      info->mtime = ((znode_phys_t *) DN_BONUS (&dn))->zp_mtime[0];
    }
  return 0;
}

/* Helper for grub_zfs_dir.  */
static int
iterate_zap (const char *name, grub_uint64_t val, struct grub_zfs_dir_ctx *ctx)
{
  grub_err_t err;
  struct grub_dirhook_info info;

  dnode_phys_t dn;
  grub_memset (&info, 0, sizeof (info));

  err = dnode_get (&(ctx->data->subvol.mdn), val, 0, &dn, ctx->data);
  if (err)
    {
      grub_print_error ();
      return 0;
    }

  if (dn.dn_bonustype == DMU_OT_SA)
    {
      err = parse_sa(&dn, &info.mtime, NULL, ctx->data);
      if (err)
	return err;
      info.mtimeset = 1;
    }

  if (dn.dn_bonustype == DMU_OT_ZNODE)
    {
      info.mtimeset = 1;
      info.mtime = ((znode_phys_t *) DN_BONUS (&dn))->zp_mtime[0];
    }
  info.dir = (dn.dn_type == DMU_OT_DIRECTORY_CONTENTS);
  grub_dprintf ("zfs", "type=%d, name=%s\n",
		(int)dn.dn_type, (char *)name);
  return ctx->hook (name, &info, ctx->hook_data);
}

/* Helper for grub_zfs_dir.  */
static int
iterate_zap_fs (const char *name, grub_uint64_t val,
		struct grub_zfs_dir_ctx *ctx)
{
  grub_err_t err;
  struct grub_dirhook_info info;

  if (name[0] == 0 && val == 0)
    return 0;

  dnode_phys_t mdn;
  err = dnode_get (&(ctx->data->mos), val, 0, &mdn, ctx->data);
  if (err)
    {
      grub_errno = 0;
      return 0;
    }
  if (mdn.dn_type != DMU_OT_DSL_DIR && mdn.dn_bonustype != DMU_OT_DSL_DIR) {
    grub_dprintf ("zfs", "type = 0x%x, val = 0x%llx\n", mdn.dn_type, (long long)val);
    return 0;
  }

  err = fill_fs_info (&info, &mdn, ctx->data);
  if (err)
    {
      grub_errno = 0;
      return 0;
    }
  return ctx->hook (name, &info, ctx->hook_data);
}

/* Helper for grub_zfs_dir.  */
static int
iterate_zap_snap (const char *name, grub_uint64_t val,
		  struct grub_zfs_dir_ctx *ctx)
{
  grub_err_t err;
  struct grub_dirhook_info info;
  char *name2;
  int ret;

  dnode_phys_t mdn;

  err = dnode_get (&(ctx->data->mos), val, 0, &mdn, ctx->data);
  if (err)
    {
      grub_errno = 0;
      return 0;
    }

  if (mdn.dn_type != DMU_OT_DSL_DATASET && mdn.dn_bonustype != DMU_OT_DSL_DATASET)
    return 0;

  err = fill_fs_info (&info, &mdn, ctx->data);
  if (err)
    {
      grub_errno = 0;
      return 0;
    }

  name2 = grub_malloc (grub_strlen (name) + 2);
  name2[0] = '@';
  grub_memcpy (name2 + 1, name, grub_strlen (name) + 1);
  ret = ctx->hook (name2, &info, ctx->hook_data);
  grub_free (name2);
  return ret;
}

static grub_err_t
grub_zfs_dir (grub_device_t device, const char *path,
	      grub_fs_dir_hook_t hook, void *hook_data)
{
  struct grub_zfs_dir_ctx ctx = {
    .hook = hook,
    .hook_data = hook_data
  };
  struct grub_zfs_data *data;
  grub_err_t err;
  int isfs;

  data = zfs_mount (device);
  if (! data)
    return grub_errno;
  err = dnode_get_fullpath (path, &(data->subvol), &(data->dnode), &isfs, data);
  if (err)
    {
      zfs_unmount (data);
      return err;
    }
  ctx.data = data;

  if (isfs)
    {
      grub_uint64_t childobj, headobj;
      grub_uint64_t snapobj;
      dnode_phys_t dn;
      struct grub_dirhook_info info;

      err = fill_fs_info (&info, &data->dnode, data);
      if (err)
	{
	  zfs_unmount (data);
	  return err;
	}
      if (hook ("@", &info, hook_data))
	{
	  zfs_unmount (data);
	  return GRUB_ERR_NONE;
	}

      childobj = ((dsl_dir_phys_t *) DN_BONUS (&data->dnode))->dd_child_dir_zapobj;
      headobj = ((dsl_dir_phys_t *) DN_BONUS (&data->dnode))->dd_head_dataset_obj;
      err = dnode_get (&(data->mos), childobj,
		       DMU_OT_DSL_DIR_CHILD_MAP, &dn, data);
      if (err)
	{
	  zfs_unmount (data);
	  return err;
	}

      zap_iterate_u64 (&dn, iterate_zap_fs, data, &ctx);

      err = dnode_get (&(data->mos), headobj, 0, &dn, data);
      if (!err && dn.dn_type != DMU_OT_DSL_DATASET && dn.dn_bonustype != DMU_OT_DSL_DATASET)
	return grub_error(GRUB_ERR_BAD_FS, "incorrect dataset dnode type");

      if (err)
	{
	  zfs_unmount (data);
	  return err;
	}

      snapobj = ((dsl_dataset_phys_t *) DN_BONUS (&dn))->ds_snapnames_zapobj;

      err = dnode_get (&(data->mos), snapobj,
		       DMU_OT_DSL_DS_SNAP_MAP, &dn, data);
      if (err)
	{
	  zfs_unmount (data);
	  return err;
	}

      zap_iterate_u64 (&dn, iterate_zap_snap, data, &ctx);
    }
  else
    {
      if (data->dnode.dn_type != DMU_OT_DIRECTORY_CONTENTS)
	{
	  zfs_unmount (data);
	  return grub_error (GRUB_ERR_BAD_FILE_TYPE, N_("not a directory"));
	}
      zap_iterate_u64 (&(data->dnode), iterate_zap, data, &ctx);
    }
  zfs_unmount (data);
  return grub_errno;
}

static int
check_feature (const char *name, grub_uint64_t val,
	       struct grub_zfs_dir_ctx *ctx __attribute__((unused)))
{
  int i;
  if (val == 0)
    return 0;
  if (name[0] == 0)
    return 0;
  for (i = 0; spa_feature_names[i] != NULL; i++)
    if (grub_strcmp (name, spa_feature_names[i]) == 0)
      return 0;
  return 1;
}

/*
 * Checks whether the MOS features that are active are supported by this
 * (GRUB's) implementation of ZFS.
 *
 * Return:
 *	0: Success.
 *	errnum: Failure.
 */

static grub_err_t
check_mos_features(dnode_phys_t *mosmdn_phys, struct grub_zfs_data* data )
{
  grub_uint64_t objnum;
  grub_err_t errnum = 0;
  dnode_phys_t dn,mosmdn;
  mzap_phys_t* mzp;
  int size;
  grub_memmove(&mosmdn,mosmdn_phys,sizeof(dnode_phys_t));
  errnum = dnode_get(&mosmdn, DMU_POOL_DIRECTORY_OBJECT,
		     DMU_OT_OBJECT_DIRECTORY, &dn,data);
  if (errnum != 0)
    return errnum;

  /*
   * Find the object number for 'features_for_read' and retrieve its
   * corresponding dnode. Note that we don't check features_for_write
   * because GRUB is not opening the pool for write.
   */
  errnum = zap_lookup(&dn, DMU_POOL_FEATURES_FOR_READ, &objnum, data,0);
  if (errnum != 0)
    return errnum;

  errnum = dnode_get(&mosmdn, objnum, DMU_OTN_ZAP_METADATA, &dn, data);
  if (errnum != 0)
    return errnum;

  errnum = dmu_read(&dn, 0, (void**)&mzp, data);
  if (errnum != 0)
    return errnum;

  size = dn.dn_datablkszsec << SPA_MINBLOCKSHIFT;
  return mzap_iterate (mzp, size, check_feature,NULL);
}


#ifdef GRUB_UTIL
static grub_err_t
grub_zfs_embed (grub_device_t device __attribute__ ((unused)),
		unsigned int *nsectors,
		unsigned int max_nsectors,
		grub_embed_type_t embed_type,
		grub_disk_addr_t **sectors)
{
  unsigned i;

  if (embed_type != GRUB_EMBED_PCBIOS)
    return grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET,
		       "ZFS currently supports only PC-BIOS embedding");

  if ((VDEV_BOOT_SIZE >> GRUB_DISK_SECTOR_BITS) < *nsectors)
    return grub_error (GRUB_ERR_OUT_OF_RANGE,
		       N_("your core.img is unusually large.  "
			  "It won't fit in the embedding area"));

  *nsectors = (VDEV_BOOT_SIZE >> GRUB_DISK_SECTOR_BITS);
  if (*nsectors > max_nsectors)
    *nsectors = max_nsectors;
  *sectors = grub_calloc (*nsectors, sizeof (**sectors));
  if (!*sectors)
    return grub_errno;
  for (i = 0; i < *nsectors; i++)
    (*sectors)[i] = i + (VDEV_BOOT_OFFSET >> GRUB_DISK_SECTOR_BITS);

  return GRUB_ERR_NONE;
}
#endif

static struct grub_fs grub_zfs_fs = {
  .name = "zfs",
  .fs_dir = grub_zfs_dir,
  .fs_open = grub_zfs_open,
  .fs_read = grub_zfs_read,
  .fs_close = grub_zfs_close,
  .fs_label = zfs_label,
  .fs_uuid = zfs_uuid,
  .fs_mtime = zfs_mtime,
#ifdef GRUB_UTIL
  .fs_embed = grub_zfs_embed,
  .reserved_first_sector = 1,
  .blocklist_install = 0,
#endif
  .next = 0
};

GRUB_MOD_INIT (zfs)
{
  COMPILE_TIME_ASSERT (sizeof (zap_leaf_chunk_t) == ZAP_LEAF_CHUNKSIZE);
  grub_fs_register (&grub_zfs_fs);
#ifndef GRUB_UTIL
  my_mod = mod;
#endif
}

GRUB_MOD_FINI (zfs)
{
  grub_fs_unregister (&grub_zfs_fs);
}
