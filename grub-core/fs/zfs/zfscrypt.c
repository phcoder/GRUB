/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2011  Free Software Foundation, Inc.
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

#include <grub/err.h>
#include <grub/file.h>
#include <grub/mm.h>
#include <grub/misc.h>
#include <grub/disk.h>
#include <grub/partition.h>
#include <grub/safemath.h>
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
#include <grub/crypto.h>
#include <grub/extcmd.h>
#include <grub/i18n.h>

GRUB_MOD_LICENSE ("GPLv3+");

/*
  Oracle part is mostly based on following article:
  https://blogs.oracle.com/darren/entry/zfs_encryption_what_is_on
  Datto part is based on comments in OpenZFS. No actal code has been used
 */

enum grub_zfs_algo
  {
    GRUB_ZFS_ALGO_CCM,
    GRUB_ZFS_ALGO_GCM,
  };

struct datto_algo {
  grub_size_t wrapkeylen;
  grub_size_t cryptkeylen;
  grub_size_t masterkeylen;
  enum grub_zfs_algo used_algo;
} datto_algos[] = {
  { 0 },
  { 0 },
  { 0 },
  { 32, 16, 16, GRUB_ZFS_ALGO_CCM },
  { 32, 24, 24, GRUB_ZFS_ALGO_CCM },
  { 32, 32, 32, GRUB_ZFS_ALGO_CCM },
  { 32, 16, 16, GRUB_ZFS_ALGO_GCM },
  { 32, 24, 24, GRUB_ZFS_ALGO_GCM },
  { 32, 32, 32, GRUB_ZFS_ALGO_GCM },
};
#define MAX_WRAPCIPHERTEXTLEN 96
#define MAX_MACLEN 16

struct grub_zfs_key_oracle
{
  grub_uint64_t algo;
  grub_uint8_t enc_nonce[13];
  grub_uint8_t unused[3];
  grub_uint8_t enc_key[48];
  grub_uint8_t unknown_purpose_nonce[13];
  grub_uint8_t unused2[3];
  grub_uint8_t unknown_purpose_key[48];
};

struct grub_zfs_wrap_key
{
  struct grub_zfs_wrap_key *next;
  grub_size_t keylen;
  int is_passphrase;
  grub_uint64_t key[0];
};

static struct grub_zfs_wrap_key *zfs_wrap_keys;

grub_err_t
grub_zfs_add_key (grub_uint8_t *key_in,
		  grub_size_t keylen,
		  int passphrase)
{
  struct grub_zfs_wrap_key *key;
  grub_size_t sz;

  if (!passphrase && keylen > 32)
    keylen = 32;
  if (grub_add (sizeof (*key), keylen, &sz))
    return GRUB_ERR_OUT_OF_RANGE;
  key = grub_malloc (sz);
  if (!key)
    return grub_errno;
  key->is_passphrase = passphrase;
  key->keylen = keylen;
  grub_memcpy (key->key, key_in, keylen);
  key->next = zfs_wrap_keys;
  zfs_wrap_keys = key;
  return GRUB_ERR_NONE;
}

static gcry_err_code_t
grub_ccm_decrypt (grub_crypto_cipher_handle_t cipher,
		  grub_uint8_t *out,
		  const grub_uint8_t *in, grub_size_t psize,
		  const grub_uint8_t *aad, grub_size_t aadsize,
		  void *mac_out, const void *nonce,
		  unsigned noncelen, unsigned m)
{
  grub_uint8_t iv[16];
  grub_uint8_t mul[16];
  grub_uint32_t mac[4];
  unsigned i, j, l = 15 - noncelen, aprefixlen = 0;
  gcry_err_code_t err;
  grub_uint8_t aprefix[16] = { 0 };

  grub_memcpy (iv + 1, nonce, noncelen);

  iv[0] = (l - 1) | (((m-2) / 2) << 3) | ((aadsize != 0) << 6);
  for (j = 0; j < l; j++)
    iv[15 - j] = psize >> (8 * j);
  err = grub_crypto_ecb_encrypt (cipher, mac, iv, 16);
  if (err)
    return err;

  if (aadsize == 0)
    aprefixlen = 0;
  else if (aadsize <= 0xFEFF)
    {
      aprefixlen = 2;
      aprefix[0] = aadsize >> 8;
      aprefix[1] = aadsize;
    }
#if GRUB_CPU_SIZEOF_VOID_P == 8
  else if ((aadsize >> 32) == 0)
#endif
    {
      aprefixlen = 6;
      aprefix[0] = 0xff;
      aprefix[1] = 0xfe;
      grub_set_unaligned32(aprefix + 2, grub_cpu_to_be32(aadsize));
    }
#if GRUB_CPU_SIZEOF_VOID_P == 8
  else
    {
      aprefixlen = 10;
      aprefix[0] = 0xff;
      aprefix[1] = 0xff;
      grub_set_unaligned64(aprefix + 2, grub_cpu_to_be64(aadsize));
    }
#endif

  if (aadsize != 0)
    {
      grub_size_t ablocks = (aadsize + aprefixlen + 15) / 16;
      grub_size_t first_block_datalen = 16 - aprefixlen;
      if (first_block_datalen > aadsize)
	first_block_datalen = aadsize;
      grub_memcpy (aprefix + aprefixlen, aad, first_block_datalen);

      grub_crypto_xor (mac, mac, aprefix, 16);
      err = grub_crypto_ecb_encrypt (cipher, mac, mac, 16);
      if (err)
	return err;

      for (i = 1; i < ablocks; i++)
	{
	  grub_size_t csize, instart = (i - 1) * 16 + first_block_datalen;
	  csize = 16;
	  if (csize > aadsize - instart)
	    csize = aadsize - instart;
	  grub_crypto_xor (mac, mac, aad + instart, csize);
	  err = grub_crypto_ecb_encrypt (cipher, mac, mac, 16);
	  if (err)
	    return err;
	}
    }

  iv[0] = l - 1;

  for (i = 0; i < (psize + 15) / 16; i++)
    {
      grub_size_t csize;
      csize = 16;
      if (csize > psize - 16 * i)
	csize = psize - 16 * i;
      for (j = 0; j < l; j++)
	iv[15 - j] = (i + 1) >> (8 * j);
      err = grub_crypto_ecb_encrypt (cipher, mul, iv, 16);
      if (err)
	return err;
      grub_crypto_xor (out + 16 * i, in + 16 * i, mul, csize);
      grub_crypto_xor (mac, mac, out + 16 * i, csize);
      err = grub_crypto_ecb_encrypt (cipher, mac, mac, 16);
      if (err)
	return err;
    }
  for (j = 0; j < l; j++)
    iv[15 - j] = 0;
  err = grub_crypto_ecb_encrypt (cipher, mul, iv, 16);
  if (err)
    return err;
  if (mac_out)
    grub_crypto_xor (mac_out, mac, mul, m);
  return GPG_ERR_NO_ERROR;
}

static void
grub_gcm_mul_x (grub_uint8_t *a)
{
  int i;
  int c = 0, d = 0;
  for (i = 0; i < 16; i++)
    {
      c = a[i] & 0x1;
      a[i] = (a[i] >> 1) | (d << 7);
      d = c;
    }
  if (d)
    a[0] ^= 0xe1;
}

static void
grub_gcm_mul (grub_uint8_t *a, const grub_uint8_t *b)
{
  grub_uint8_t res[16], bs[16];
  int i;
  grub_memcpy (bs, b, 16);
  grub_memset (res, 0, 16);
  for (i = 0; i < 128; i++)
    {
      if ((a[i / 8] << (i % 8)) & 0x80)
	grub_crypto_xor (res, res, bs, 16);
      grub_gcm_mul_x (bs);
    }

  grub_memcpy (a, res, 16);
}

static gcry_err_code_t
grub_gcm_decrypt (grub_crypto_cipher_handle_t cipher,
		  grub_uint8_t *out, const grub_uint8_t *in,
		  grub_size_t psize,
		  const grub_uint8_t *aad,
		  grub_size_t aadsize,
		  void *mac_out, const void *nonce,
		  unsigned nonce_len, unsigned m)
{
  grub_uint8_t iv[16];
  grub_uint8_t mul[16];
  grub_uint8_t mac[16], h[16], mac_xor[16];
  unsigned i, j;
  gcry_err_code_t err;

  grub_memset (mac, 0, sizeof (mac));

  err = grub_crypto_ecb_encrypt (cipher, h, mac, 16);
  if (err)
    return err;

  if (nonce_len == 12)
    {
      grub_memcpy (iv, nonce, 12);
      iv[12] = 0;
      iv[13] = 0;
      iv[14] = 0;
      iv[15] = 1;
    }
  else
    {
      grub_memset (iv, 0, sizeof (iv));
      grub_memcpy (iv, nonce, nonce_len > sizeof (iv) ? sizeof (iv) : nonce_len);
      grub_gcm_mul (iv, h);
      iv[15] ^= nonce_len * 8;
      grub_gcm_mul (iv, h);
    }

  err = grub_crypto_ecb_encrypt (cipher, mac_xor, iv, 16);
  if (err)
    return err;

  for (i = 0; i < (aadsize + 15) / 16; i++)
    {
      grub_size_t csize;
      csize = 16;
      if (csize > aadsize - 16 * i)
	csize = aadsize - 16 * i;
      grub_crypto_xor (mac, mac, aad + 16 * i, csize);
      grub_gcm_mul (mac, h);
    }

  for (i = 0; i < (psize + 15) / 16; i++)
    {
      grub_size_t csize;
      csize = 16;
      if (csize > psize - 16 * i)
	csize = psize - 16 * i;
      for (j = 0; j < 4; j++)
	{
	  iv[15 - j]++;
	  if (iv[15 - j] != 0)
	    break;
	}
      grub_crypto_xor (mac, mac, in + 16 * i, csize);
      grub_gcm_mul (mac, h);
      err = grub_crypto_ecb_encrypt (cipher, mul, iv, 16);
      if (err)
	return err;
      grub_crypto_xor (out + 16 * i, in + 16 * i, mul, csize);
    }
  for (j = 0; j < 8; j++)
    mac[15 - j] ^= ((((grub_uint64_t) psize) * 8) >> (8 * j));
  for (j = 0; j < 8; j++)
    mac[7 - j] ^= ((((grub_uint64_t) aadsize) * 8) >> (8 * j));
  grub_gcm_mul (mac, h);

  if (mac_out)
    grub_crypto_xor (mac_out, mac, mac_xor, m);

  return GPG_ERR_NO_ERROR;
}


static gcry_err_code_t
algo_decrypt (grub_crypto_cipher_handle_t cipher, grub_uint64_t algo,
	      grub_uint8_t *out, const grub_uint8_t *in,
	      grub_size_t psize,
	      const grub_uint8_t *aad,
	      grub_size_t aadsize,
	      void *mac_out, const void *nonce,
	      unsigned l, unsigned m)
{
  switch (algo)
    {
    case GRUB_ZFS_ALGO_CCM:
      return grub_ccm_decrypt (cipher, out, in, psize,
			       aad, aadsize, mac_out, nonce,
			       l <= 15 ? l : 0, m);
    case GRUB_ZFS_ALGO_GCM:
      return grub_gcm_decrypt (cipher, out, in, psize,
			       aad, aadsize, mac_out, nonce,
			       l <= 15 ? l : 0, m);
    default:
      return GPG_ERR_CIPHER_ALGO;
    }
}

static grub_err_t
grub_zfs_decrypt_oracle (grub_crypto_cipher_handle_t cipher,
			 grub_uint64_t algo,
			 const void *nonce,
			 char *buf, grub_size_t size,
			 const grub_uint32_t *expected_mac,
			 grub_zfs_endian_t endian)
{
  grub_uint32_t mac[4];
  unsigned i;
  grub_uint32_t sw[4];
  gcry_err_code_t err;

  grub_memcpy (sw, nonce, 16);
  if (endian != GRUB_ZFS_BIG_ENDIAN)
    for (i = 0; i < 4; i++)
      sw[i] = grub_swap_bytes32 (sw[i]);

  if (!cipher)
    return grub_error (GRUB_ERR_ACCESS_DENIED,
		       N_("no decryption key available"));
  err = algo_decrypt (cipher, algo,
		      (grub_uint8_t *) buf,
		      (grub_uint8_t *) buf,
		      size, NULL, 0, mac,
		      sw, 12, 12);
  if (err)
    return grub_crypto_gcry_error (err);

  for (i = 0; i < 3; i++)
    if (grub_zfs_to_cpu32 (expected_mac[i], endian)
	!= grub_be_to_cpu32 (mac[i]))
      return grub_error (GRUB_ERR_BAD_FS, N_("MAC verification failed"));
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_zfs_decrypt_datto (const struct grub_zfs_datto_key *key,
			const grub_uint32_t *nonce, grub_uint64_t salt,
			char *buf, grub_size_t size,
			const char *aadbuf, grub_size_t aadsize,
			const grub_uint64_t *expected_mac,
			grub_zfs_endian_t endian)
{
  grub_uint64_t mac[2];
  unsigned i;
  grub_uint8_t extractkey[64];
  grub_uint8_t expandkey[64];
  grub_uint8_t t[9];
  gcry_err_code_t err;
  grub_crypto_cipher_handle_t cipher;

  if (!key)
    return grub_error (GRUB_ERR_ACCESS_DENIED,
		       N_("no decryption key available"));

  /* A special case of HKDF with 512 > keylen.  */
  err = grub_crypto_hmac_buffer (GRUB_MD_SHA512,
				 "", 0,
				 key->master_key, key->master_keylen,
				 extractkey);
  if (err)
    return grub_crypto_gcry_error (err);

  grub_memcpy(t, &salt, 8);
  t[8] = 1;
  err = grub_crypto_hmac_buffer (GRUB_MD_SHA512,
				 extractkey, 64,
				 t, 9, expandkey);
  if (err)
    return grub_crypto_gcry_error (err);

  cipher = grub_crypto_cipher_open (GRUB_CIPHER_AES);
  if (!cipher)
    return grub_crypto_gcry_error (err);
  err = grub_crypto_cipher_set_key (cipher, expandkey, datto_algos[key->algo].cryptkeylen);
  if (err)
    {
      grub_crypto_cipher_close (cipher);
      return grub_crypto_gcry_error (err);
    }

  err = algo_decrypt (cipher, datto_algos[key->algo].used_algo,
		      (grub_uint8_t *) buf,
		      (grub_uint8_t *) buf,
		      size, (grub_uint8_t *) aadbuf, aadsize, mac,
		      nonce, 12, 16);
  grub_crypto_cipher_close (cipher);
  if (err)
    return grub_crypto_gcry_error (err);

  for (i = 0; i < 2; i++)
    if (grub_zfs_to_cpu64 (expected_mac[i], endian)
  	!= grub_le_to_cpu64 (mac[i]))
      grub_dprintf("zfs", N_("MAC verification failed"));
  return GRUB_ERR_NONE;
}

static grub_crypto_cipher_handle_t
grub_zfs_load_key_oracle (const struct grub_zfs_key_oracle *key,
			  grub_size_t keysize,
			  grub_uint64_t salt,
			  grub_uint64_t algo)
{
  unsigned keylen;
  struct grub_zfs_wrap_key *wrap_key;
  grub_crypto_cipher_handle_t ret = NULL;

  if (keysize != sizeof (*key))
    {
      grub_dprintf ("zfs", "Unexpected key size %" PRIuGRUB_SIZE "\n", keysize);
      return 0;
    }

  if (grub_memcmp (key->enc_key + 32, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16)
      == 0)
    keylen = 16;
  else if (grub_memcmp (key->enc_key + 40, "\0\0\0\0\0\0\0\0", 8) == 0)
    keylen = 24;
  else
    keylen = 32;

  for (wrap_key = zfs_wrap_keys; wrap_key; wrap_key = wrap_key->next)
    {
      grub_crypto_cipher_handle_t cipher;
      grub_uint8_t decrypted[32], mac[32], wrap_key_real[32];
      gcry_err_code_t err;
      cipher = grub_crypto_cipher_open (GRUB_CIPHER_AES);
      if (!cipher)
	{
	  grub_errno = GRUB_ERR_NONE;
	  return 0;
	}
      grub_memset (wrap_key_real, 0, sizeof (wrap_key_real));
      err = 0;
      if (!wrap_key->is_passphrase)
	grub_memcpy(wrap_key_real, wrap_key->key,
		    wrap_key->keylen < keylen ? wrap_key->keylen : keylen);
      else
	err = grub_crypto_pbkdf2 (GRUB_MD_SHA1,
				  (const grub_uint8_t *) wrap_key->key,
				  wrap_key->keylen,
				  (const grub_uint8_t *) &salt, sizeof (salt),
				  1000, wrap_key_real, keylen);
      if (err)
	{
	  grub_errno = GRUB_ERR_NONE;
	  grub_crypto_cipher_close (cipher);
	  continue;
	}

      err = grub_crypto_cipher_set_key (cipher, wrap_key_real,
					keylen);
      if (err)
	{
	  grub_errno = GRUB_ERR_NONE;
	  grub_crypto_cipher_close (cipher);
	  continue;
	}

      err = algo_decrypt (cipher, algo, decrypted, key->unknown_purpose_key, 32,
			  NULL, 0, mac, key->unknown_purpose_nonce, 13, 16);
      if (err || (grub_crypto_memcmp (mac, key->unknown_purpose_key + 32, 16)
		  != 0))
	{
	  grub_dprintf ("zfs", "key loading failed\n");
	  grub_errno = GRUB_ERR_NONE;
	  grub_crypto_cipher_close (cipher);
	  continue;
	}

      err = algo_decrypt (cipher, algo, decrypted, key->enc_key, keylen, NULL, 0,
			  mac, key->enc_nonce, 13, 16);
      if (err || grub_crypto_memcmp (mac, key->enc_key + keylen, 16) != 0)
	{
	  grub_dprintf ("zfs", "key loading failed\n");
	  grub_errno = GRUB_ERR_NONE;
	  grub_crypto_cipher_close (cipher);
	  continue;
	}
      ret = grub_crypto_cipher_open (GRUB_CIPHER_AES);
      if (!ret)
	{
	  grub_errno = GRUB_ERR_NONE;
	  grub_crypto_cipher_close (cipher);
	  continue;
	}
      err = grub_crypto_cipher_set_key (ret, decrypted, keylen);
      if (err)
	{
	  grub_errno = GRUB_ERR_NONE;
	  grub_crypto_cipher_close (ret);
	  grub_crypto_cipher_close (cipher);
	  continue;
	}
      grub_crypto_cipher_close (cipher);
      return ret;
    }
  return NULL;
}

static struct grub_zfs_datto_key
grub_zfs_load_key_datto (const grub_uint8_t *iv, grub_size_t ivlen,
			 const grub_uint8_t *mac_in, grub_size_t mac_inlen,
			 const grub_uint8_t *master, grub_size_t masterlen,
			 const grub_uint8_t *hmac, grub_size_t hmaclen,
			 const grub_uint8_t *pbkdf2salt, grub_size_t pbkdf2saltlen,
			 grub_uint64_t pbkdf2iters, grub_uint64_t guid, grub_uint64_t algo, grub_uint64_t version)
{
  struct grub_zfs_wrap_key *wrap_key;
  struct grub_zfs_datto_key ret = { 0 };

  if (algo <= 2 || algo >= ARRAY_SIZE(datto_algos))
    {
      grub_error(GRUB_ERR_BAD_FS, "unsupported crypto algo");
      return ret;
    }

  /* Note: if last two ever become variable they need to be checkecked as matching algo. Especially mac_inlen.  */
  if (masterlen < datto_algos[algo].masterkeylen || hmaclen != 64 || mac_inlen != 16)
    {
      grub_error(GRUB_ERR_BAD_FS, "crypto keys are invalid");
      return ret;
    }

  masterlen = datto_algos[algo].masterkeylen;

  grub_uint8_t ciphertext[MAX_WRAPCIPHERTEXTLEN];
  grub_uint8_t plaintext[MAX_WRAPCIPHERTEXTLEN];
  grub_uint8_t mac_computed[MAX_MACLEN];
  struct {
    grub_uint64_t guid;
    grub_uint64_t algo;
    grub_uint64_t version;
  } aad = {
    grub_cpu_to_le64(guid),
    grub_cpu_to_le64(algo),
    grub_cpu_to_le64(version)
  };

  grub_memcpy(ciphertext, master, masterlen);
  grub_memcpy(ciphertext + masterlen, hmac, hmaclen);

  for (wrap_key = zfs_wrap_keys; wrap_key; wrap_key = wrap_key->next)
    {
      grub_crypto_cipher_handle_t cipher;
      grub_uint8_t wrap_key_real[32] = { 0 };
      gcry_err_code_t err = 0;

      cipher = grub_crypto_cipher_open (GRUB_CIPHER_AES);
      if (!cipher)
	{
	  grub_errno = GRUB_ERR_NONE;
	  return ret;
	}
      grub_memset (wrap_key_real, 0, sizeof (wrap_key_real));

      if (!wrap_key->is_passphrase)
	grub_memcpy(wrap_key_real, wrap_key->key,
		    wrap_key->keylen < datto_algos[algo].wrapkeylen ? wrap_key->keylen : datto_algos[algo].wrapkeylen);
      else
	// TODO: PBKDF2
	err = grub_crypto_pbkdf2 (GRUB_MD_SHA1,
				  (const grub_uint8_t *) wrap_key->key,
				  wrap_key->keylen,
				  pbkdf2salt, pbkdf2saltlen,
				  pbkdf2iters, wrap_key_real, datto_algos[algo].wrapkeylen);

      if (err)
	{
	  grub_errno = GRUB_ERR_NONE;
	  grub_crypto_cipher_close (cipher);
	  continue;
	}

      err = grub_crypto_cipher_set_key (cipher, wrap_key_real, datto_algos[algo].wrapkeylen);
      if (err)
	{
	  grub_errno = GRUB_ERR_NONE;
	  grub_crypto_cipher_close (cipher);
	  continue;
	}

      err = algo_decrypt (cipher, datto_algos[algo].used_algo,
			  plaintext, ciphertext, masterlen + hmaclen,
			  (grub_uint8_t *) &aad, sizeof(aad),
			  mac_computed, iv, ivlen, mac_inlen);
      if (err || (grub_crypto_memcmp (mac_computed, mac_in, mac_inlen) != 0))
	{
	  grub_dprintf ("zfs", "key loading failed\n");
	  grub_errno = GRUB_ERR_NONE;
	  grub_crypto_cipher_close (cipher);
	  continue;
	}

      ret.master_keylen = masterlen;
      ret.master_key = grub_malloc(ret.master_keylen);
      if (!ret.master_key)
	return ret;
      grub_memcpy(ret.master_key, plaintext, masterlen);
      ret.hmac_key = grub_malloc(hmaclen);
      if (!ret.hmac_key)
	{
	  grub_free(ret.master_key);
	  ret.master_key = NULL;
	  return ret;
	}
      ret.algo = algo;
      grub_memcpy(ret.hmac_key, plaintext + masterlen, hmaclen);
      return ret;
    }
  return ret;
}

static const struct grub_arg_option options[] =
  {
    {"raw", 'r', 0, N_("Assume input is raw."), 0, 0},
    {"hex", 'h', 0, N_("Assume input is hex."), 0, 0},
    {"passphrase", 'p', 0, N_("Assume input is passphrase."), 0, 0},
    {0, 0, 0, 0, 0, 0}
  };

static grub_err_t
grub_cmd_zfs_key (grub_extcmd_context_t ctxt, int argc, char **args)
{
  grub_uint8_t buf[1024];
  grub_ssize_t real_size;

  if (argc > 0)
    {
      grub_file_t file;
      file = grub_file_open (args[0], GRUB_FILE_TYPE_ZFS_ENCRYPTION_KEY);
      if (!file)
	return grub_errno;
      real_size = grub_file_read (file, buf, 1024);
      if (real_size < 0)
	return grub_errno;
    }
  else
    {
      grub_xputs (_("Enter ZFS password: "));
      if (!grub_password_get ((char *) buf, 1023))
	return grub_errno;
      real_size = grub_strlen ((char *) buf);
    }

  if (ctxt->state[1].set)
    {
      int i;
      grub_err_t err;
      for (i = 0; i < real_size / 2; i++)
	{
	  char c1 = grub_tolower (buf[2 * i]) - '0';
	  char c2 = grub_tolower (buf[2 * i + 1]) - '0';
	  if (c1 > 9)
	    c1 += '0' - 'a' + 10;
	  if (c2 > 9)
	    c2 += '0' - 'a' + 10;
	  buf[i] = (c1 << 4) | c2;
	}
      err = grub_zfs_add_key (buf, real_size / 2, 0);
      if (err)
	return err;
      return GRUB_ERR_NONE;
    }

  return grub_zfs_add_key (buf, real_size,
			   ctxt->state[2].set
			   || (argc == 0 && !ctxt->state[0].set
			       && !ctxt->state[1].set));
}

struct grub_zfs_decryptor grub_zfs_decrypt_real = {
  .decrypt_oracle = grub_zfs_decrypt_oracle,
  .decrypt_datto = grub_zfs_decrypt_datto,
  .load_key_oracle = grub_zfs_load_key_oracle,
  .load_key_datto = grub_zfs_load_key_datto,
};
static grub_extcmd_t cmd_key;

GRUB_MOD_INIT(zfscrypt)
{
  grub_zfs_decrypt = &grub_zfs_decrypt_real;
  cmd_key = grub_register_extcmd ("zfskey", grub_cmd_zfs_key, 0,
				  N_("[-h|-p|-r] [FILE]"),
				  N_("Import ZFS wrapping key stored in FILE."),
				  options);
}

GRUB_MOD_FINI(zfscrypt)
{
  grub_zfs_decrypt = 0;
  grub_unregister_extcmd (cmd_key);
}
