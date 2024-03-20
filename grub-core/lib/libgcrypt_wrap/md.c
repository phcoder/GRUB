#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "g10lib.h"

typedef struct gcry_md_list
{
  const gcry_md_spec_t *spec;
  struct gcry_md_list *next;
  size_t actual_struct_size;     /* Allocated size of this structure. */
  PROPERLY_ALIGNED_TYPE context[1];
} GcryDigestEntry;

/* This structure is put right after the gcry_md_hd_t buffer, so that
 * only one memory block is needed. */
struct gcry_md_context
{
  int  magic;
  struct {
    unsigned int secure:1;
    unsigned int finalized:1;
    unsigned int bugemu1:1;
    unsigned int hmac:1;
  } flags;
  size_t actual_handle_size;     /* Allocated size of this handle. */
  GcryDigestEntry *list;
};

#define CTX_MAGIC_NORMAL 0x11071961
#define CTX_MAGIC_SECURE 0x16917011

static gcry_err_code_t md_enable (gcry_md_hd_t hd, int algo);
static void md_close (gcry_md_hd_t a);
static void md_write (gcry_md_hd_t a, const void *inbuf, size_t inlen);
static byte *md_read( gcry_md_hd_t a, int algo );
///static int md_get_algo( gcry_md_hd_t a );
static int md_digest_length( int algo );

int
_gcry_md_map_name (const char *string)
{
  const gcry_md_spec_t *spec;

  if (!string)
    return 0;

  /* If the string starts with a digit (optionally prefixed with
     either "OID." or "oid."), we first look into our table of ASN.1
     object identifiers to figure out the algorithm */
  spec = grub_crypto_lookup_md_by_oid (string);
  if (spec)
    return spec->algo;

  /* Not found, search a matching digest name.  */
  spec = grub_crypto_lookup_md_by_name (string);
  if (spec)
    return spec->algo;

  return 0;
}

static gcry_err_code_t
check_digest_algo (int algorithm)
{
  const gcry_md_spec_t *spec;

  spec = spec_from_algo (algorithm);
  if (spec && !spec->flags.disabled && (spec->flags.fips || !fips_mode ()))
    return 0;

  return GPG_ERR_DIGEST_ALGO;

}

/****************
 * Open a message digest handle for use with algorithm ALGO.
 * More algorithms may be added by md_enable(). The initial algorithm
 * may be 0.
 */
static gcry_err_code_t
md_open (gcry_md_hd_t *h, int algo, unsigned int flags)
{
  gcry_err_code_t err = 0;
  int secure = !!(flags & GCRY_MD_FLAG_SECURE);
  int hmac =   !!(flags & GCRY_MD_FLAG_HMAC);
  int bufsize = secure ? 512 : 1024;
  gcry_md_hd_t hd;
  size_t n;

  /* Allocate a memory area to hold the caller visible buffer with it's
   * control information and the data required by this module. Set the
   * context pointer at the beginning to this area.
   * We have to use this strange scheme because we want to hide the
   * internal data but have a variable sized buffer.
   *
   *	+---+------+---........------+-------------+
   *	!ctx! bctl !  buffer	     ! private	   !
   *	+---+------+---........------+-------------+
   *	  !			      ^
   *	  !---------------------------!
   *
   * We have to make sure that private is well aligned.
   */
  n = offsetof (struct gcry_md_handle, buf) + bufsize;
  n = ((n + sizeof (PROPERLY_ALIGNED_TYPE) - 1)
       / sizeof (PROPERLY_ALIGNED_TYPE)) * sizeof (PROPERLY_ALIGNED_TYPE);

  /* Allocate and set the Context pointer to the private data */
  if (secure)
    hd = xtrymalloc_secure (n + sizeof (struct gcry_md_context));
  else
    hd = xtrymalloc (n + sizeof (struct gcry_md_context));

  if (! hd)
    err = gpg_err_code_from_errno (errno);

  if (! err)
    {
      struct gcry_md_context *ctx;

      ctx = (void *) (hd->buf - offsetof (struct gcry_md_handle, buf) + n);
      /* Setup the globally visible data (bctl in the diagram).*/
      hd->ctx = ctx;
      hd->bufsize = n - offsetof (struct gcry_md_handle, buf);
      hd->bufpos = 0;

      /* Initialize the private data. */
      wipememory2 (ctx, 0, sizeof *ctx);
      ctx->magic = secure ? CTX_MAGIC_SECURE : CTX_MAGIC_NORMAL;
      ctx->actual_handle_size = n + sizeof (struct gcry_md_context);
      ctx->flags.secure = secure;
      ctx->flags.hmac = hmac;
      ctx->flags.bugemu1 = !!(flags & GCRY_MD_FLAG_BUGEMU1);
    }

  if (! err)
    {
      if (algo)
	{
	  err = md_enable (hd, algo);
	  if (err)
	    md_close (hd);
	}
    }

  if (! err)
    *h = hd;

  return err;
}

/* Create a message digest object for algorithm ALGO.  FLAGS may be
   given as an bitwise OR of the gcry_md_flags values.  ALGO may be
   given as 0 if the algorithms to be used are later set using
   gcry_md_enable. H is guaranteed to be a valid handle or NULL on
   error.  */
gcry_err_code_t
_gcry_md_open (gcry_md_hd_t *h, int algo, unsigned int flags)
{
  gcry_err_code_t rc;
  gcry_md_hd_t hd;

  if ((flags & ~(GCRY_MD_FLAG_SECURE
                 | GCRY_MD_FLAG_HMAC
                 | GCRY_MD_FLAG_BUGEMU1)))
    rc = GPG_ERR_INV_ARG;
  else
    rc = md_open (&hd, algo, flags);

  *h = rc? NULL : hd;
  return rc;
}

static gcry_err_code_t
md_copy (gcry_md_hd_t ahd, gcry_md_hd_t *b_hd)
{
  gcry_err_code_t err = 0;
  struct gcry_md_context *a = ahd->ctx;
  struct gcry_md_context *b;
  GcryDigestEntry *ar, *br;
  gcry_md_hd_t bhd;
  size_t n;

  if (ahd->bufpos)
    md_write (ahd, NULL, 0);

  n = (char *) ahd->ctx - (char *) ahd;
  if (a->flags.secure)
    bhd = xtrymalloc_secure (n + sizeof (struct gcry_md_context));
  else
    bhd = xtrymalloc (n + sizeof (struct gcry_md_context));

  if (!bhd)
    {
      err = gpg_err_code_from_syserror ();
      goto leave;
    }

  bhd->ctx = b = (void *) ((char *) bhd + n);
  /* No need to copy the buffer due to the write above. */
  gcry_assert (ahd->bufsize == (n - offsetof (struct gcry_md_handle, buf)));
  bhd->bufsize = ahd->bufsize;
  bhd->bufpos = 0;
  gcry_assert (! ahd->bufpos);
  memcpy (b, a, sizeof *a);
  b->list = NULL;

  /* Copy the complete list of algorithms.  The copied list is
     reversed, but that doesn't matter. */
  for (ar = a->list; ar; ar = ar->next)
    {
      if (a->flags.secure)
        br = xtrymalloc_secure (ar->actual_struct_size);
      else
        br = xtrymalloc (ar->actual_struct_size);
      if (!br)
        {
          err = gpg_err_code_from_syserror ();
          md_close (bhd);
          goto leave;
        }

      memcpy (br, ar, ar->actual_struct_size);
      br->next = b->list;
      b->list = br;
    }

  *b_hd = bhd;

 leave:
  return err;
}

gcry_err_code_t
_gcry_md_copy (gcry_md_hd_t *handle, gcry_md_hd_t hd)
{
  gcry_err_code_t rc;

  rc = md_copy (hd, handle);
  if (rc)
    *handle = NULL;
  return rc;
}


/*
 * Reset all contexts and discard any buffered stuff.  This may be used
 * instead of a md_close(); md_open().
 */
void
_gcry_md_reset (gcry_md_hd_t a)
{
  GcryDigestEntry *r;

  /* Note: We allow this even in fips non operational mode.  */

  a->bufpos = a->ctx->flags.finalized = 0;

  if (a->ctx->flags.hmac)
    for (r = a->ctx->list; r; r = r->next)
      {
        memcpy (r->context, (char *)r->context + r->spec->contextsize,
                r->spec->contextsize);
      }
  else
    for (r = a->ctx->list; r; r = r->next)
      {
        memset (r->context, 0, r->spec->contextsize);
        (*r->spec->init) (r->context,
                          a->ctx->flags.bugemu1? GCRY_MD_FLAG_BUGEMU1:0);
      }
}

static void
md_close (gcry_md_hd_t a)
{
  GcryDigestEntry *r, *r2;

  if (! a)
    return;
  for (r = a->ctx->list; r; r = r2)
    {
      r2 = r->next;
      wipememory (r, r->actual_struct_size);
      xfree (r);
    }

  wipememory (a, a->ctx->actual_handle_size);
  xfree(a);
}


void
_gcry_md_close (gcry_md_hd_t hd)
{
  /* Note: We allow this even in fips non operational mode.  */
  md_close (hd);
}

static void
md_write (gcry_md_hd_t a, const void *inbuf, size_t inlen)
{
  GcryDigestEntry *r;

  for (r = a->ctx->list; r; r = r->next)
    {
      if (a->bufpos)
	(*r->spec->write) (r->context, a->buf, a->bufpos);
      (*r->spec->write) (r->context, inbuf, inlen);
    }
  a->bufpos = 0;
}


/* Note that this function may be used after finalize and read to keep
   on writing to the transform function so to mitigate timing
   attacks.  */
void
_gcry_md_write (gcry_md_hd_t hd, const void *inbuf, size_t inlen)
{
  md_write (hd, inbuf, inlen);
}

static void
md_final (gcry_md_hd_t a)
{
  GcryDigestEntry *r;

  if (a->ctx->flags.finalized)
    return;

  if (a->bufpos)
    md_write (a, NULL, 0);

  for (r = a->ctx->list; r; r = r->next)
    (*r->spec->final) (r->context);

  a->ctx->flags.finalized = 1;

  if (!a->ctx->flags.hmac)
    return;

  for (r = a->ctx->list; r; r = r->next)
    {
      byte *p;
      size_t dlen = r->spec->mdlen;
      byte *hash;
      gcry_err_code_t err;

      if (r->spec->read == NULL)
        continue;

      p = r->spec->read (r->context);

      if (a->ctx->flags.secure)
        hash = xtrymalloc_secure (dlen);
      else
        hash = xtrymalloc (dlen);
      if (!hash)
        {
          err = gpg_err_code_from_errno (errno);
          _gcry_fatal_error (err, NULL);
        }

      memcpy (hash, p, dlen);
      memcpy (r->context, (char *)r->context + r->spec->contextsize * 2,
              r->spec->contextsize);
      (*r->spec->write) (r->context, hash, dlen);
      (*r->spec->final) (r->context);
      xfree (hash);
    }
}

static gcry_err_code_t
md_enable (gcry_md_hd_t hd, int algorithm)
{
  struct gcry_md_context *h = hd->ctx;
  const gcry_md_spec_t *spec;
  GcryDigestEntry *entry;
  gcry_err_code_t err = 0;

  for (entry = h->list; entry; entry = entry->next)
    if (entry->spec->algo == algorithm)
      return 0; /* Already enabled */

  spec = spec_from_algo (algorithm);
  if (!spec)
    {
      log_debug ("md_enable: algorithm %d not available\n", algorithm);
      err = GPG_ERR_DIGEST_ALGO;
    }

  if (!err && spec->flags.disabled)
    err = GPG_ERR_DIGEST_ALGO;

  /* Any non-FIPS algorithm should go this way */
  if (!err && !spec->flags.fips && fips_mode ())
    err = GPG_ERR_DIGEST_ALGO;

  if (!err && h->flags.hmac && spec->read == NULL)
    {
      /* Expandable output function cannot act as part of HMAC. */
      err = GPG_ERR_DIGEST_ALGO;
    }

  if (!err)
    {
      size_t size = (sizeof (*entry)
                     + spec->contextsize * (h->flags.hmac? 3 : 1)
                     - sizeof (entry->context));

      /* And allocate a new list entry. */
      if (h->flags.secure)
	entry = xtrymalloc_secure (size);
      else
	entry = xtrymalloc (size);

      if (! entry)
	err = gpg_err_code_from_errno (errno);
      else
	{
	  entry->spec = spec;
	  entry->next = h->list;
          entry->actual_struct_size = size;
	  h->list = entry;

	  /* And init this instance. */
	  entry->spec->init (entry->context,
                             h->flags.bugemu1? GCRY_MD_FLAG_BUGEMU1:0);
	}
    }

  return err;
}


gcry_err_code_t
_gcry_md_enable (gcry_md_hd_t hd, int algorithm)
{
  return md_enable (hd, algorithm);
}

gcry_err_code_t
_gcry_md_ctl (gcry_md_hd_t hd, int cmd, void *buffer, size_t buflen)
{
  gcry_err_code_t rc = 0;

  (void)buflen; /* Currently not used.  */
  (void)buffer;

  switch (cmd)
    {
    case GCRYCTL_FINALIZE:
      md_final (hd);
      break;
    case GCRYCTL_START_DUMP:
      break;
    case GCRYCTL_STOP_DUMP:
      break;
    default:
      rc = GPG_ERR_INV_OP;
    }
  return rc;
}

/****************
 * If ALGO is null get the digest for the used algo (which should be
 * only one)
 */
static byte *
md_read( gcry_md_hd_t a, int algo )
{
  GcryDigestEntry *r = a->ctx->list;

  if (! algo)
    {
      /* Return the first algorithm */
      if (r)
        {
          if (r->next)
            log_debug ("more than one algorithm in md_read(0)\n");
          if (r->spec->read)
            return r->spec->read (r->context);
        }
    }
  else
    {
      for (r = a->ctx->list; r; r = r->next)
	if (r->spec->algo == algo)
	  {
	    if (r->spec->read)
              return r->spec->read (r->context);
            break;
	  }
    }

  if (r && !r->spec->read)
    _gcry_fatal_error (GPG_ERR_DIGEST_ALGO,
                       "requested algo has no fixed digest length");
  else
    _gcry_fatal_error (GPG_ERR_DIGEST_ALGO, "requested algo not in md context");
  return NULL;
}


/*
 * Read out the complete digest, this function implictly finalizes
 * the hash.
 */
byte *
_gcry_md_read (gcry_md_hd_t hd, int algo)
{
  /* This function is expected to always return a digest, thus we
     can't return an error which we actually should do in
     non-operational state.  */
  _gcry_md_ctl (hd, GCRYCTL_FINALIZE, NULL, 0);
  return md_read (hd, algo);
}

/*
 * Shortcut function to hash a buffer with a given algo. The only
 * guaranteed supported algorithms are RIPE-MD160 and SHA-1. The
 * supplied digest buffer must be large enough to store the resulting
 * hash.  No error is returned, the function will abort on an invalid
 * algo.  DISABLED_ALGOS are ignored here.  */
void
_gcry_md_hash_buffer (int algo, void *digest,
                      const void *buffer, size_t length)
{
  const gcry_md_spec_t *spec;

  spec = spec_from_algo (algo);
  if (!spec)
    {
      log_debug ("md_hash_buffer: algorithm %d not available\n", algo);
      return;
    }

  /*  if (spec->hash_buffers != NULL)
    {
      gcry_buffer_t iov;

      iov.size = 0;
      iov.data = (void *)buffer;
      iov.off = 0;
      iov.len = length;

      if (spec->flags.disabled || (!spec->flags.fips && fips_mode ()))
        log_bug ("gcry_md_hash_buffer failed for algo %d: %s",
                algo, gpg_strerror (gcry_error (GPG_ERR_DIGEST_ALGO)));

      spec->hash_buffers (digest, spec->mdlen, &iov, 1);
    }
    else*/
    {
      /* For the others we do not have a fast function, so we use the
         normal functions. */
      gcry_md_hd_t h;
      gpg_err_code_t err;

      err = md_open (&h, algo, 0);
      if (err)
        log_bug ("gcry_md_open failed for algo %d: %s",
                algo, gpg_strerror (gcry_error(err)));
      md_write (h, (byte *) buffer, length);
      md_final (h);
      memcpy (digest, md_read (h, algo), md_digest_length (algo));
      md_close (h);
    }
}

/****************
 * Return the length of the digest
 */
static int
md_digest_length (int algorithm)
{
  const gcry_md_spec_t *spec;

  spec = spec_from_algo (algorithm);
  return spec? spec->mdlen : 0;
}


/****************
 * Return the length of the digest in bytes.
 * This function will return 0 in case of errors.
 */
unsigned int
_gcry_md_get_algo_dlen (int algorithm)
{
  return md_digest_length (algorithm);
}

/* Hmmm: add a mode to enumerate the OIDs
 *	to make g10/sig-check.c more portable */
static const byte *
md_asn_oid (int algorithm, size_t *asnlen, size_t *mdlen)
{
  const gcry_md_spec_t *spec;
  const byte *asnoid = NULL;

  spec = spec_from_algo (algorithm);
  if (spec)
    {
      if (asnlen)
	*asnlen = spec->asnlen;
      if (mdlen)
	*mdlen = spec->mdlen;
      asnoid = spec->asnoid;
    }
  else
    log_bug ("no ASN.1 OID for md algo %d\n", algorithm);

  return asnoid;
}


/****************
 * Return information about the given cipher algorithm
 * WHAT select the kind of information returned:
 *  GCRYCTL_TEST_ALGO:
 *	Returns 0 when the specified algorithm is available for use.
 *	buffer and nbytes must be zero.
 *  GCRYCTL_GET_ASNOID:
 *	Return the ASNOID of the algorithm in buffer. if buffer is NULL, only
 *	the required length is returned.
 *  GCRYCTL_SELFTEST
 *      Helper for the regression tests - shall not be used by applications.
 *
 * Note:  Because this function is in most cases used to return an
 * integer value, we can make it easier for the caller to just look at
 * the return value.  The caller will in all cases consult the value
 * and thereby detecting whether a error occurred or not (i.e. while checking
 * the block size)
 */
gcry_err_code_t
_gcry_md_algo_info (int algo, int what, void *buffer, size_t *nbytes)
{
  gcry_err_code_t rc;

  switch (what)
    {
    case GCRYCTL_TEST_ALGO:
      if (buffer || nbytes)
	rc = GPG_ERR_INV_ARG;
      else
	rc = check_digest_algo (algo);
      break;

    case GCRYCTL_GET_ASNOID:
      /* We need to check that the algo is available because
         md_asn_oid would otherwise raise an assertion. */
      rc = check_digest_algo (algo);
      if (!rc)
        {
          const char unsigned *asn;
          size_t asnlen;

          asn = md_asn_oid (algo, &asnlen, NULL);
          if (buffer && (*nbytes >= asnlen))
            {
              memcpy (buffer, asn, asnlen);
              *nbytes = asnlen;
            }
          else if (!buffer && nbytes)
            *nbytes = asnlen;
          else
            {
              if (buffer)
                rc = GPG_ERR_TOO_SHORT;
              else
                rc = GPG_ERR_INV_ARG;
            }
        }
      break;

    default:
      rc = GPG_ERR_INV_OP;
      break;
  }

  return rc;
}
