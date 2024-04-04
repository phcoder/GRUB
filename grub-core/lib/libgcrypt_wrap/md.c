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
///static int md_digest_length( int algo );


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
