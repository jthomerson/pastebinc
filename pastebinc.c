/*
 * Copyright (C) 2011 Jeremy Thomerson - http://www.jeremythomerson.com
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

#include <glib.h>
#include <curl/curl.h>

#define BSIZE (8 * 1024)
#define TMPNAME "/tmp/pastebinc.XXXXXX"
#define TMPNAMELEN 22

struct paste_info {
  char tmpname[TMPNAMELEN];
  FILE *content;
  int fd;
};

int main(int argc, char *argv[]) {
  struct paste_info pi;
  int retval = 0;
  gchar *site;

  GKeyFile *keyfile;
  GKeyFileFlags flags;
  GError *error = NULL;
  gsize length;

  keyfile = g_key_file_new();
  flags = G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS;

  /* Load the GKeyFile from keyfile.conf or return. */
  if (!g_key_file_load_from_file(keyfile, "./pastebinc.conf", flags, &error))
  {
    fprintf(stderr, "error reading config: %s\n", error->message);
    g_error_free(error);
    retval = 1;
    goto completed;
  }

  site = g_key_file_get_string(keyfile, "pastebin", "basename", NULL);
  fprintf(stderr, "Pasting to site: %s\n", site);
  // for full example see: http://www.gtkbook.com/tutorial.php?page=keyfile

#ifdef PRINTPASTE
  char *buf;
  int readval;
#endif

  if (write_input_to_paste_info(&pi) == -1) {
    retval = 1;
    goto completed;
  }

#ifdef PRINTPASTE
  if ((buf = malloc((u_int)BSIZE)) == NULL) {
    fprintf(stderr, "error allocating %d memory for read buffer: %s\n", ((u_int)BSIZE), strerror(errno));
    retval = 1;
    goto completed;
  }

  rewind(pi.content);
  fprintf(stderr, "Paste will be:\n");

  while ((readval = read(pi.fd, buf, BSIZE)) > 0) {
    fprintf(stderr, "%s", buf);
  }
#endif

completed:
  // TODO: do I need to free the memory held by pi?  (if so, do it with each return above)
  unlink(pi.tmpname);
  close(pi.fd);
  return 0;
}

int write_input_to_paste_info(struct paste_info *pi) {
  int readval;
  char *buf;

  strcpy(pi->tmpname, TMPNAME);

  if ((pi->fd = mkstemp(pi->tmpname)) == -1 || (pi->content = fdopen(pi->fd, "w+")) == NULL) {
    if (pi->fd != -1) {
      unlink(pi->tmpname);
      close(pi->fd);
    }
    fprintf(stderr, "%s: %s\n", pi->tmpname, strerror(errno));
    return -1;
  }

  fprintf(stderr, "writing to: %s\n", pi->tmpname);

  if ((buf = malloc((u_int)BSIZE)) == NULL) {
    fprintf(stderr, "error allocating %d memory for stdin buffer: %s\n", ((u_int)BSIZE), strerror(errno));
    return -1;
  }

  while ((readval = read(STDIN_FILENO, buf, BSIZE)) > 0)
  {
    //fprintf(stderr, "content: %s\n", buf);
    if (write(pi->fd, buf, readval) == -1) 
    {
      fprintf(stderr, "error writing to file: %s", strerror(errno));
      return -1;
    }
  }

  return 0;
}
