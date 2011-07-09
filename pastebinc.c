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

struct pastebinc_config {
};

struct mem_struct {
  char *memory;
  size_t size;
};

struct paste_info {
  char tmpname[TMPNAMELEN];
  FILE *content;
  int fd;
};

int main(int argc, char *argv[]) {
  struct paste_info pi;
  struct pastebinc_config config;
  int retval = 0;

  get_configuration(&config, argc, argv);

  if (write_input_to_paste_info(&pi) == -1) {
    retval = 1;
  }

  if(retval == 0) {
    retval = print_paste(&pi);
  }

  pastebin_post(&config, &pi);

  // TODO: do I need to free the memory held by pi?  (if so, do it with each return above)
  unlink(pi.tmpname);
  close(pi.fd);
  return 0;
}


size_t write_data(void *buffer, size_t size, size_t nmemb, void *userp) {
  size_t realsize = size * nmemb;
  struct mem_struct *mem = (struct mem_struct *) userp;

  mem->memory = realloc(mem->memory, mem->size + realsize + 1);
  if (mem->memory == NULL) {
    fprintf(stderr, "not enough memory to buffer pastebin response (realloc returned NULL)\n");
    exit(1);
  }

  memcpy(&(mem->memory[mem->size]), buffer, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;

  return realsize;
}

int pastebin_post(struct pastebinc_config *config, struct paste_info *pi) {
  static const char url[] = "http://pastebin.com/api_public.php";
  static const char content_fieldname[] = "paste_code";

  struct mem_struct chunk;
  chunk.memory = malloc(1);
  chunk.size = 0;

  CURL *curl;
  CURLcode res;
  struct curl_httppost *post = NULL;
  struct curl_httppost *last = NULL;

  curl_global_init(CURL_GLOBAL_ALL);

  curl_formadd(&post, &last, CURLFORM_COPYNAME, "paste_name", CURLFORM_COPYCONTENTS, "pastebinc test", CURLFORM_END);
  curl_formadd(&post, &last, CURLFORM_COPYNAME, content_fieldname, CURLFORM_FILECONTENT, pi->tmpname, CURLFORM_END);

  curl = curl_easy_init();
  if (curl) {
    fprintf(stderr, "pasting to: %s\n", url);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPPOST, post);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_data);

    res = curl_easy_perform(curl);

    curl_easy_cleanup(curl);
    curl_formfree(post);

    fprintf(stderr, "Response: %s\n", chunk.memory);
  } else {
    fprintf(stderr, "Error initializing curl: %s\n", strerror(errno));
    return 1;
  }

  if(chunk.memory)
    free(chunk.memory);

  curl_global_cleanup();

  return 0;
}

int print_paste(struct paste_info *pi) {
#ifdef PRINTPASTE
  char *buf;
  int readval;

  if ((buf = malloc((u_int)BSIZE)) == NULL) {
    fprintf(stderr, "error allocating %d memory for read buffer: %s\n", ((u_int)BSIZE), strerror(errno));
    return 1;
  }

  rewind(pi->content);
  fprintf(stderr, "Paste will be:\n");

  while ((readval = read(pi->fd, buf, BSIZE)) > 0) {
    fprintf(stderr, "%s", buf);
  }
#endif
}

/*
int get_configuration(void) {
  gchar *site;

  GKeyFile *keyfile;
  GKeyFileFlags flags;
  GError *error = NULL;
  gsize length;

  keyfile = g_key_file_new();
  flags = G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS;

  // Load the GKeyFile from keyfile.conf or return.
  if (!g_key_file_load_from_file(keyfile, "./pastebinc.conf", flags, &error))
  {
    fprintf(stderr, "error reading config: %s\n", error->message);
    g_error_free(error);
    return 1;
  }

  site = g_key_file_get_string(keyfile, "pastebin", "basename", NULL);
  fprintf(stderr, "Pasting to site: %s\n", site);
  // for full example see: http://www.gtkbook.com/tutorial.php?page=keyfile
  return 0;
}
*/

int get_configuration(struct pastebinc_config *config, int argc, char *argv[]) {
  int aflag = 0;
  int bflag = 0;
  char *cvalue = NULL;
  int index;
  int c;

  opterr = 0;

  while ((c = getopt(argc, argv, "abc:")) != -1) {
    switch (c) {
      case 'a':
        aflag = 1;
        break;
      case 'b':
        bflag = 1;
        break;
      case 'c':
        cvalue = optarg;
        break;
      case '?':
        if (optopt == 'c')
    fprintf (stderr, "Option -%c requires an argument.\n", optopt);
        else if (isprint (optopt))
    fprintf (stderr, "Unknown option `-%c'.\n", optopt);
        else
    fprintf (stderr, "Unknown option character `\\x%x'.\n", optopt);
        return 1;
      default:
        abort();
    }
  }

  if(aflag)
    printf("yes - aflag\n");

  printf("aflag = %d, bflag = %d, cvalue = %s\n", aflag, bflag, cvalue);

  for (index = optind; index < argc; index++) {
    printf ("Non-option argument %s\n", argv[index]);
  }

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
    return 1;
  }

  fprintf(stderr, "writing to: %s\n", pi->tmpname);

  if ((buf = malloc((u_int)BSIZE)) == NULL) {
    fprintf(stderr, "error allocating %d memory for stdin buffer: %s\n", ((u_int)BSIZE), strerror(errno));
    return 1;
  }

  while ((readval = read(STDIN_FILENO, buf, BSIZE)) > 0)
  {
    //fprintf(stderr, "content: %s\n", buf);
    if (write(pi->fd, buf, readval) == -1) 
    {
      fprintf(stderr, "error writing to file: %s", strerror(errno));
      return 1;
    }
  }

  return 0;
}
