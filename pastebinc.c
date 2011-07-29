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

#ifndef CONFDIR
#define CONFDIR "./etc"
#endif

#ifndef CONFFILE
#define CONFFILE "pastebinc.conf"
#endif

#ifndef VERSION
#define VERSION "0.9"
#endif

#define BSIZE (8 * 1024)
#define TMPNAME "/tmp/pastebinc.XXXXXX"
#define TMPNAMELEN 22

typedef struct user_field {
  char *name;
  char *value;
  struct user_field *next;
} t_user_field;

typedef struct user_field_option {
  char *name;
  struct user_field_option_value *values;
  struct user_field_option *next;
} t_user_field_option;

typedef struct user_field_option_value {
  char *user_value;
  char *post_value;
  struct user_field_option_value *next;
} t_user_field_option_value;

struct pastebinc_config {
  char *name;
  int verbose;
  int tee;
  int bypass_proxy;
  char *provider;
  t_user_field_option *user_field_options;
  t_user_field *user_fields;
  GKeyFile *keyfile;
};

struct http_response {
  char *body;
  size_t body_size;
  char *location;
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
    display_usage(&config, 0);
    abort = 1;
  }

  if (!abort && write_input_to_paste_info(&config, &pi))
    abort = 1;

  if (!abort)
    abort = pastebin_post(&config, &pi);

  // TODO: do I need to free the memory held by pi? or other variables?
  unlink(pi.tmpname);
  close(pi.fd);

  if (config.keyfile)
    g_key_file_free(config.keyfile);

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
 * Callback for curl that uses the http_response structure to build the response
 * of the HTTP post into a char array.
 */
size_t http_resp_body_data_received(void *buffer, size_t size, size_t nmemb, void *userp) {
  size_t realsize = size * nmemb;
  struct http_response *resp = (struct http_response *) userp;

  resp->body = realloc(resp->body, resp->body_size + realsize + 1);
  if (resp->body == NULL) {
    fprintf(stderr, "not enough memory to buffer pastebin response (realloc returned NULL)\n");
    exit(1);
  }

  memcpy(&(resp->body[resp->body_size]), buffer, realsize);
  resp->body_size += realsize;
  resp->body[resp->body_size] = 0;

  return realsize;
}

/*
 * Callback for curl that uses the http_response structure to save the location header
 * if one is sent.
 */
size_t http_resp_header_received(void *buffer, size_t size, size_t nmemb, void *userp) {
  struct http_response *resp = (struct http_response *) userp;
  int loclen;

  if (memcmp(buffer, "Location:", 9) == 0) {
    loclen = strlen(buffer) - 12; // "Location: " plus the trailing newline and final null
    resp->location = (char *) malloc(loclen + 1);
    memcpy(resp->location, buffer + 10, loclen);
    resp->location[loclen] = 0;
  }

  return size * nmemb;
}

/*
 * Post the content contained within paste_info to the appropriate site (from config)
 */
int pastebin_post(struct pastebinc_config *config, struct paste_info *pi) {
  struct http_response resp;
  char *paste_url = NULL;
  resp.body = malloc(1);
  resp.body_size = 0;
  resp.location = malloc(1);

  CURL *curl;
  CURLcode res;
  struct curl_httppost *post = NULL;
  struct curl_httppost *last = NULL;
  struct curl_slist *headers = NULL;
  long http_resp_code = 0;
  long curl_code;
  int abort = 0;

  char *url = g_key_file_get_string(config->keyfile, "server", "url", NULL);
  char *content_fieldname = g_key_file_get_string(config->keyfile, "fieldnames", "content", NULL);
  char *title_fieldname = g_key_file_get_string(config->keyfile, "fieldnames", "title", NULL);

  // don't want to have the curl default "Expect: 100" header, so we override it:
  headers = curl_slist_append(headers, "Expect:");

  curl_global_init(CURL_GLOBAL_ALL);

  curl_formadd(&post, &last, CURLFORM_COPYNAME, title_fieldname, CURLFORM_COPYCONTENTS, config->name, CURLFORM_END);
  curl_formadd(&post, &last, CURLFORM_COPYNAME, content_fieldname, CURLFORM_FILECONTENT, pi->tmpname, CURLFORM_END);

  // add static fields to request:
  if (g_key_file_has_group(config->keyfile, "static_fields")) {
    gchar** static_fields = NULL;
    gchar** field = NULL;

    static_fields = g_key_file_get_keys(config->keyfile, "static_fields", NULL, NULL);
    field = static_fields;
    while (*field != NULL) {
      gchar* value = g_key_file_get_string(config->keyfile, "static_fields", *field, NULL);
      curl_formadd(&post, &last, CURLFORM_COPYNAME, *field, CURLFORM_COPYCONTENTS, value, CURLFORM_END);
      if (config->verbose)
        fprintf(stderr, "DEBUG: adding static form field: %s = %s\n", *field, value);

      field++;
    }
    g_strfreev(static_fields);
  }

  if (config->user_fields != NULL) {
    t_user_field *uf = config->user_fields;
    while (uf) {
      if (config->verbose)
        fprintf(stderr, "DEBUG: adding user field: %s = %s\n", uf->name, uf->value);
      curl_formadd(&post, &last, CURLFORM_COPYNAME, uf->name, CURLFORM_COPYCONTENTS, uf->value, CURLFORM_END);
      uf = uf->next;
    }
  }

  curl = curl_easy_init();
  if (curl) {
    if (config->verbose)
      fprintf(stderr,
        "DEBUG: Posting to: %s\n"
        "DEBUG: content fieldname: %s\n"
        "DEBUG: title fieldname: %s\n",
        url, content_fieldname, title_fieldname);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_HTTPPOST, post);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&resp);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &http_resp_body_data_received);
    curl_easy_setopt(curl, CURLOPT_WRITEHEADER, (void *)&resp);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &http_resp_header_received);

    if (config->bypass_proxy)
      curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");

    res = curl_easy_perform(curl);

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_resp_code);
    if (http_resp_code == 302) { // this provider uses a redirect to the paste
      paste_url = resp.location;
    } else if (http_resp_code != 200 || curl_code == CURLE_ABORTED_BY_CALLBACK) {
      abort = 1; // call failed
      fprintf(stderr, "ERROR: server response was %ld\n", http_resp_code);
      if (config->verbose) {
        fprintf(stderr, "DEBUG: Contents of response were: \n%s\n", resp.body);
      }
    } else {
      paste_url = resp.body;
    }

    fprintf(stderr, (config->verbose || paste_url == NULL ? "Paste URL: %s\n" : "%s\n"), paste_url);

    if (paste_url == NULL)
      abort = 1; // failed

    curl_easy_cleanup(curl);
    curl_formfree(post);
  } else {
    fprintf(stderr, "Error initializing curl: %s\n", strerror(errno));
    return 1;
  }

  if(resp.body)
    free(resp.body);

  if (resp.location)
    free(resp.location);

  curl_global_cleanup();

  return abort;
}

/*
 * Parses command-line options and configuration files to fully configure the
 * information we need to run the program.
 */
int get_configuration(struct pastebinc_config *config, int argc, char *argv[]) {
  // TODO: support long flags
  int c;
  int abort;
  int show_usage = 0;
  opterr = 0;
  char *expiration = NULL;
  char *format = NULL;

  config->tee = 0;
  config->verbose = 0;
  config->bypass_proxy = 0;
  config->name = NULL;
  config->user_fields = NULL;
  config->provider = NULL;
  config->keyfile = NULL;
  config->user_field_options = NULL;

  t_user_field *last_user_field = config->user_fields;

  while ((c = getopt(argc, argv, "tvn:p:d:x:f:bhH")) != -1) {
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
      case 'p':
        config->provider = optarg;
        break;
      case 'd':
        add_user_field(config, strtok(optarg, "="), strtok(NULL, "="));
        break;
      case 'x':
        expiration = optarg;
        break;
      case 'f':
        format = optarg;
        break;
      case 'b':
        config->bypass_proxy = 1;
        break;
      case 'h':
        show_usage = 1;
        break;
      case 'H':
        show_usage = 2;
        break;
      default:
        fprintf(stderr, "Unknown option: '%c'\n", optopt);
        return 1;
    }
  }

  abort = read_config_files(config);
  if (!abort && config->name == NULL) { // user didn't supply title, use provider's default
    config->name = g_key_file_get_string(config->keyfile, "defaults", "title", NULL);
  }

  if (show_usage) {
    display_usage(config, show_usage == 2);
    abort = 1;
  }

  if (!abort)
    abort = add_config_user_field(config, expiration, "expiration");

  if (!abort)
    abort = add_config_user_field(config, format, "format");

  return abort;
}

int add_user_field(struct pastebinc_config *config, char *name, char *value) {
  t_user_field *uf;
  uf = (t_user_field *) malloc(sizeof(t_user_field));
  uf->name = name;
  uf->value = value;
  uf->next = NULL;
  if (config->user_fields == NULL) {
    config->user_fields = uf;
  } else {
    t_user_field *last_uf = config->user_fields;
    while (last_uf != NULL) {
      if (last_uf->next == NULL) {
        last_uf->next = uf;
        break;
      } else {
        last_uf = last_uf->next;
      }
    }
  }
}

int add_config_user_field(struct pastebinc_config *config, char *value, char *fieldname) {
  if (value == NULL)
    return 0;

  char *post_field_name = g_key_file_get_string(config->keyfile, "standard_field_names", fieldname, NULL);
  add_user_field(config, post_field_name, value);

  t_user_field_option *ufo = config->user_field_options;
  int found = 0;
  while (ufo) {
    if (strcmp(ufo->name, post_field_name) == 0) {
      t_user_field_option_value *ufov = ufo->values;
      while (ufov) {
        if (strcmp(ufov->post_value, value) == 0) {
          found = 1;
          break;
        }
        ufov = ufov->next;
      }
    }
    if (found)
      break;
    ufo = ufo->next;
  }
  if (!found) {
    fprintf(stderr, "ERROR: invalid value for %s field.  Use -H for possible values\n", post_field_name);
    return 1;
  }
  return 0;
}

int read_config_files(struct pastebinc_config *config) {
  GError *error;
  GKeyFileFlags flags;
  gsize length;
  char conffile[256];
  static const char *default_conf = CONFDIR "/" CONFFILE;

  flags = G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS;

  if (config->provider == NULL) {
    // use system default provider
    config->keyfile = g_key_file_new();
    error = NULL;

    if (access(default_conf, R_OK) == -1) {
      fprintf(stderr, "ERROR: Can not access defaults config file: %s\n", default_conf);
      return 1;
    }
    if (!g_key_file_load_from_file(config->keyfile, default_conf, flags, &error)) {
      g_error("Error loading default config file [%s]: %s\n", default_conf, error->message);
      g_free(error);
      return 1;
    }

    config->provider = g_key_file_get_string(config->keyfile, "defaults", "provider", NULL);

    g_key_file_free(config->keyfile);
    g_free(error);
    config->keyfile = NULL;
  }

  sprintf(conffile, "%s/%s.conf", CONFDIR, config->provider);

  if (config->verbose) {
    fprintf(stderr, "DEBUG: Provider: %s\n", config->provider);
    fprintf(stderr, "DEBUG: Config file: %s\n", conffile);
  }

  if (access(conffile, R_OK) == -1) {
    fprintf(stderr, "ERROR: Can not access config file: %s\n", conffile);
    fprintf(stderr, "Perhaps the provider you specified (%s) does not exist?\n", config->provider);
    return 1;
  }

  config->keyfile = g_key_file_new();
  error = NULL;

  if (!g_key_file_load_from_file(config->keyfile, conffile, flags, &error)) {
    g_error("%s\n", error->message);
    g_free(error);
    return 1;
  }

  if (g_key_file_has_group(config->keyfile, "user_fields")) {
    gchar **fields = NULL;
    gchar **field = NULL;

    t_user_field_option *last_ufo = NULL;
    fields = g_key_file_get_keys(config->keyfile, "user_fields", NULL, NULL);
    field = fields;
    while (*field != NULL) {
      t_user_field_option *ufo = (t_user_field_option *) malloc(sizeof(t_user_field_option));
      gchar** values = NULL;
      gchar** value = NULL;
      int first = 1;

      ufo->next = NULL;
      ufo->values = NULL;
      ufo->name = *field;

      if (config->user_field_options == NULL)
        config->user_field_options = ufo;

      if (last_ufo != NULL)
        last_ufo->next = ufo;

      last_ufo = ufo;

      t_user_field_option_value *last_ufov = NULL;
      values = g_key_file_get_string_list(config->keyfile, "user_fields", *field, NULL, NULL);
      value = values;

      while (*value != NULL) {
        t_user_field_option_value *ufov = (t_user_field_option_value *) malloc(sizeof(t_user_field_option_value));
        ufov->post_value = strtok(*value, ":");
        ufov->user_value = strtok(NULL, ":");
        ufov->next = NULL;

        if (ufo->values == NULL)
          ufo->values = ufov;

        if (last_ufov != NULL)
          last_ufov->next = ufov;

        last_ufov = ufov;

        value++;
        first = 0;
      }

      // TODO: freeing these throws away the values I saved into ufov...
      // g_strfreev(values);
      field++;
    }
    // TODO: freeing these throws away the values I saved into ufo...
    // g_strfreev(fields);
  }

  return 0;
}

/*
 * Print basic usage information for users.
 */
int display_usage(struct pastebinc_config *config, int show_extended) {
  fprintf(stderr,
    PROGNAME " " VERSION "\n\n"
   "Pastes whatever is piped in to stdin to pastebin.com or similar site.\n"
   "Options:\n\n"
   "  -t             'tee', or print out all input from stdin to stdout\n"
   "  -v             'verbose', or print out debugging information as I work\n"
   "  -n [value]     the name (or title) of your paste\n"
   "  -p [value]     the provider (site) to paste to (i.e. pastebin.com)\n"
   "  -d [name=val]  custom form field data to send to this provider\n"
   "  -x [value]     the expiration value to send with your paste\n"
   "  -f [value]     the format of your paste\n"
   "  -b             when this argument is present, we will bypass HTTP proxies\n"
   "  -h             print this usage message\n"
   "  -H             print this usage message with extended provider information\n"
   "\n"
   "NOTE: To see custom form fields for a provider, use both -p [provider] and -H\n"
   "      This will also give you the valid values for this provider for the -x\n"
   "      and -f flags.\n"
   );

  if (!show_extended) {
    return;
  }

  if (config->keyfile != NULL) {
    fprintf(stderr,
      "\nYou are using the following provider: %s\n",
      g_key_file_get_string(config->keyfile, "server", "name", NULL));

    fprintf(stderr,
      "It will post to the following URL: %s\n",
      g_key_file_get_string(config->keyfile, "server", "url", NULL));
  }

  fprintf(stderr, "\nThe following fields can have values supplied by the user on the command line:\n");

  t_user_field_option *ufo = config->user_field_options;
  t_user_field_option *first_ufo = NULL;
  t_user_field_option_value *first_ufov = NULL;

  while (ufo != NULL) {
    fprintf(stderr, "\nField name: %s\nOptions:\n", ufo->name);
    t_user_field_option_value *ufov = ufo->values;

    if (first_ufo == NULL)
      first_ufo = ufo;
    if (first_ufov == NULL)
      first_ufov = ufov;

    while (ufov != NULL) {
      fprintf(stderr, "    %s (%s)\n", ufov->post_value, ufov->user_value);
      ufov = ufov->next;
    }
    ufo = ufo->next;
    if (first_ufov == NULL)
      first_ufo = NULL;
  }

  if (first_ufo != NULL && first_ufov != NULL)
    fprintf(stderr,
      "\nDo this by passing them after the -f argument, like this:\n"
      PROGNAME " -p %s -f \"%s=%s\"\n",
     config->provider, first_ufo->name, first_ufov->post_value);
}
