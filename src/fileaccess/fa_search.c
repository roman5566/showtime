/*
 *  Backend using file I/O - Search methods
 *  Copyright (C) 2010 Andreas Öman
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>
#include <sys/types.h>
#include <regex.h>

#include "showtime.h"
#include "backend/backend.h"
#include "fileaccess.h"
#include "fa_probe.h"
#include "fa_search.h"
#include "service.h"

/* FIXME: utf-8 support? */


typedef struct fa_search_s {
  char                 *fas_query;
  backend_search_type_t fas_type;
  prop_t               *fas_nodes;
  prop_sub_t           *fas_sub;
  FILE                 *fas_fp;
  prop_courier_t       *fas_pc;
  int                   fas_run;
} fa_search_t;


static void
fa_search_destroy (fa_search_t *fas)
{
  free(fas->fas_query);

  if (fas->fas_pc)
    prop_courier_destroy(fas->fas_pc);

  if (fas->fas_sub)
    prop_unsubscribe(fas->fas_sub);

  if (fas->fas_nodes)
    prop_ref_dec(fas->fas_nodes);

  /* FIXME: We should wrap our own popen2() to get the pid
   *        of the child process so we can kill it.
   *        popen() simply waits for the process to finish, which
   *        could be quite some time for Mr locate. */
  if (fas->fas_fp)
    pclose(fas->fas_fp);

  free(fas);
}


static void
fa_search_nodesub(void *opaque, prop_event_t event, ...)
{
  fa_search_t *fas = opaque;
  va_list ap;
  event_t *e;

  va_start(ap, event);

  switch(event)
    {
    case PROP_DESTROYED:
      fas->fas_run = 0;
      break;

    case PROP_EXT_EVENT:
      e = va_arg(ap, event_t *);
   
      if(e->e_type_x != EVENT_APPEND_REQUEST)
	break;

      /* FIXME: Support offsets. */
      break;

    default:
      break;
    }

  va_end(ap);
}


/**
 * Replaces regex tokens with '.' in the string.
 * FIXME: Do this properly by escaping instead.
 */
static char *
deregex (char *str)
{
  char *t = str;

  t = str;
  while (*t) {
    switch (*t)
      {
      case '|':
      case '\\':
      case '(':
      case ')':
      case '^':
      case '$':
      case '+':
      case '?':
      case '*':
      case '[':
      case ']':
      case '{':
      case '}':
	*t = '.';
      break;
      }
    t++;
  }
  
  return str;
}


/**
 * Compiles a regexp matching all paths of existing file:// services.
 * E.g.: "^(path1|path2|path3...)".
 */
static int
fa_create_paths_regex (regex_t *preg) {
  service_t *s;
  int len = 0;
  char *str, *t;
  int errcode;

  hts_mutex_lock(&service_mutex);

  /* First calculate the space needed for the regex. */
  LIST_FOREACH(s, &services, s_link)
    if (!strncmp(s->s_url, "file://", strlen("file://")))
      len += strlen(s->s_url + strlen("file://")) + strlen("/|");

  if (len == 0) {
    /* No file:// services found. We should either flunk out
     * and do nothing here, or act as if this was a feature
     * and provide un-filtered 'locate' output. I really dont know
     * whats best. */
    str = strdup(".*");

  } else {
    str = t = malloc(len + strlen("^()") + 1);

    t += sprintf(str, "^(");

    /* Then construct the regex. */
    LIST_FOREACH(s, &services, s_link)
      if (!strncmp(s->s_url, "file://", strlen("file://")))
	t += sprintf(t, "%s%s|",
		     deregex(mystrdupa(s->s_url + strlen("file://"))),
		     (s->s_url[strlen(s->s_url)-1] == '/' ? "" : "/"));

    *(t-1) = ')';
  }

  hts_mutex_unlock(&service_mutex);

  if ((errcode = regcomp(preg, str, REG_EXTENDED|REG_ICASE|REG_NOSUB))) {
    char buf[64];
    regerror(errcode, preg, buf, sizeof(buf));
    TRACE(TRACE_ERROR, "FA", "Search regex compilation of \"%s\" failed: %s",
	  str, buf);
    free(str);
    return -1;
  }

  free(str);
  return 0;
}


/**
 * Consume 'locate' (updatedb) output results and feed into search results.
 */
static void
fa_locate_searcher (fa_search_t *fas)
{
  char buf[PATH_MAX];
  regex_t preg;

  if (fa_create_paths_regex(&preg) == -1)
    return fa_search_destroy(fas);

  /* Consume 'locate' results. */
  while (1) {
    char url[PATH_MAX+strlen("file://")];
    prop_t *p, *metadata;
    const char *type;
    struct stat st;
    int ctype;

    prop_courier_poll(fas->fas_pc);

    if (!fas->fas_run)
      break;

    if (!fgets(buf, sizeof(buf), fas->fas_fp))
      break;

    if (!*buf || *buf == '\n')
      continue;

    buf[strlen(buf)-1] = '\0';

    /* Ignore dot-files/dirs. */
    if (strstr(buf, "/."))
      continue;

    if (regexec(&preg, buf, 0, NULL, 0)) {
      TRACE(TRACE_DEBUG, "FA", "Searcher: %s: \"%s\" not matching regex: SKIP",
	    fas->fas_query, buf);
      continue;
    }

    /* Probe media type.
     *
     * FIXME: We might want to hide matching files under a matching directory,
     *        or the other way around.
     *        E..g:
     *           Metallica/
     *                     01-Metallica-Song1.mp3
     *                     02-Metallica-Song1.mp3
     *
     *        Should either hide Metallica/ or 01-Metallica-Song1..2..
     *        But that would require the 'locate' output to be sorted, is it?
     *        Its also problematic where a sub set of the tracks matches
     *        the directory name. Then what should we show?.
     *
     *        There is also the problem with:
     *        Scrubs S01E01/
     *                 Scrubs_s01e01.avi
     *                 Sample/
     *                    Scrubs_s01e01_sample.avi
     *        Which will show as four separate entries, less than optimal.
     *
     *        For now we provide all matches, directories and files,
     *        matching on the entire path (not just the basename).
     */

    snprintf(url, sizeof(url), "file://%s", buf);

    if (fa_stat(url, &st, NULL, 0))
      continue;

    metadata = prop_create(NULL, "metadata");

    if (S_ISDIR(st.st_mode)) {
      ctype = CONTENT_DIR;
      prop_set_string(prop_create(metadata, "title"), basename(buf));
    } else
      ctype = fa_probe(metadata, url, NULL, 0, NULL, 0, NULL);

    if (ctype == CONTENT_UNKNOWN)
      continue;

    /* Filter out based on search type. */
    switch (fas->fas_type)
      {
	/* FIXME: We dont want to include matching dirs whose content
	 *        doesn't match the fas_type. */
      case BACKEND_SEARCH_AUDIO:
	if (ctype != CONTENT_AUDIO &&
	    ctype != CONTENT_DIR)
	  continue; /* Skip */
	break;
	
      case BACKEND_SEARCH_VIDEO:
	if (ctype != CONTENT_VIDEO &&
	    ctype != CONTENT_DVD &&
	    ctype != CONTENT_DIR)
	  continue; /* Skip */
	break;

      case BACKEND_SEARCH_ALL:
	break;
      }

    if ((type = content2type(ctype)) == NULL)
      continue; /* Unlikely.. */


    p = prop_create(NULL, NULL);

    if (prop_set_parent(metadata, p))
      prop_destroy(metadata);

    prop_set_string(prop_create(p, "url"), url);
    prop_set_string(prop_create(p, "type"), type);

    if (prop_set_parent(p, fas->fas_nodes))
      prop_destroy(p);
  }


  TRACE(TRACE_DEBUG, "FA", "Searcher: %s: Done", fas->fas_query);
  fa_search_destroy(fas);

  regfree(&preg);
}


static void *
fa_searcher (void *aux)
{
  fa_search_t *fas = aux;
  char cmd[PATH_MAX];

  /* FIXME: We should have some sort of priority here. E.g.:
   *         1) User defined search command.
   *         2) Some omnipresent indexer (beagle?)
   *         3) locate/updatedb
   *         4) standard find.
   */
  snprintf(cmd, sizeof(cmd),
	   "locate -i -L -q -b '%s'", fas->fas_query);

  TRACE(TRACE_DEBUG, "FA", "Searcher: %s: executing \"%s\"",
	fas->fas_query, cmd);

  if ((fas->fas_fp = popen(cmd, "re")) == NULL) {
    TRACE(TRACE_ERROR, "FA", "Searcher: %s: Unable to execute \"%s\": %s",
	  fas->fas_query, cmd, strerror(errno));
    fa_search_destroy(fas);
    return NULL;
  }

  fas->fas_pc = prop_courier_create_passive();
  fas->fas_sub = 
    prop_subscribe(PROP_SUB_TRACK_DESTROY,
		   PROP_TAG_CALLBACK, fa_search_nodesub, fas,
		   PROP_TAG_ROOT, fas->fas_nodes,
		   PROP_TAG_COURIER, fas->fas_pc,
		   NULL);

  fa_locate_searcher(fas);

  return NULL;
}


void
fa_search_start (prop_t *source, const char *query, backend_search_type_t type)
{
  fa_search_t *fas = calloc(1, sizeof(*fas));
  char *s;


  /* Convery query to lower-case to provide case-insensitive search. */
  fas->fas_query = s = strdup(query);
  do {
    *s = tolower(*s);
  } while (*++s);

  fas->fas_run = 1;
  fas->fas_type = type;
  fas->fas_nodes = prop_create(source, "nodes");
  prop_ref_inc(fas->fas_nodes);

  hts_thread_create_detached("fa search", fa_searcher, fas);
}
