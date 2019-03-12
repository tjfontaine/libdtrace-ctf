/* CTF file creation.
   Copyright (c) 2006, 2019, Oracle and/or its affiliates. All rights reserved.

   Licensed under the Universal Permissive License v 1.0 as shown at
   http://oss.oracle.com/licenses/upl.

   Licensed under the GNU General Public License (GPL), version 2. See the file
   COPYING in the top level of this tree.  */

#include <sys/param.h>
#include <sys/mman.h>
#include <assert.h>
#include <gelf.h>
#include <string.h>
#include <ctf-impl.h>

/* To create an empty CTF container, we just declare a zeroed header and call
   ctf_bufopen() on it.  If ctf_bufopen succeeds, we mark the new container r/w
   and initialize the dynamic members.  We set dtvstrlen to 1 to reserve the
   first byte of the string table for a \0 byte, and we start assigning type
   IDs at 1 because type ID 0 is used as a sentinel.  */

ctf_file_t *
ctf_create (int *errp)
{
  static const ctf_header_t hdr = { .cth_preamble = {CTF_MAGIC, CTF_VERSION } };

  const unsigned long hashlen = 1024;
  ctf_dtdef_t **dthash = ctf_alloc (hashlen * sizeof (ctf_dtdef_t *));
  ctf_dvdef_t **dvhash = ctf_alloc (hashlen * sizeof (ctf_dvdef_t *));
  ctf_sect_t cts;
  ctf_file_t *fp;

  if (dthash == NULL)
    return (ctf_set_open_errno (errp, EAGAIN));

  if (dvhash == NULL)
    {
      ctf_free (dthash, hashlen * sizeof (ctf_dtdef_t *));
      return (ctf_set_open_errno (errp, EAGAIN));
    }

  cts.cts_name = _CTF_SECTION;
  cts.cts_type = SHT_PROGBITS;
  cts.cts_flags = 0;
  cts.cts_data = &hdr;
  cts.cts_size = sizeof (hdr);
  cts.cts_entsize = 1;
  cts.cts_offset = 0;

  if ((fp = ctf_bufopen (&cts, NULL, NULL, errp)) == NULL)
    {
      ctf_free (dthash, hashlen * sizeof (ctf_dtdef_t *));
      ctf_free (dvhash, hashlen * sizeof (ctf_dvdef_t *));
      return NULL;
    }

  fp->ctf_flags |= LCTF_RDWR;
  fp->ctf_dthashlen = hashlen;
  fp->ctf_dvhashlen = hashlen;
  memset (dthash, 0, hashlen * sizeof (ctf_dtdef_t *));
  memset (dvhash, 0, hashlen * sizeof (ctf_dvdef_t *));
  fp->ctf_dthash = dthash;
  fp->ctf_dvhash = dvhash;
  fp->ctf_dtvstrlen = 1;
  fp->ctf_dtnextid = 1;
  fp->ctf_dtoldid = 0;
  fp->ctf_snapshots = 0;
  fp->ctf_snapshot_lu = 0;

  return fp;
}

static unsigned char *
ctf_copy_smembers (ctf_dtdef_t *dtd, uint32_t soff, unsigned char *t)
{
  ctf_dmdef_t *dmd = ctf_list_next (&dtd->dtd_u.dtu_members);
  ctf_member_t ctm;

  for (; dmd != NULL; dmd = ctf_list_next (dmd))
    {
      if (dmd->dmd_name)
	{
	  ctm.ctm_name = soff;
	  soff += strlen (dmd->dmd_name) + 1;
	}
      else
	ctm.ctm_name = 0;

      ctm.ctm_type = (uint32_t) dmd->dmd_type;
      ctm.ctm_offset = (uint32_t) dmd->dmd_offset;

      memcpy (t, &ctm, sizeof (ctm));
      t += sizeof (ctm);
    }

  return t;
}

static unsigned char *
ctf_copy_lmembers (ctf_dtdef_t *dtd, uint32_t soff, unsigned char *t)
{
  ctf_dmdef_t *dmd = ctf_list_next (&dtd->dtd_u.dtu_members);
  ctf_lmember_t ctlm;

  for (; dmd != NULL; dmd = ctf_list_next (dmd))
    {
      if (dmd->dmd_name)
	{
	  ctlm.ctlm_name = soff;
	  soff += strlen (dmd->dmd_name) + 1;
	}
      else
	ctlm.ctlm_name = 0;

      ctlm.ctlm_type = (uint32_t) dmd->dmd_type;
      ctlm.ctlm_offsethi = CTF_OFFSET_TO_LMEMHI (dmd->dmd_offset);
      ctlm.ctlm_offsetlo = CTF_OFFSET_TO_LMEMLO (dmd->dmd_offset);

      memcpy (t, &ctlm, sizeof (ctlm));
      t += sizeof (ctlm);
    }

  return t;
}

static unsigned char *
ctf_copy_emembers (ctf_dtdef_t *dtd, uint32_t soff, unsigned char *t)
{
  ctf_dmdef_t *dmd = ctf_list_next (&dtd->dtd_u.dtu_members);
  ctf_enum_t cte;

  for (; dmd != NULL; dmd = ctf_list_next (dmd))
    {
      cte.cte_name = soff;
      cte.cte_value = dmd->dmd_value;
      soff += strlen (dmd->dmd_name) + 1;
      memcpy (t, &cte, sizeof (cte));
      t += sizeof (cte);
    }

  return t;
}

static unsigned char *
ctf_copy_membnames (ctf_dtdef_t *dtd, unsigned char *s)
{
  ctf_dmdef_t *dmd = ctf_list_next (&dtd->dtd_u.dtu_members);
  size_t len;

  for (; dmd != NULL; dmd = ctf_list_next (dmd))
    {
      if (dmd->dmd_name == NULL)
	continue;			/* Skip anonymous members.  */
      len = strlen (dmd->dmd_name) + 1;
      memcpy (s, dmd->dmd_name, len);
      s += len;
    }

  return s;
}

/* Sort a newly-constructed static variable array.  */

const char *sort_strtab_ = NULL;

static int
ctf_sort_var (const void *one_, const void *two_)
{
  const ctf_varent_t *one = one_;
  const ctf_varent_t *two = two_;
  const char *strtab = sort_strtab_;
  const char *n1 = strtab + CTF_NAME_OFFSET (one->ctv_name);
  const char *n2 = strtab + CTF_NAME_OFFSET (two->ctv_name);

  return (strcmp (n1, n2));
}

/* If the specified CTF container is writable and has been modified, reload this
   container with the updated type definitions.  In order to make this code and
   the rest of libctf as simple as possible, we perform updates by taking the
   dynamic type definitions and creating an in-memory CTF file containing the
   definitions, and then call ctf_bufopen() on it.  This not only leverages
   ctf_bufopen(), but also avoids having to bifurcate the rest of the library
   code with different lookup paths for static and dynamic type definitions.  We
/   are therefore optimizing greatly for lookup over update, which we assume will
   be an uncommon operation.  We perform one extra trick here for the benefit of
   callers and to keep our code simple: ctf_bufopen() will return a new
   ctf_file_t, but we want to keep the fp constant for the caller, so after
   ctf_bufopen() returns, we use memcpy to swap the interior of the old and new
   ctf_file_t's, and then free the old.  */
int
ctf_update (ctf_file_t *fp)
{
  ctf_file_t ofp, *nfp;
  ctf_header_t hdr;
  ctf_dtdef_t *dtd;
  ctf_dvdef_t *dvd;
  ctf_varent_t *dvarents;
  ctf_sect_t cts;

  unsigned char *s, *s0, *t;
  unsigned long i;
  size_t buf_size, type_size, nvars;
  void *buf;
  int err;

  if (!(fp->ctf_flags & LCTF_RDWR))
    return (ctf_set_errno (fp, ECTF_RDONLY));

  /* Update required?  */
  if (!(fp->ctf_flags & LCTF_DIRTY))
    return 0;

  /* Fill in an initial CTF header.  We will leave the label, object,
     and function sections empty and only output a header, type section,
     and string table.  The type section begins at a 4-byte aligned
     boundary past the CTF header itself (at relative offset zero).  */

  memset (&hdr, 0, sizeof (hdr));
  hdr.cth_magic = CTF_MAGIC;
  hdr.cth_version = CTF_VERSION;

  if (fp->ctf_flags & LCTF_CHILD)
    hdr.cth_parname = 1;		/* parname added just below.  */

  /* Iterate through the dynamic type definition list and compute the
     size of the CTF type section we will need to generate.  */

  for (type_size = 0, dtd = ctf_list_next (&fp->ctf_dtdefs);
       dtd != NULL; dtd = ctf_list_next (dtd))
    {
      uint32_t kind = LCTF_INFO_KIND (fp, dtd->dtd_data.ctt_info);
      uint32_t vlen = LCTF_INFO_VLEN (fp, dtd->dtd_data.ctt_info);

      if (dtd->dtd_data.ctt_size != CTF_LSIZE_SENT)
	type_size += sizeof (ctf_stype_t);
      else
	type_size += sizeof (ctf_type_t);

      switch (kind)
	{
	case CTF_K_INTEGER:
	case CTF_K_FLOAT:
	  type_size += sizeof (uint32_t);
	  break;
	case CTF_K_ARRAY:
	  type_size += sizeof (ctf_array_t);
	  break;
	case CTF_K_FUNCTION:
	  type_size += sizeof (uint32_t) * (vlen + (vlen & 1));
	  break;
	case CTF_K_STRUCT:
	case CTF_K_UNION:
	  if (dtd->dtd_data.ctt_size < CTF_LSTRUCT_THRESH)
	    type_size += sizeof (ctf_member_t) * vlen;
	  else
	    type_size += sizeof (ctf_lmember_t) * vlen;
	  break;
	case CTF_K_ENUM:
	  type_size += sizeof (ctf_enum_t) * vlen;
	  break;
	}
    }

  /* Computing the number of entries in the CTF variable section is much
     simpler.  */

  for (nvars = 0, dvd = ctf_list_next (&fp->ctf_dvdefs);
       dvd != NULL; dvd = ctf_list_next (dvd), nvars++);

  /* Fill in the string table and type offset and size, compute the size
     of the entire CTF buffer we need, and then allocate a new buffer and
     memcpy the finished header to the start of the buffer.  */

  hdr.cth_typeoff = hdr.cth_varoff + (nvars * sizeof (ctf_varent_t));
  hdr.cth_stroff = hdr.cth_typeoff + type_size;
  hdr.cth_strlen = fp->ctf_dtvstrlen;
  if (fp->ctf_parname != NULL)
    hdr.cth_strlen += strlen (fp->ctf_parname) + 1;

  buf_size = sizeof (ctf_header_t) + hdr.cth_stroff + hdr.cth_strlen;

  if ((buf = ctf_data_alloc (buf_size)) == MAP_FAILED)
    return (ctf_set_errno (fp, EAGAIN));

  memcpy (buf, &hdr, sizeof (ctf_header_t));
  t = (unsigned char *) buf + sizeof (ctf_header_t) + hdr.cth_varoff;
  s = s0 = (unsigned char *) buf + sizeof (ctf_header_t) + hdr.cth_stroff;

  s[0] = '\0';
  s++;

  if (fp->ctf_parname != NULL)
    {
      memcpy (s, fp->ctf_parname, strlen (fp->ctf_parname) + 1);
      s += strlen (fp->ctf_parname) + 1;
    }

  /* Work over the variable list, translating everything into
     ctf_varent_t's and filling out the string table, then sort the buffer
     of ctf_varent_t's.  */

  dvarents = (ctf_varent_t *) t;
  for (i = 0, dvd = ctf_list_next (&fp->ctf_dvdefs); dvd != NULL;
       dvd = ctf_list_next (dvd), i++)
    {
      ctf_varent_t *var = &dvarents[i];
      size_t len = strlen (dvd->dvd_name) + 1;

      var->ctv_name = (uint32_t) (s - s0);
      var->ctv_typeidx = dvd->dvd_type;
      memcpy (s, dvd->dvd_name, len);
      s += len;
    }
  assert (i == nvars);

  sort_strtab_ = s0;
  qsort (dvarents, nvars, sizeof (ctf_varent_t), ctf_sort_var);
  t += sizeof (ctf_varent_t) * nvars;

  assert (t == (unsigned char *) buf + sizeof (ctf_header_t) + hdr.cth_typeoff);

  /* We now take a final lap through the dynamic type definition list and
     copy the appropriate type records and strings to the output buffer.  */

  for (dtd = ctf_list_next (&fp->ctf_dtdefs);
       dtd != NULL; dtd = ctf_list_next (dtd))
    {

      uint32_t kind = LCTF_INFO_KIND (fp, dtd->dtd_data.ctt_info);
      uint32_t vlen = LCTF_INFO_VLEN (fp, dtd->dtd_data.ctt_info);

      ctf_array_t cta;
      uint32_t encoding;
      size_t len;

      if (dtd->dtd_name != NULL)
	{
	  dtd->dtd_data.ctt_name = (uint32_t) (s - s0);
	  len = strlen (dtd->dtd_name) + 1;
	  memcpy (s, dtd->dtd_name, len);
	  s += len;
	}
      else
	dtd->dtd_data.ctt_name = 0;

      if (dtd->dtd_data.ctt_size != CTF_LSIZE_SENT)
	len = sizeof (ctf_stype_t);
      else
	len = sizeof (ctf_type_t);

      memcpy (t, &dtd->dtd_data, len);
      t += len;

      switch (kind)
	{
	case CTF_K_INTEGER:
	case CTF_K_FLOAT:
	  if (kind == CTF_K_INTEGER)
	    {
	      encoding = CTF_INT_DATA (dtd->dtd_u.dtu_enc.cte_format,
				       dtd->dtd_u.dtu_enc.cte_offset,
				       dtd->dtd_u.dtu_enc.cte_bits);
	    }
	  else
	    {
	      encoding = CTF_FP_DATA (dtd->dtd_u.dtu_enc.cte_format,
				      dtd->dtd_u.dtu_enc.cte_offset,
				      dtd->dtd_u.dtu_enc.cte_bits);
	    }
	  memcpy (t, &encoding, sizeof (encoding));
	  t += sizeof (encoding);
	  break;

	case CTF_K_ARRAY:
	  cta.cta_contents = (uint32_t) dtd->dtd_u.dtu_arr.ctr_contents;
	  cta.cta_index = (uint32_t) dtd->dtd_u.dtu_arr.ctr_index;
	  cta.cta_nelems = dtd->dtd_u.dtu_arr.ctr_nelems;
	  memcpy (t, &cta, sizeof (cta));
	  t += sizeof (cta);
	  break;

	case CTF_K_FUNCTION:
	  {
	    uint32_t *argv = (uint32_t *) (uintptr_t) t;
	    uint32_t argc;

	    for (argc = 0; argc < vlen; argc++)
	      *argv++ = (uint32_t) dtd->dtd_u.dtu_argv[argc];

	    if (vlen & 1)
	      *argv++ = 0;	/* Pad to 4-byte boundary.  */

	    t = (unsigned char *) argv;
	    break;
	  }

	case CTF_K_STRUCT:
	case CTF_K_UNION:
	  if (dtd->dtd_data.ctt_size < CTF_LSTRUCT_THRESH)
	    t = ctf_copy_smembers (dtd, (uint32_t) (s - s0), t);
	  else
	    t = ctf_copy_lmembers (dtd, (uint32_t) (s - s0), t);
	  s = ctf_copy_membnames (dtd, s);
	  break;

	case CTF_K_ENUM:
	  t = ctf_copy_emembers (dtd, (uint32_t) (s - s0), t);
	  s = ctf_copy_membnames (dtd, s);
	  break;
	}
    }
  assert (t == (unsigned char *) buf + sizeof (ctf_header_t) + hdr.cth_stroff);

  /* Finally, we are ready to ctf_bufopen() the new container.  If this
     is successful, we then switch nfp and fp and free the old container.  */

  ctf_data_protect (buf, buf_size);
  cts.cts_name = _CTF_SECTION;
  cts.cts_type = SHT_PROGBITS;
  cts.cts_flags = 0;
  cts.cts_data = buf;
  cts.cts_size = buf_size;
  cts.cts_entsize = 1;
  cts.cts_offset = 0;

  if ((nfp = ctf_bufopen (&cts, NULL, NULL, &err)) == NULL)
    {
      ctf_data_free (buf, buf_size);
      return (ctf_set_errno (fp, err));
    }

  (void) ctf_setmodel (nfp, ctf_getmodel (fp));
  (void) ctf_import (nfp, fp->ctf_parent);

  nfp->ctf_refcnt = fp->ctf_refcnt;
  nfp->ctf_flags |= fp->ctf_flags & ~LCTF_DIRTY;
  nfp->ctf_data.cts_data = NULL;	/* Force ctf_data_free() on close.  */
  nfp->ctf_dthash = fp->ctf_dthash;
  nfp->ctf_dthashlen = fp->ctf_dthashlen;
  nfp->ctf_dtdefs = fp->ctf_dtdefs;
  nfp->ctf_dvhash = fp->ctf_dvhash;
  nfp->ctf_dvhashlen = fp->ctf_dvhashlen;
  nfp->ctf_dvdefs = fp->ctf_dvdefs;
  nfp->ctf_dtvstrlen = fp->ctf_dtvstrlen;
  nfp->ctf_dtnextid = fp->ctf_dtnextid;
  nfp->ctf_dtoldid = fp->ctf_dtnextid - 1;
  nfp->ctf_snapshots = fp->ctf_snapshots + 1;
  nfp->ctf_specific = fp->ctf_specific;

  nfp->ctf_snapshot_lu = fp->ctf_snapshots;

  fp->ctf_dthash = NULL;
  fp->ctf_dthashlen = 0;
  memset (&fp->ctf_dtdefs, 0, sizeof (ctf_list_t));

  fp->ctf_dvhash = NULL;
  fp->ctf_dvhashlen = 0;
  memset (&fp->ctf_dvdefs, 0, sizeof (ctf_list_t));

  memcpy (&ofp, fp, sizeof (ctf_file_t));
  memcpy (fp, nfp, sizeof (ctf_file_t));
  memcpy (nfp, &ofp, sizeof (ctf_file_t));

  /* Initialize the ctf_lookup_by_name top-level dictionary.  We keep an
     array of type name prefixes and the corresponding ctf_hash to use.
     NOTE: This code must be kept in sync with the code in ctf_bufopen().  */

  fp->ctf_lookups[0].ctl_hash = &fp->ctf_structs;
  fp->ctf_lookups[1].ctl_hash = &fp->ctf_unions;
  fp->ctf_lookups[2].ctl_hash = &fp->ctf_enums;
  fp->ctf_lookups[3].ctl_hash = &fp->ctf_names;

  nfp->ctf_refcnt = 1;		/* Force nfp to be freed.  */
  ctf_close (nfp);

  return 0;
}

void
ctf_dtd_insert (ctf_file_t *fp, ctf_dtdef_t *dtd)
{
  unsigned long h = dtd->dtd_type % (fp->ctf_dthashlen - 1);

  dtd->dtd_hash = fp->ctf_dthash[h];
  fp->ctf_dthash[h] = dtd;
  ctf_list_append (&fp->ctf_dtdefs, dtd);
}

void
ctf_dtd_delete (ctf_file_t *fp, ctf_dtdef_t *dtd)
{
  unsigned long h = dtd->dtd_type % (fp->ctf_dthashlen - 1);
  ctf_dtdef_t *p, **q = &fp->ctf_dthash[h];
  ctf_dmdef_t *dmd, *nmd;
  size_t len;

  for (p = *q; p != NULL; p = p->dtd_hash)
    {
      if (p != dtd)
	q = &p->dtd_hash;
      else
	break;
    }

  if (p != NULL)
    *q = p->dtd_hash;

  switch (LCTF_INFO_KIND (fp, dtd->dtd_data.ctt_info))
    {
    case CTF_K_STRUCT:
    case CTF_K_UNION:
    case CTF_K_ENUM:
      for (dmd = ctf_list_next (&dtd->dtd_u.dtu_members);
	   dmd != NULL; dmd = nmd)
	{
	  if (dmd->dmd_name != NULL)
	    {
	      len = strlen (dmd->dmd_name) + 1;
	      ctf_free (dmd->dmd_name, len);
	      fp->ctf_dtvstrlen -= len;
	    }
	  nmd = ctf_list_next (dmd);
	  ctf_free (dmd, sizeof (ctf_dmdef_t));
	}
      break;
    case CTF_K_FUNCTION:
      ctf_free (dtd->dtd_u.dtu_argv, sizeof (ctf_id_t) *
		LCTF_INFO_VLEN (fp, dtd->dtd_data.ctt_info));
      break;
    }

  if (dtd->dtd_name)
    {
      len = strlen (dtd->dtd_name) + 1;
      ctf_free (dtd->dtd_name, len);
      fp->ctf_dtvstrlen -= len;
    }

  ctf_list_delete (&fp->ctf_dtdefs, dtd);
  ctf_free (dtd, sizeof (ctf_dtdef_t));
}

ctf_dtdef_t *
ctf_dtd_lookup (ctf_file_t *fp, ctf_id_t type)
{
  unsigned long h = type % (fp->ctf_dthashlen - 1);
  ctf_dtdef_t *dtd;

  if (fp->ctf_dthash == NULL)
    return NULL;

  for (dtd = fp->ctf_dthash[h]; dtd != NULL; dtd = dtd->dtd_hash)
    {
      if (dtd->dtd_type == type)
	break;
    }

  return dtd;
}

void
ctf_dvd_insert (ctf_file_t *fp, ctf_dvdef_t *dvd)
{
  unsigned long h = ctf_hash_compute (dvd->dvd_name, strlen (dvd->dvd_name))
    % (fp->ctf_dvhashlen - 1);

  dvd->dvd_hash = fp->ctf_dvhash[h];
  fp->ctf_dvhash[h] = dvd;
  ctf_list_append (&fp->ctf_dvdefs, dvd);
}

void
ctf_dvd_delete (ctf_file_t *fp, ctf_dvdef_t *dvd)
{
  size_t len = strlen (dvd->dvd_name);
  unsigned long h = ctf_hash_compute (dvd->dvd_name, len)
    % (fp->ctf_dvhashlen - 1);
  ctf_dvdef_t *p, **q = &fp->ctf_dvhash[h];

  for (p = *q; p != NULL; p = p->dvd_hash)
    {
      if (p != dvd)
	q = &p->dvd_hash;
      else
	break;
    }

  if (p != NULL)
    *q = p->dvd_hash;

  if (dvd->dvd_name)
    {
      ctf_free (dvd->dvd_name, len + 1);
      fp->ctf_dtvstrlen -= len + 1;
    }

  ctf_list_delete (&fp->ctf_dvdefs, dvd);
  ctf_free (dvd, sizeof (ctf_dvdef_t));
}

ctf_dvdef_t *
ctf_dvd_lookup (ctf_file_t *fp, const char *name)
{
  unsigned long h = ctf_hash_compute (name, strlen (name))
    % (fp->ctf_dvhashlen - 1);
  ctf_dvdef_t *dvd;

  if (fp->ctf_dvhash == NULL)
    return NULL;

  for (dvd = fp->ctf_dvhash[h]; dvd != NULL; dvd = dvd->dvd_hash)
    {
      if (strcmp (name, dvd->dvd_name) == 0)
	break;
    }

  return dvd;
}

/* Discard all of the dynamic type definitions and variable definitions that
   have been added to the container since the last call to ctf_update().  We
   locate such types by scanning the dtd list and deleting elements that have
   type IDs greater than ctf_dtoldid, which is set by ctf_update(), above, and
   by scanning the variable list and deleting elements that have update IDs
   equal to the current value of the last-update snapshot count (indicating that
   they were added after the most recent call to ctf_update()).  */
int
ctf_discard (ctf_file_t *fp)
{
  ctf_snapshot_id_t last_update =
    { fp->ctf_dtoldid,
      fp->ctf_snapshot_lu + 1 };

  /* Update required?  */
  if (!(fp->ctf_flags & LCTF_DIRTY))
    return 0;

  return (ctf_rollback (fp, last_update));
}

ctf_snapshot_id_t
ctf_snapshot (ctf_file_t *fp)
{
  ctf_snapshot_id_t snapid;
  snapid.dtd_id = fp->ctf_dtnextid - 1;
  snapid.snapshot_id = fp->ctf_snapshots++;
  return snapid;
}

/* Like ctf_discard(), only discards everything after a particular ID.  */
int
ctf_rollback (ctf_file_t *fp, ctf_snapshot_id_t id)
{
  ctf_dtdef_t *dtd, *ntd;
  ctf_dvdef_t *dvd, *nvd;

  if (!(fp->ctf_flags & LCTF_RDWR))
    return (ctf_set_errno (fp, ECTF_RDONLY));

  if (fp->ctf_dtoldid > id.dtd_id)
    return (ctf_set_errno (fp, ECTF_OVERROLLBACK));

  if (fp->ctf_snapshot_lu >= id.snapshot_id)
    return (ctf_set_errno (fp, ECTF_OVERROLLBACK));

  for (dtd = ctf_list_next (&fp->ctf_dtdefs); dtd != NULL; dtd = ntd)
    {
      ntd = ctf_list_next (dtd);

      if (LCTF_TYPE_TO_INDEX (fp, dtd->dtd_type) <= id.dtd_id)
	continue;

      ctf_dtd_delete (fp, dtd);
    }

  for (dvd = ctf_list_next (&fp->ctf_dvdefs); dvd != NULL; dvd = nvd)
    {
      nvd = ctf_list_next (dvd);

      if (dvd->dvd_snapshots <= id.snapshot_id)
	continue;

      ctf_dvd_delete (fp, dvd);
    }

  fp->ctf_dtnextid = id.dtd_id + 1;
  fp->ctf_snapshots = id.snapshot_id;

  if (fp->ctf_snapshots == fp->ctf_snapshot_lu)
    fp->ctf_flags &= ~LCTF_DIRTY;

  return 0;
}

static ctf_id_t
ctf_add_generic (ctf_file_t *fp, uint32_t flag, const char *name,
		 ctf_dtdef_t **rp)
{
  ctf_dtdef_t *dtd;
  ctf_id_t type;
  char *s = NULL;

  if (flag != CTF_ADD_NONROOT && flag != CTF_ADD_ROOT)
    return (ctf_set_errno (fp, EINVAL));

  if (!(fp->ctf_flags & LCTF_RDWR))
    return (ctf_set_errno (fp, ECTF_RDONLY));

  if (LCTF_INDEX_TO_TYPE (fp, fp->ctf_dtnextid, 1) > CTF_MAX_TYPE)
    return (ctf_set_errno (fp, ECTF_FULL));

  if (LCTF_INDEX_TO_TYPE (fp, fp->ctf_dtnextid, 1) == CTF_MAX_PTYPE)
    return (ctf_set_errno (fp, ECTF_FULL));

  if ((dtd = ctf_alloc (sizeof (ctf_dtdef_t))) == NULL)
    return (ctf_set_errno (fp, EAGAIN));

  if (name != NULL && (s = ctf_strdup (name)) == NULL)
    {
      ctf_free (dtd, sizeof (ctf_dtdef_t));
      return (ctf_set_errno (fp, EAGAIN));
    }

  type = fp->ctf_dtnextid++;
  type = LCTF_INDEX_TO_TYPE (fp, type, (fp->ctf_flags & LCTF_CHILD));

  memset (dtd, 0, sizeof (ctf_dtdef_t));
  dtd->dtd_name = s;
  dtd->dtd_type = type;

  if (s != NULL)
    fp->ctf_dtvstrlen += strlen (s) + 1;

  ctf_dtd_insert (fp, dtd);
  fp->ctf_flags |= LCTF_DIRTY;

  *rp = dtd;
  return type;
}

/* When encoding integer sizes, we want to convert a byte count in the range
   1-8 to the closest power of 2 (e.g. 3->4, 5->8, etc).  The clp2() function
   is a clever implementation from "Hacker's Delight" by Henry Warren, Jr.  */
static size_t
clp2 (size_t x)
{
  x--;

  x |= (x >> 1);
  x |= (x >> 2);
  x |= (x >> 4);
  x |= (x >> 8);
  x |= (x >> 16);

  return (x + 1);
}

static ctf_id_t
ctf_add_encoded (ctf_file_t *fp, uint32_t flag,
		 const char *name, const ctf_encoding_t *ep, uint32_t kind)
{
  ctf_dtdef_t *dtd;
  ctf_id_t type;

  if (ep == NULL)
    return (ctf_set_errno (fp, EINVAL));

  if ((type = ctf_add_generic (fp, flag, name, &dtd)) == CTF_ERR)
    return CTF_ERR;		/* errno is set for us.  */

  dtd->dtd_data.ctt_info = CTF_TYPE_INFO (kind, flag, 0);
  dtd->dtd_data.ctt_size = clp2 (P2ROUNDUP (ep->cte_bits, NBBY) / NBBY);
  dtd->dtd_u.dtu_enc = *ep;

  return type;
}

static ctf_id_t
ctf_add_reftype (ctf_file_t *fp, uint32_t flag, ctf_id_t ref, uint32_t kind)
{
  ctf_dtdef_t *dtd;
  ctf_id_t type;

  if (ref == CTF_ERR || ref < 0 || ref > CTF_MAX_TYPE)
    return (ctf_set_errno (fp, EINVAL));

  if ((type = ctf_add_generic (fp, flag, NULL, &dtd)) == CTF_ERR)
    return CTF_ERR;		/* errno is set for us.  */

  dtd->dtd_data.ctt_info = CTF_TYPE_INFO (kind, flag, 0);
  dtd->dtd_data.ctt_type = (uint32_t) ref;

  return type;
}

ctf_id_t
ctf_add_integer (ctf_file_t *fp, uint32_t flag,
		 const char *name, const ctf_encoding_t *ep)
{
  return (ctf_add_encoded (fp, flag, name, ep, CTF_K_INTEGER));
}

ctf_id_t
ctf_add_float (ctf_file_t *fp, uint32_t flag,
	       const char *name, const ctf_encoding_t *ep)
{
  return (ctf_add_encoded (fp, flag, name, ep, CTF_K_FLOAT));
}

ctf_id_t
ctf_add_pointer (ctf_file_t *fp, uint32_t flag, ctf_id_t ref)
{
  return (ctf_add_reftype (fp, flag, ref, CTF_K_POINTER));
}

ctf_id_t
ctf_add_array (ctf_file_t *fp, uint32_t flag, const ctf_arinfo_t *arp)
{
  ctf_dtdef_t *dtd;
  ctf_id_t type;

  if (arp == NULL)
    return (ctf_set_errno (fp, EINVAL));

  if ((type = ctf_add_generic (fp, flag, NULL, &dtd)) == CTF_ERR)
    return CTF_ERR;		/* errno is set for us.  */

  dtd->dtd_data.ctt_info = CTF_TYPE_INFO (CTF_K_ARRAY, flag, 0);
  dtd->dtd_data.ctt_size = 0;
  dtd->dtd_u.dtu_arr = *arp;

  return type;
}

int
ctf_set_array (ctf_file_t *fp, ctf_id_t type, const ctf_arinfo_t *arp)
{
  ctf_dtdef_t *dtd = ctf_dtd_lookup (fp, type);

  if (!(fp->ctf_flags & LCTF_RDWR))
    return (ctf_set_errno (fp, ECTF_RDONLY));

  if (dtd == NULL
      || LCTF_INFO_KIND (fp, dtd->dtd_data.ctt_info) != CTF_K_ARRAY)
    return (ctf_set_errno (fp, ECTF_BADID));

  fp->ctf_flags |= LCTF_DIRTY;
  dtd->dtd_u.dtu_arr = *arp;

  return 0;
}

ctf_id_t
ctf_add_function (ctf_file_t *fp, uint32_t flag,
		  const ctf_funcinfo_t *ctc, const ctf_id_t *argv)
{
  ctf_dtdef_t *dtd;
  ctf_id_t type;
  uint32_t vlen;
  ctf_id_t *vdat = NULL;

  if (ctc == NULL || (ctc->ctc_flags & ~CTF_FUNC_VARARG) != 0
      || (ctc->ctc_argc != 0 && argv == NULL))
    return (ctf_set_errno (fp, EINVAL));

  vlen = ctc->ctc_argc;
  if (ctc->ctc_flags & CTF_FUNC_VARARG)
    vlen++;	       /* Add trailing zero to indicate varargs (see below).  */

  if (vlen > CTF_MAX_VLEN)
    return (ctf_set_errno (fp, EOVERFLOW));

  if (vlen != 0 && (vdat = ctf_alloc (sizeof (ctf_id_t) * vlen)) == NULL)
    return (ctf_set_errno (fp, EAGAIN));

  if ((type = ctf_add_generic (fp, flag, NULL, &dtd)) == CTF_ERR)
    {
      ctf_free (vdat, sizeof (ctf_id_t) * vlen);
      return CTF_ERR;		   /* errno is set for us.  */
    }

  dtd->dtd_data.ctt_info = CTF_TYPE_INFO (CTF_K_FUNCTION, flag, vlen);
  dtd->dtd_data.ctt_type = (uint32_t) ctc->ctc_return;

  memcpy (vdat, argv, sizeof (ctf_id_t) * ctc->ctc_argc);
  if (ctc->ctc_flags & CTF_FUNC_VARARG)
    vdat[vlen - 1] = 0;		   /* Add trailing zero to indicate varargs.  */
  dtd->dtd_u.dtu_argv = vdat;

  return type;
}

ctf_id_t
ctf_add_struct_sized (ctf_file_t *fp, uint32_t flag, const char *name,
		      size_t size)
{
  ctf_hash_t *hp = &fp->ctf_structs;
  ctf_helem_t *hep = NULL;
  ctf_dtdef_t *dtd;
  ctf_id_t type;

  if (name != NULL)
    hep = ctf_hash_lookup (hp, fp, name, strlen (name));

  if (hep != NULL && ctf_type_kind (fp, hep->h_type) == CTF_K_FORWARD)
    dtd = ctf_dtd_lookup (fp, type = hep->h_type);
  else if ((type = ctf_add_generic (fp, flag, name, &dtd)) == CTF_ERR)
    return CTF_ERR;		/* errno is set for us.  */

  dtd->dtd_data.ctt_info = CTF_TYPE_INFO (CTF_K_STRUCT, flag, 0);

  if (size > CTF_MAX_SIZE)
    {
      dtd->dtd_data.ctt_size = CTF_LSIZE_SENT;
      dtd->dtd_data.ctt_lsizehi = CTF_SIZE_TO_LSIZE_HI (size);
      dtd->dtd_data.ctt_lsizelo = CTF_SIZE_TO_LSIZE_LO (size);
    }
  else
    dtd->dtd_data.ctt_size = (uint32_t) size;

  return type;
}

ctf_id_t
ctf_add_struct (ctf_file_t *fp, uint32_t flag, const char *name)
{
  return (ctf_add_struct_sized (fp, flag, name, 0));
}

ctf_id_t
ctf_add_union_sized (ctf_file_t *fp, uint32_t flag, const char *name,
		     size_t size)
{
  ctf_hash_t *hp = &fp->ctf_unions;
  ctf_helem_t *hep = NULL;
  ctf_dtdef_t *dtd;
  ctf_id_t type;

  if (name != NULL)
    hep = ctf_hash_lookup (hp, fp, name, strlen (name));

  if (hep != NULL && ctf_type_kind (fp, hep->h_type) == CTF_K_FORWARD)
    dtd = ctf_dtd_lookup (fp, type = hep->h_type);
  else if ((type = ctf_add_generic (fp, flag, name, &dtd)) == CTF_ERR)
    return CTF_ERR;		/* errno is set for us */

  dtd->dtd_data.ctt_info = CTF_TYPE_INFO (CTF_K_UNION, flag, 0);

  if (size > CTF_MAX_SIZE)
    {
      dtd->dtd_data.ctt_size = CTF_LSIZE_SENT;
      dtd->dtd_data.ctt_lsizehi = CTF_SIZE_TO_LSIZE_HI (size);
      dtd->dtd_data.ctt_lsizelo = CTF_SIZE_TO_LSIZE_LO (size);
    }
  else
    dtd->dtd_data.ctt_size = (uint32_t) size;

  return type;
}

ctf_id_t
ctf_add_union (ctf_file_t *fp, uint32_t flag, const char *name)
{
  return (ctf_add_union_sized (fp, flag, name, 0));
}

ctf_id_t
ctf_add_enum (ctf_file_t *fp, uint32_t flag, const char *name)
{
  ctf_hash_t *hp = &fp->ctf_enums;
  ctf_helem_t *hep = NULL;
  ctf_dtdef_t *dtd;
  ctf_id_t type;

  if (name != NULL)
    hep = ctf_hash_lookup (hp, fp, name, strlen (name));

  if (hep != NULL && ctf_type_kind (fp, hep->h_type) == CTF_K_FORWARD)
    dtd = ctf_dtd_lookup (fp, type = hep->h_type);
  else if ((type = ctf_add_generic (fp, flag, name, &dtd)) == CTF_ERR)
    return CTF_ERR;		/* errno is set for us.  */

  dtd->dtd_data.ctt_info = CTF_TYPE_INFO (CTF_K_ENUM, flag, 0);
  dtd->dtd_data.ctt_size = fp->ctf_dmodel->ctd_int;

  return type;
}

ctf_id_t
ctf_add_forward (ctf_file_t *fp, uint32_t flag, const char *name,
		 uint32_t kind)
{
  ctf_hash_t *hp;
  ctf_helem_t *hep;
  ctf_dtdef_t *dtd;
  ctf_id_t type;

  switch (kind)
    {
    case CTF_K_STRUCT:
      hp = &fp->ctf_structs;
      break;
    case CTF_K_UNION:
      hp = &fp->ctf_unions;
      break;
    case CTF_K_ENUM:
      hp = &fp->ctf_enums;
      break;
    default:
      return (ctf_set_errno (fp, ECTF_NOTSUE));
    }

  /* If the type is already defined or exists as a forward tag, just
     return the ctf_id_t of the existing definition.  */
  if (name != NULL && (hep = ctf_hash_lookup (hp, fp, name,
					      strlen (name))) != NULL)
    return hep->h_type;

  if ((type = ctf_add_generic (fp, flag, name, &dtd)) == CTF_ERR)
    return CTF_ERR;		/* errno is set for us.  */

  dtd->dtd_data.ctt_info = CTF_TYPE_INFO (CTF_K_FORWARD, flag, 0);
  dtd->dtd_data.ctt_type = kind;

  return type;
}

ctf_id_t
ctf_add_typedef (ctf_file_t *fp, uint32_t flag, const char *name,
		 ctf_id_t ref)
{
  ctf_dtdef_t *dtd;
  ctf_id_t type;

  if (ref == CTF_ERR || ref < 0 || ref > CTF_MAX_TYPE)
    return (ctf_set_errno (fp, EINVAL));

  if ((type = ctf_add_generic (fp, flag, name, &dtd)) == CTF_ERR)
    return CTF_ERR;		/* errno is set for us.  */

  dtd->dtd_data.ctt_info = CTF_TYPE_INFO (CTF_K_TYPEDEF, flag, 0);
  dtd->dtd_data.ctt_type = (uint32_t) ref;

  return type;
}

ctf_id_t
ctf_add_volatile (ctf_file_t *fp, uint32_t flag, ctf_id_t ref)
{
  return (ctf_add_reftype (fp, flag, ref, CTF_K_VOLATILE));
}

ctf_id_t
ctf_add_const (ctf_file_t *fp, uint32_t flag, ctf_id_t ref)
{
  return (ctf_add_reftype (fp, flag, ref, CTF_K_CONST));
}

ctf_id_t
ctf_add_restrict (ctf_file_t *fp, uint32_t flag, ctf_id_t ref)
{
  return (ctf_add_reftype (fp, flag, ref, CTF_K_RESTRICT));
}

int
ctf_add_enumerator (ctf_file_t *fp, ctf_id_t enid, const char *name,
		    int value)
{
  ctf_dtdef_t *dtd = ctf_dtd_lookup (fp, enid);
  ctf_dmdef_t *dmd;

  uint32_t kind, vlen, root;
  char *s;

  if (name == NULL)
    return (ctf_set_errno (fp, EINVAL));

  if (!(fp->ctf_flags & LCTF_RDWR))
    return (ctf_set_errno (fp, ECTF_RDONLY));

  if (dtd == NULL)
    return (ctf_set_errno (fp, ECTF_BADID));

  kind = LCTF_INFO_KIND (fp, dtd->dtd_data.ctt_info);
  root = LCTF_INFO_ISROOT (fp, dtd->dtd_data.ctt_info);
  vlen = LCTF_INFO_VLEN (fp, dtd->dtd_data.ctt_info);

  if (kind != CTF_K_ENUM)
    return (ctf_set_errno (fp, ECTF_NOTENUM));

  if (vlen == CTF_MAX_VLEN)
    return (ctf_set_errno (fp, ECTF_DTFULL));

  for (dmd = ctf_list_next (&dtd->dtd_u.dtu_members);
       dmd != NULL; dmd = ctf_list_next (dmd))
    {
      if (strcmp (dmd->dmd_name, name) == 0)
	return (ctf_set_errno (fp, ECTF_DUPLICATE));
    }

  if ((dmd = ctf_alloc (sizeof (ctf_dmdef_t))) == NULL)
    return (ctf_set_errno (fp, EAGAIN));

  if ((s = ctf_strdup (name)) == NULL)
    {
      ctf_free (dmd, sizeof (ctf_dmdef_t));
      return (ctf_set_errno (fp, EAGAIN));
    }

  dmd->dmd_name = s;
  dmd->dmd_type = CTF_ERR;
  dmd->dmd_offset = 0;
  dmd->dmd_value = value;

  dtd->dtd_data.ctt_info = CTF_TYPE_INFO (kind, root, vlen + 1);
  ctf_list_append (&dtd->dtd_u.dtu_members, dmd);

  fp->ctf_dtvstrlen += strlen (s) + 1;
  fp->ctf_flags |= LCTF_DIRTY;

  return 0;
}

int
ctf_add_member_offset (ctf_file_t *fp, ctf_id_t souid, const char *name,
		       ctf_id_t type, unsigned long bit_offset)
{
  ctf_dtdef_t *dtd = ctf_dtd_lookup (fp, souid);
  ctf_dmdef_t *dmd;

  ssize_t msize, malign, ssize;
  uint32_t kind, vlen, root;
  char *s = NULL;

  if (!(fp->ctf_flags & LCTF_RDWR))
    return (ctf_set_errno (fp, ECTF_RDONLY));

  if (dtd == NULL)
    return (ctf_set_errno (fp, ECTF_BADID));

  kind = LCTF_INFO_KIND (fp, dtd->dtd_data.ctt_info);
  root = LCTF_INFO_ISROOT (fp, dtd->dtd_data.ctt_info);
  vlen = LCTF_INFO_VLEN (fp, dtd->dtd_data.ctt_info);

  if (kind != CTF_K_STRUCT && kind != CTF_K_UNION)
    return (ctf_set_errno (fp, ECTF_NOTSOU));

  if (vlen == CTF_MAX_VLEN)
    return (ctf_set_errno (fp, ECTF_DTFULL));

  if (name != NULL)
    {
      for (dmd = ctf_list_next (&dtd->dtd_u.dtu_members);
	   dmd != NULL; dmd = ctf_list_next (dmd))
	{
	  if (dmd->dmd_name != NULL && strcmp (dmd->dmd_name, name) == 0)
	    return (ctf_set_errno (fp, ECTF_DUPLICATE));
	}
    }

  if ((msize = ctf_type_size (fp, type)) == CTF_ERR ||
      (malign = ctf_type_align (fp, type)) == CTF_ERR)
    return CTF_ERR;		/* errno is set for us.  */

  if ((dmd = ctf_alloc (sizeof (ctf_dmdef_t))) == NULL)
    return (ctf_set_errno (fp, EAGAIN));

  if (name != NULL && (s = ctf_strdup (name)) == NULL)
    {
      ctf_free (dmd, sizeof (ctf_dmdef_t));
      return (ctf_set_errno (fp, EAGAIN));
    }

  dmd->dmd_name = s;
  dmd->dmd_type = type;
  dmd->dmd_value = -1;

  if (kind == CTF_K_STRUCT && vlen != 0)
    {
      if (bit_offset == (unsigned long) - 1)
	{
	  /* Natural alignment.  */

	  ctf_dmdef_t *lmd = ctf_list_prev (&dtd->dtd_u.dtu_members);
	  ctf_id_t ltype = ctf_type_resolve (fp, lmd->dmd_type);
	  size_t off = lmd->dmd_offset;

	  ctf_encoding_t linfo;
	  ssize_t lsize;

	  if (ctf_type_encoding (fp, ltype, &linfo) != CTF_ERR)
	    off += linfo.cte_bits;
	  else if ((lsize = ctf_type_size (fp, ltype)) != CTF_ERR)
	    off += lsize * NBBY;

	  /* Round up the offset of the end of the last member to
	     the next byte boundary, convert 'off' to bytes, and
	     then round it up again to the next multiple of the
	     alignment required by the new member.  Finally,
	     convert back to bits and store the result in
	     dmd_offset.  Technically we could do more efficient
	     packing if the new member is a bit-field, but we're
	     the "compiler" and ANSI says we can do as we choose.  */

	  off = roundup (off, NBBY) / NBBY;
	  off = roundup (off, MAX (malign, 1));
	  dmd->dmd_offset = off * NBBY;
	  ssize = off + msize;
	}
      else
	{
	  /* Specified offset in bits.  */

	  dmd->dmd_offset = bit_offset;
	  ssize = ctf_get_ctt_size (fp, &dtd->dtd_data, NULL, NULL);
	  ssize = MAX (ssize, (bit_offset / NBBY) + msize);
	}
    }
  else
    {
      dmd->dmd_offset = 0;
      ssize = ctf_get_ctt_size (fp, &dtd->dtd_data, NULL, NULL);
      ssize = MAX (ssize, msize);
    }

  if (ssize > CTF_MAX_SIZE)
    {
      dtd->dtd_data.ctt_size = CTF_LSIZE_SENT;
      dtd->dtd_data.ctt_lsizehi = CTF_SIZE_TO_LSIZE_HI (ssize);
      dtd->dtd_data.ctt_lsizelo = CTF_SIZE_TO_LSIZE_LO (ssize);
    }
  else
    dtd->dtd_data.ctt_size = (uint32_t) ssize;

  dtd->dtd_data.ctt_info = CTF_TYPE_INFO (kind, root, vlen + 1);
  ctf_list_append (&dtd->dtd_u.dtu_members, dmd);

  if (s != NULL)
    fp->ctf_dtvstrlen += strlen (s) + 1;

  fp->ctf_flags |= LCTF_DIRTY;
  return 0;
}

int
ctf_add_member (ctf_file_t *fp, ctf_id_t souid, const char *name,
		ctf_id_t type)
{
  return ctf_add_member_offset (fp, souid, name, type, (unsigned long) - 1);
}

int
ctf_add_variable (ctf_file_t *fp, const char *name, ctf_id_t ref)
{
  ctf_dvdef_t *dvd;

  if (!(fp->ctf_flags & LCTF_RDWR))
    return (ctf_set_errno (fp, ECTF_RDONLY));

  if (ctf_dvd_lookup (fp, name) != NULL)
    return (ctf_set_errno (fp, ECTF_DUPLICATE));

  if ((dvd = ctf_alloc (sizeof (ctf_dvdef_t))) == NULL)
    return (ctf_set_errno (fp, EAGAIN));

  if (name != NULL && (dvd->dvd_name = ctf_strdup (name)) == NULL)
    {
      ctf_free (dvd, sizeof (ctf_dvdef_t));
      return (ctf_set_errno (fp, EAGAIN));
    }
  dvd->dvd_type = ref;
  dvd->dvd_snapshots = fp->ctf_snapshots;

  ctf_dvd_insert (fp, dvd);

  fp->ctf_dtvstrlen += strlen (name) + 1;
  fp->ctf_flags |= LCTF_DIRTY;
  return 0;
}

static int
enumcmp (const char *name, int value, void *arg)
{
  ctf_bundle_t *ctb = arg;
  int bvalue;

  if (ctf_enum_value (ctb->ctb_file, ctb->ctb_type, name, &bvalue) == CTF_ERR)
    {
      ctf_dprintf ("Conflict due to member %s iteration error.\n", name);
      return 1;
    }
  if (value != bvalue)
    {
      ctf_dprintf ("Conflict due to value change: %i versus %i\n",
		   value, bvalue);
      return 1;
    }
  return 0;
}

static int
enumadd (const char *name, int value, void *arg)
{
  ctf_bundle_t *ctb = arg;

  return (ctf_add_enumerator (ctb->ctb_file, ctb->ctb_type,
			      name, value) == CTF_ERR);
}

static int
membcmp (const char *name, ctf_id_t type _libctf_unused_, unsigned long offset,
	 void *arg)
{
  ctf_bundle_t *ctb = arg;
  ctf_membinfo_t ctm;

  if (ctf_member_info (ctb->ctb_file, ctb->ctb_type, name, &ctm) == CTF_ERR)
    {
      ctf_dprintf ("Conflict due to member %s iteration error.\n", name);
      return 1;
    }
  if (ctm.ctm_offset != offset)
    {
      ctf_dprintf ("Conflict due to member %s offset change: "
		   "%lx versus %lx\n", name, ctm.ctm_offset, offset);
      return 1;
    }
  return 0;
}

static int
membadd (const char *name, ctf_id_t type, unsigned long offset, void *arg)
{
  ctf_bundle_t *ctb = arg;
  ctf_dmdef_t *dmd;
  char *s = NULL;

  if ((dmd = ctf_alloc (sizeof (ctf_dmdef_t))) == NULL)
    return (ctf_set_errno (ctb->ctb_file, EAGAIN));

  if (name != NULL && (s = ctf_strdup (name)) == NULL)
    {
      ctf_free (dmd, sizeof (ctf_dmdef_t));
      return (ctf_set_errno (ctb->ctb_file, EAGAIN));
    }

  /* For now, dmd_type is copied as the src_fp's type; it is reset to an
    equivalent dst_fp type by a final loop in ctf_add_type(), below.  */
  dmd->dmd_name = s;
  dmd->dmd_type = type;
  dmd->dmd_offset = offset;
  dmd->dmd_value = -1;

  ctf_list_append (&ctb->ctb_dtd->dtd_u.dtu_members, dmd);

  if (s != NULL)
    ctb->ctb_file->ctf_dtvstrlen += strlen (s) + 1;

  ctb->ctb_file->ctf_flags |= LCTF_DIRTY;
  return 0;
}

/* The ctf_add_type routine is used to copy a type from a source CTF container
   to a dynamic destination container.  This routine operates recursively by
   following the source type's links and embedded member types.  If the
   destination container already contains a named type which has the same
   attributes, then we succeed and return this type but no changes occur.  */
ctf_id_t
ctf_add_type (ctf_file_t *dst_fp, ctf_file_t *src_fp, ctf_id_t src_type)
{
  ctf_id_t dst_type = CTF_ERR;
  uint32_t dst_kind = CTF_K_UNKNOWN;

  const char *name;
  uint32_t kind, flag, vlen;

  const ctf_type_t *src_tp, *dst_tp;
  ctf_bundle_t src, dst;
  ctf_encoding_t src_en, dst_en;
  ctf_arinfo_t src_ar, dst_ar;

  ctf_dtdef_t *dtd;
  ctf_funcinfo_t ctc;
  ssize_t size;

  ctf_hash_t *hp;
  ctf_helem_t *hep;

  if (!(dst_fp->ctf_flags & LCTF_RDWR))
    return (ctf_set_errno (dst_fp, ECTF_RDONLY));

  if ((src_tp = ctf_lookup_by_id (&src_fp, src_type)) == NULL)
    return (ctf_set_errno (dst_fp, ctf_errno (src_fp)));

  name = ctf_strptr (src_fp, src_tp->ctt_name);
  kind = LCTF_INFO_KIND (src_fp, src_tp->ctt_info);
  flag = LCTF_INFO_ISROOT (src_fp, src_tp->ctt_info);
  vlen = LCTF_INFO_VLEN (src_fp, src_tp->ctt_info);

  switch (kind)
    {
    case CTF_K_STRUCT:
      hp = &dst_fp->ctf_structs;
      break;
    case CTF_K_UNION:
      hp = &dst_fp->ctf_unions;
      break;
    case CTF_K_ENUM:
      hp = &dst_fp->ctf_enums;
      break;
    default:
      hp = &dst_fp->ctf_names;
      break;
    }

  /* If the source type has a name and is a root type (visible at the
     top-level scope), lookup the name in the destination container and
     verify that it is of the same kind before we do anything else.  */

  if ((flag & CTF_ADD_ROOT) && name[0] != '\0'
      && (hep = ctf_hash_lookup (hp, dst_fp, name, strlen (name))) != NULL)
    {
      dst_type = (ctf_id_t) hep->h_type;
      dst_kind = ctf_type_kind (dst_fp, dst_type);
    }

  /* If an identically named dst_type exists, fail with ECTF_CONFLICT
     unless dst_type is a forward declaration and src_type is a struct,
     union, or enum (i.e. the definition of the previous forward decl).  */

  if (dst_type != CTF_ERR && dst_kind != kind
      && (dst_kind != CTF_K_FORWARD
	  || (kind != CTF_K_ENUM && kind != CTF_K_STRUCT
	      && kind != CTF_K_UNION)))
    {
      ctf_dprintf ("Conflict for type %s: kinds differ, new: %i; "
		   "old (ID %lx): %i\n", name, kind, dst_type, dst_kind);
      return (ctf_set_errno (dst_fp, ECTF_CONFLICT));
    }

  /* We take special action for an integer or a float since it is
     described not only by its name but also its encoding.  For integers,
     bit-fields exploit this degeneracy.  */

  if (kind == CTF_K_INTEGER || kind == CTF_K_FLOAT)
    {
      if (ctf_type_encoding (src_fp, src_type, &src_en) != 0)
	return (ctf_set_errno (dst_fp, ctf_errno (src_fp)));

      if (dst_type != CTF_ERR)
	{
	  ctf_file_t *fp = dst_fp;

	  if ((dst_tp = ctf_lookup_by_id (&fp, dst_type)) == NULL)
	    return CTF_ERR;

	  if (LCTF_INFO_ISROOT (fp, dst_tp->ctt_info) & CTF_ADD_ROOT)
	    {
	      /* The type that we found in the hash is also root-visible.  If
		 the two types match then use the existing one; otherwise,
		 declare a conflict.  */

#ifndef NO_COMPAT
	      /* Do not report a conflict if the source type is a 1- or 4-bit
		 root-visible int: this works around CTF damage in old kernels
		 before UEK4 4.1.12-99.  */
#endif /* !NO_COMPAT */
	      if (ctf_type_encoding (dst_fp, dst_type, &dst_en) != 0)
		return CTF_ERR;			/* errno set for us.  */

	      if (memcmp (&src_en, &dst_en, sizeof (ctf_encoding_t)) == 0)
		return dst_type;
	      else
#ifndef NO_COMPAT
		if (!(strcmp (name, "int") == 0
			 && (flag & CTF_ADD_ROOT)
			 && (CTF_INT_BITS (src_tp->ctt_type) == 4 ||
			     CTF_INT_BITS (src_tp->ctt_type) == 1)))
#endif /* !NO_COMPAT */
		  {
		    return (ctf_set_errno (dst_fp, ECTF_CONFLICT));
		  }
	    }
	  else
	    {
	      /* We found a non-root-visible type in the hash.  We reset
	         dst_type to ensure that we continue to look for a possible
	         conflict in the pending list.  */

	      dst_type = CTF_ERR;
	    }
	}
    }

  /* If the non-empty name was not found in the appropriate hash, search
     the list of pending dynamic definitions that are not yet committed.
     If a matching name and kind are found, assume this is the type that
     we are looking for.  This is necessary to permit ctf_add_type() to
     operate recursively on entities such as a struct that contains a
     pointer member that refers to the same struct type.  */

  if (dst_type == CTF_ERR && name[0] != '\0')
    {
      for (dtd = ctf_list_prev (&dst_fp->ctf_dtdefs); dtd != NULL
	     && LCTF_TYPE_TO_INDEX (src_fp, dtd->dtd_type) > dst_fp->ctf_dtoldid;
	   dtd = ctf_list_prev (dtd))
	{
	  if (LCTF_INFO_KIND (src_fp, dtd->dtd_data.ctt_info) == kind
	      && dtd->dtd_name != NULL && strcmp (dtd->dtd_name, name) == 0)
	    {
	      int sroot;	/* Is the src root-visible?  */
	      int droot;	/* Is the dst root-visible?  */
	      int match;	/* Do the encodings match?  */

	      if (kind != CTF_K_INTEGER && kind != CTF_K_FLOAT)
		return dtd->dtd_type;

	      sroot = (flag & CTF_ADD_ROOT);
	      droot = (LCTF_INFO_ISROOT (dst_fp,
					 dtd->dtd_data.
					 ctt_info) & CTF_ADD_ROOT);

	      match = (memcmp (&src_en, &dtd->dtd_u.dtu_enc,
			       sizeof (ctf_encoding_t)) == 0);

	      /* If the types share the same encoding then return the id of the
		 first unless one type is root-visible and the other is not; in
		 that case the new type must get a new id if a match is never
		 found. */

#ifndef NO_COMPAT
	      /* If there's no match then keep looking unless both types are
		 root-visible, in which case we report a conflict.  Do not
		 report a conflict if the source type is a 1- or 4-bit
		 root-visible int: this works around CTF damage in old kernels <
		 UEK4 4.1.12-99.  */
#endif /* !NO_COMPAT */
	      if (match && sroot == droot)
		return dtd->dtd_type;
	      else if (!match && sroot && droot)
#ifndef NO_COMPAT
		if (!(strcmp (name, "int") == 0 && sroot
		      && (CTF_INT_BITS (src_tp->ctt_type) == 4 ||
			  CTF_INT_BITS (src_tp->ctt_type) == 1)))
#endif /* !NO_COMPAT */
		{
		  return (ctf_set_errno (dst_fp, ECTF_CONFLICT));
		}
	    }
	}
    }

  src.ctb_file = src_fp;
  src.ctb_type = src_type;
  src.ctb_dtd = NULL;

  dst.ctb_file = dst_fp;
  dst.ctb_type = dst_type;
  dst.ctb_dtd = NULL;

  /* Now perform kind-specific processing.  If dst_type is CTF_ERR, then
     we add a new type with the same properties as src_type to dst_fp.
     If dst_type is not CTF_ERR, then we verify that dst_type has the
     same attributes as src_type.  We recurse for embedded references.  */
  switch (kind)
    {
    case CTF_K_INTEGER:
      /*  If we found a match we will have either returned it or declared a
	  conflict.  */
      dst_type = ctf_add_integer (dst_fp, flag, name, &src_en);
      break;

    case CTF_K_FLOAT:
      /* If we found a match we will have either returned it or declared a
       conflict.  */
      dst_type = ctf_add_float (dst_fp, flag, name, &src_en);
      break;

    case CTF_K_POINTER:
    case CTF_K_VOLATILE:
    case CTF_K_CONST:
    case CTF_K_RESTRICT:
      src_type = ctf_type_reference (src_fp, src_type);
      src_type = ctf_add_type (dst_fp, src_fp, src_type);

      if (src_type == CTF_ERR)
	return CTF_ERR;				/* errno is set for us.  */

      dst_type = ctf_add_reftype (dst_fp, flag, src_type, kind);
      break;

    case CTF_K_ARRAY:
      if (ctf_array_info (src_fp, src_type, &src_ar) == CTF_ERR)
	return (ctf_set_errno (dst_fp, ctf_errno (src_fp)));

      src_ar.ctr_contents =
	ctf_add_type (dst_fp, src_fp, src_ar.ctr_contents);
      src_ar.ctr_index = ctf_add_type (dst_fp, src_fp, src_ar.ctr_index);
      src_ar.ctr_nelems = src_ar.ctr_nelems;

      if (src_ar.ctr_contents == CTF_ERR || src_ar.ctr_index == CTF_ERR)
	return CTF_ERR;				/* errno is set for us.  */

      if (dst_type != CTF_ERR)
	{
	  if (ctf_array_info (dst_fp, dst_type, &dst_ar) != 0)
	    return CTF_ERR;			/* errno is set for us.  */

	  if (memcmp (&src_ar, &dst_ar, sizeof (ctf_arinfo_t)))
	    {
	      ctf_dprintf ("Conflict for type %s against ID %lx: "
			   "array info differs, old %lx/%lx/%x; "
			   "new: %lx/%lx/%x\n", name, dst_type,
			   src_ar.ctr_contents, src_ar.ctr_index,
			   src_ar.ctr_nelems, dst_ar.ctr_contents,
			   dst_ar.ctr_index, dst_ar.ctr_nelems);
	      return (ctf_set_errno (dst_fp, ECTF_CONFLICT));
	    }
	}
      else
	dst_type = ctf_add_array (dst_fp, flag, &src_ar);
      break;

    case CTF_K_FUNCTION:
      ctc.ctc_return = ctf_add_type (dst_fp, src_fp, src_tp->ctt_type);
      ctc.ctc_argc = 0;
      ctc.ctc_flags = 0;

      if (ctc.ctc_return == CTF_ERR)
	return CTF_ERR;				/* errno is set for us.  */

      dst_type = ctf_add_function (dst_fp, flag, &ctc, NULL);
      break;

    case CTF_K_STRUCT:
    case CTF_K_UNION:
      {
	ctf_dmdef_t *dmd;
	int errs = 0;

	/* Technically to match a struct or union we need to check both
	   ways (src members vs. dst, dst members vs. src) but we make
	   this more optimal by only checking src vs. dst and comparing
	   the total size of the structure (which we must do anyway)
	   which covers the possibility of dst members not in src.
	   This optimization can be defeated for unions, but is so
	   pathological as to render it irrelevant for our purposes.  */

	if (dst_type != CTF_ERR && dst_kind != CTF_K_FORWARD)
	  {
	    if (ctf_type_size (src_fp, src_type) !=
	 	ctf_type_size (dst_fp, dst_type))
	      {
		ctf_dprintf ("Conflict for type %s against ID %lx: "
			     "union size differs, old %li, new %li\n",
			     name, dst_type, ctf_type_size (src_fp, src_type),
			     ctf_type_size (dst_fp, dst_type));
		return (ctf_set_errno (dst_fp, ECTF_CONFLICT));
	      }

	    if (ctf_member_iter (src_fp, src_type, membcmp, &dst))
	      {
		ctf_dprintf ("Conflict for type %s against ID %lx: "
			     "members differ, see above\n", name, dst_type);
		return (ctf_set_errno (dst_fp, ECTF_CONFLICT));
	      }

	    break;
	  }

	/* Unlike the other cases, copying structs and unions is done
	   manually so as to avoid repeated lookups in ctf_add_member
	   and to ensure the exact same member offsets as in src_type.  */

	dst_type = ctf_add_generic (dst_fp, flag, name, &dtd);
	if (dst_type == CTF_ERR)
	  return CTF_ERR;			/* errno is set for us.  */

	dst.ctb_type = dst_type;
	dst.ctb_dtd = dtd;

	if (ctf_member_iter (src_fp, src_type, membadd, &dst) != 0)
	  errs++;	       /* Increment errs and fail at bottom of case.  */

	if ((size = ctf_type_size (src_fp, src_type)) > CTF_MAX_SIZE)
	  {
	    dtd->dtd_data.ctt_size = CTF_LSIZE_SENT;
	    dtd->dtd_data.ctt_lsizehi = CTF_SIZE_TO_LSIZE_HI (size);
	    dtd->dtd_data.ctt_lsizelo = CTF_SIZE_TO_LSIZE_LO (size);
	  }
	else
	  dtd->dtd_data.ctt_size = (uint32_t) size;

	dtd->dtd_data.ctt_info = CTF_TYPE_INFO (kind, flag, vlen);

	/* Make a final pass through the members changing each dmd_type (a
	   src_fp type) to an equivalent type in dst_fp.  We pass through all
	   members, leaving any that fail set to CTF_ERR.  */
	for (dmd = ctf_list_next (&dtd->dtd_u.dtu_members);
	     dmd != NULL; dmd = ctf_list_next (dmd))
	  {
	    if ((dmd->dmd_type = ctf_add_type (dst_fp, src_fp,
					       dmd->dmd_type)) == CTF_ERR)
	      errs++;
	  }

	if (errs)
	  return CTF_ERR;			/* errno is set for us.  */
	break;
      }

    case CTF_K_ENUM:
      if (dst_type != CTF_ERR && dst_kind != CTF_K_FORWARD)
	{
	  if (ctf_enum_iter (src_fp, src_type, enumcmp, &dst)
	      || ctf_enum_iter (dst_fp, dst_type, enumcmp, &src))
	    {
	      ctf_dprintf ("Conflict for enum %s against ID %lx: "
			   "members differ, see above\n", name, dst_type);
	      return (ctf_set_errno (dst_fp, ECTF_CONFLICT));
	    }
	}
      else
	{
	  dst_type = ctf_add_enum (dst_fp, flag, name);
	  if ((dst.ctb_type = dst_type) == CTF_ERR
	      || ctf_enum_iter (src_fp, src_type, enumadd, &dst))
	    return CTF_ERR;			/* errno is set for us */
	}
      break;

    case CTF_K_FORWARD:
      if (dst_type == CTF_ERR)
	{
	  dst_type = ctf_add_forward (dst_fp, flag,
				      name, CTF_K_STRUCT); /* Assume STRUCT. */
	}
      break;

    case CTF_K_TYPEDEF:
      src_type = ctf_type_reference (src_fp, src_type);
      src_type = ctf_add_type (dst_fp, src_fp, src_type);

      if (src_type == CTF_ERR)
	return CTF_ERR;				/* errno is set for us.  */

      /* If dst_type is not CTF_ERR at this point, we should check if
	 ctf_type_reference(dst_fp, dst_type) != src_type and if so fail with
	 ECTF_CONFLICT.  However, this causes problems with bitness typedefs
	 that vary based on things like if 32-bit then pid_t is int otherwise
	 long.  We therefore omit this check and assume that if the identically
	 named typedef already exists in dst_fp, it is correct or
	 equivalent.  */

      if (dst_type == CTF_ERR)
	{
	  dst_type = ctf_add_typedef (dst_fp, flag, name, src_type);
	}
      break;

    default:
      return (ctf_set_errno (dst_fp, ECTF_CORRUPT));
    }

  return dst_type;
}
