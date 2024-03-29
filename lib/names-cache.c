/*
 *	The PCI Library -- ID to Name Cache
 *
 *	Copyright (c) 2008--2009 Martin Mares <mj@ucw.cz>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL v2+.
 *
 *	SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "internal.h"
#include "names.h"

#ifdef PCI_USE_DNS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pwd.h>
#include <unistd.h>

static const char cache_version[] = "#PCI-CACHE-1.0";

static char *get_cache_name(struct pci_access *a)
{
  if (!a->id_cache_name)
    {
      char *name = pci_get_param(a, "net.cache_name");
      if (!name || !name[0])
	return NULL;

      if (strncmp(name, "~/", 2))
	a->id_cache_name = pci_strdup(a, name);
      else
	{
	  uid_t uid = getuid();
	  struct passwd *pw = getpwuid(uid);
	  if (!pw)
	    return name;

	  a->id_cache_name = pci_malloc(a, strlen(pw->pw_dir) + strlen(name+1) + 1);
	  sprintf(a->id_cache_name, "%s%s", pw->pw_dir, name+1);
	}
    }

  return a->id_cache_name;
}

static void create_parent_dirs(struct pci_access *a, char *name)
{
  // Assumes that we have a private copy of the name we can modify

  char *p = name + strlen(name);
  while (p > name && *p != '/')
    p--;
  if (p == name)
    return;

  while (p > name)
    {
      // We stand at a slash. Check if the current prefix exists.
      *p = 0;
      struct stat st;
      int res = stat(name, &st);
      *p = '/';
      if (res >= 0)
	break;

      // Does not exist yet, move up one directory
      p--;
      while (p > name && *p != '/')
	p--;
    }

  // We now stand at the end of the longest existing prefix.
  // Create all directories to the right of it.
  for (;;)
    {
      p++;
      while (*p && *p != '/')
	p++;
      if (!*p)
	break;

      *p = 0;
      int res = mkdir(name, 0777);
      if (res < 0)
	{
	  a->warning("Cannot create directory %s: %s", name, strerror(errno));
	  *p = '/';
	  break;
	}
      *p = '/';
    }
}

int
pci_id_cache_load(struct pci_access *a, int flags)
{
  char *name;
  char line[MAX_LINE];
  FILE *f;
  int lino;

  if (a->id_cache_status > 0)
    return 0;
  a->id_cache_status = 1;

  name = get_cache_name(a);
  if (!name)
    return 0;
  a->debug("Using cache %s\n", name);

  if (flags & PCI_LOOKUP_REFRESH_CACHE)
    {
      a->debug("Not loading cache, will refresh everything\n");
      a->id_cache_status = 2;
      return 0;
    }

  f = fopen(name, "rb");
  if (!f)
    {
      a->debug("Cache file does not exist\n");
      return 0;
    }
  /* FIXME: Compare timestamp with the pci.ids file? */

  lino = 0;
  while (fgets(line, sizeof(line), f))
    {
      char *p = strchr(line, '\n');
      lino++;
      if (p)
        {
	  *p = 0;
	  if (lino == 1)
	    {
	      if (strcmp(line, cache_version))
	        {
		  a->debug("Unrecognized cache version %s, ignoring\n", line);
		  break;
		}
	      continue;
	    }
	  else
	    {
	      int cat, id1, id2, id3, id4, cnt;
	      if (sscanf(line, "%d%x%x%x%x%n", &cat, &id1, &id2, &id3, &id4, &cnt) >= 5)
	        {
		  p = line + cnt;
		  while (*p && *p == ' ')
		    p++;
		  pci_id_insert(a, cat, id1, id2, id3, id4, p, SRC_CACHE);
		  continue;
		}
	    }
	}
      a->warning("Malformed cache file %s (line %d), ignoring", name, lino);
      break;
    }

  if (ferror(f))
    a->warning("Error while reading %s", name);
  fclose(f);
  return 1;
}

void
pci_id_cache_flush(struct pci_access *a)
{
  int orig_status = a->id_cache_status;
  FILE *f;
  unsigned int h;
  struct id_entry *e, *e2;
  char hostname[256], *tmpname, *name;
  int this_pid;

  a->id_cache_status = 0;
  if (orig_status < 2)
    return;
  name = get_cache_name(a);
  if (!name)
    return;

  create_parent_dirs(a, name);

  this_pid = getpid();
  if (gethostname(hostname, sizeof(hostname)) < 0)
    hostname[0] = 0;
  else
    hostname[sizeof(hostname)-1] = 0;
  tmpname = pci_malloc(a, strlen(name) + strlen(hostname) + 64);
  sprintf(tmpname, "%s.tmp-%s-%d", name, hostname, this_pid);

  f = fopen(tmpname, "wb");
  if (!f)
    {
      a->warning("Cannot write to %s: %s", name, strerror(errno));
      pci_mfree(tmpname);
      return;
    }
  a->debug("Writing cache to %s\n", name);
  fprintf(f, "%s\n", cache_version);

  for (h=0; h<HASH_SIZE; h++)
    for (e=a->id_hash[h]; e; e=e->next)
      if (e->src == SRC_CACHE || e->src == SRC_NET)
	{
	  /* Negative entries are not written */
	  if (!e->name[0])
	    continue;

	  /* Verify that every entry is written at most once */
	  for (e2=a->id_hash[h]; e2 != e; e2=e2->next)
	    if ((e2->src == SRC_CACHE || e2->src == SRC_NET) &&
	        e2->cat == e->cat &&
		e2->id12 == e->id12 && e2->id34 == e->id34)
	    break;
	  if (e2 == e)
	    fprintf(f, "%d %x %x %x %x %s\n",
	            e->cat,
		    pair_first(e->id12), pair_second(e->id12),
		    pair_first(e->id34), pair_second(e->id34),
		    e->name);
	}

  fflush(f);
  if (ferror(f))
    a->warning("Error writing %s", name);
  fclose(f);

  if (rename(tmpname, name) < 0)
    {
      a->warning("Cannot rename %s to %s: %s", tmpname, name, strerror(errno));
      unlink(tmpname);
    }
  pci_mfree(tmpname);
}

#else

int pci_id_cache_load(struct pci_access *a UNUSED, int flags UNUSED)
{
  a->id_cache_status = 1;
  return 0;
}

void pci_id_cache_flush(struct pci_access *a)
{
  a->id_cache_status = 0;
  pci_mfree(a->id_cache_name);
  a->id_cache_name = NULL;
}

#endif

void
pci_id_cache_dirty(struct pci_access *a)
{
  if (a->id_cache_status >= 1)
    a->id_cache_status = 2;
}
