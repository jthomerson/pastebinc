/* pastebinc.c:
 *
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

#ifndef PROGNAME
#define PROGNAME "pastebinc"
#endif

#ifndef VERSION
#define VERSION "0.1-BETA"
#endif

#define BSIZE (8 * 1024)
#define TMPNAME "/tmp/pastebinc.XXXXXX"
#define TMPNAMELEN 22

struct pastebinc_config {
  char *name;
  int verbose;
  int tee;
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
  int abort = 0;

  abort = get_configuration(&config, argc, argv);

  if (!abort && isatty(fileno(stdin))) {
    fprintf(stderr, "ERROR: You must pipe data into " PROGNAME "\n");
    display_usage();
    abort = 1;
  }

  if (!abort && write_input_to_paste_info(&config, &pi))
    abort = 1;

  if (!abort)
    abort = pastebin_post(&config, &pi);

  // TODO: do I need to free the memory held by pi? or other variables?
  unlink(pi.tmpname);
  close(pi.fd);
  return abort ? 1 : 0;
}

/*
 * Takes stdin input and writes it to a temporary file that will be used
 * to post to a pastebin site.  Fills paste_info with the appropriate info
 * about what it did (file path and pointer to written file).
 */
int write_input_to_paste_info(struct pastebinc_config *config, struct paste_info *pi) {
  int readval;
  char *buf;

  strcpy(pi->tmpname, TMPNAME);

  if ((pi->fd = mkstemp(pi->tmpname)) == -1 || (pi->content = fdopen(pi->fd, "w+")) == NULL) {
    if (pi->fd != -1) {
      unlink(pi->tmpname);
      close(pi->fd);
    }
    fprintf(stderr, "Error opening tmp file (%s): %s\n", pi->tmpname, strerror(errno));
    return 1;
  }

  if (config->verbose)
    fprintf(stderr, "DEBUG: Writing to tmp file: %s\n", pi->tmpname);

  if ((buf = malloc((u_int)BSIZE)) == NULL) {
    fprintf(stderr, "Error allocating %d memory for stdin read buffer: %s\n", ((u_int)BSIZE), strerror(errno));
    return 1;
  }

  while ((readval = read(STDIN_FILENO, buf, BSIZE)) > 0)
  {
    if (config->tee)
      fprintf(stdout, "%s", buf);

    if (write(pi->fd, buf, readval) == -1)
    {
      fprintf(stderr, "Error writing to tmp file: %s", strerror(errno));
      return 1;
    }
  }

  return 0;
}

/*
 * Callback for curl that uses the mem_struct structure to build the response
 * of the HTTP post into a char array.
 */
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

/*
 * Post the content contained within paste_info to the appropriate site (from config)
 */
int pastebin_post(struct pastebinc_config *config, struct paste_info *pi) {
 // TODO: these need to come from config files:
  static const char url[] = "http://pastebin.com/api_public.php";
  static const char content_fieldname[] = "paste_code";

  struct mem_struct chunk;
  chunk.memory = malloc(1);
  chunk.size = 0;

  CURL *curl;
  CURLcode res;
  struct curl_httppost *post = NULL;
  struct curl_httppost *last = NULL;
  long http_resp_code = 0;
  long curl_code;
  int abort = 0;

  curl_global_init(CURL_GLOBAL_ALL);

  curl_formadd(&post, &last, CURLFORM_COPYNAME, "paste_name", CURLFORM_COPYCONTENTS, config->name, CURLFORM_END);
  curl_formadd(&post, &last, CURLFORM_COPYNAME, content_fieldname, CURLFORM_FILECONTENT, pi->tmpname, CURLFORM_END);

  curl = curl_easy_init();
  if (curl) {
    if (config->verbose)
      fprintf(stderr, "DEBUG: Pasting to: %s\n", url);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPPOST, post);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_data);

    res = curl_easy_perform(curl);

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_resp_code);
    if (http_resp_code != 200 || curl_code == CURLE_ABORTED_BY_CALLBACK) {
      abort = 1; // call failed
      fprintf(stderr, "ERROR: server response was %ld\n", http_resp_code);
      if (config->verbose) {
        fprintf(stderr, "Contents of reponse where: \n%s\n", chunk.memory);
      }
    } else {
      fprintf(stderr, (config->verbose ? "Paste URL: %s\n" : "%s\n"), chunk.memory);
    }

    curl_easy_cleanup(curl);
    curl_formfree(post);
  } else {
    fprintf(stderr, "Error initializing curl: %s\n", strerror(errno));
    return 1;
  }

  if(chunk.memory)
    free(chunk.memory);

  curl_global_cleanup();

  return abort;
}

/*
 * Parses command-line options and configuration files to fully configure the
 * information we need to run the program.
 */
int get_configuration(struct pastebinc_config *config, int argc, char *argv[]) {
  /* TODO: Want to support the following flags:
   -f, --format  paste format (java, bash, etc)

   TODO: right now we're only supporting the short form of each flag
  */
  int c;
  opterr = 0;

  config->tee = 0;
  config->verbose = 0;
  config->name = NULL;

  while ((c = getopt(argc, argv, "htvn:")) != -1) {
    switch (c) {
      case 't':
        config->tee = 1;
        break;
      case 'v':
        config->verbose = 1;
        break;
      case 'n':
        config->name = optarg;
        break;
      case 'h':
        display_usage();
        return 1;
      default:
        fprintf(stderr, "Unknown option: '%c'\n", optopt);
        return 1;
    }
  }

  return 0;
}

/*
 * Print basic usage information for users.
 */
int display_usage(void) {
  fprintf(stderr,
    PROGNAME " " VERSION "\n\n"
   "Pastes whatever is piped in to stdin to pastebin.com or similar site.\n"
   "Options:\n\n"
   "  -t          'tee', or print out all input from stdin to stdout\n"
   "  -v          'verbose', or print out debugging information as I work\n"
   "  -n [value]  the name (or title) of your paste\n"
   "  -h          print this usage message\n"
   );
}
