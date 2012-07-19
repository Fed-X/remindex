#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <time.h>
#include "onion/onion.h"
#include "curl/curl.h"
#include "curl/easy.h"

#define AVERAGE_FILE_SIZE 6000000
#define FILE_SIZE_INCREMENT 1500000

typedef struct stream {
  unsigned short int listeners;
  unsigned long int size;
  unsigned long int allocated_size;
  bool finished;
  bool broken;
  char *stream;
  struct stream *last;
  struct stream *next;
} stream;

typedef struct channel {
  char *name;
  unsigned short int listeners;
  stream *streams;
  struct channel *last;
  struct channel *next;
} channel;

typedef struct {
  onion_response *resp;
  onion_request *req;
  channel *channel;
} response_object;

channel *channels = NULL;

channel *find_channel(channel *chan, const char *name) {
  if (chan)
    if (chan->name == name)
      return chan;
    else
      if (chan->next)
        return find_channel(chan->next, name);
      else
        return NULL;
  return NULL;
}

void remove_stream(stream *str, channel *chan) {
  printf("Removing %s stream..\n", chan->name);
  if (str->last == NULL) {
    if (str->next == NULL) {
      printf("Freeing stream from front\n");
      free(str->stream);
      free(str);
      chan->streams = NULL;
    }
  } else if (str->next == NULL) {
    for (stream *pstr = str; pstr->listeners == 0; pstr = pstr->last) {
      printf("Freeing stream from back\n");
      if (pstr->last) {
        pstr->last->next = NULL;
        free(pstr->stream);
      } else {
        free(pstr->stream);
        chan->streams = NULL;
      }
      free(pstr);
    }
  }
}

void remove_channel(channel *chan) {
  if (chan->last == NULL)
    channels = chan->next;
  else if (chan->next == NULL)
    chan->last->next = NULL;
  else {
    chan->last->next = chan->next;
    chan->next->last = chan->last;
  }
  while (chan->streams)
    remove_stream(chan->streams, chan);
  printf("Freeing channel %s\n", chan->name);
  free(chan);
}

size_t stream_music(const char *incoming_stream, size_t size, size_t nmemb, response_object *ro) {
  stream *cached_stream = ro->channel->streams;
  if (!onion_response_write(ro->resp, incoming_stream, size * nmemb)) {
    ro->channel->listeners -= 1;
    cached_stream->listeners -= 1;
    cached_stream->broken = 1;
    if (cached_stream->listeners == 0)
      remove_stream(cached_stream, ro->channel);
    if (ro->channel->listeners == 0)
      remove_channel(ro->channel);
    free(ro);
    return 0;
  }
  if (cached_stream->size + size * nmemb > cached_stream->allocated_size) {
    cached_stream->stream = realloc(cached_stream->stream, cached_stream->allocated_size + FILE_SIZE_INCREMENT);
    cached_stream->allocated_size += FILE_SIZE_INCREMENT;
  }
  //snprintf(cached_stream->stream + cached_stream->size, size * nmemb, incoming_stream);
  for (register int i=0; i < size * nmemb; i++)
    cached_stream->stream[cached_stream->size + i] = incoming_stream[i];
  cached_stream->size += size * nmemb;
  return size * nmemb;
}

int start_stream(response_object *ro, bool seek, long data) {
  char url[80] = "http://localhost:8000/";
  strcat(url, ro->channel->name);
  CURL *curl = curl_easy_init();
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_music);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, ro);
  if (seek)
    curl_easy_setopt(curl, CURLOPT_RESUME_FROM, data);
  return curl_easy_perform(curl);
}

bool write_response(onion_response *resp, const char *data, int buffer, channel *chan) {
  if (!onion_response_write(resp, data, buffer)) {
    chan->listeners -= 1;
    chan->streams->listeners -= 1;
    if (chan->streams->listeners == 0)
      remove_stream(chan->streams, chan);
    if (chan->listeners == 0)
      remove_channel(chan);
    return false;
  } else return true;
}

onion_connection_status stream_handler(void *none, onion_request *req, onion_response *resp) {
  const char *client = onion_request_get_client_description(req);
  char *cname = onion_request_get_fullpath(req);
  cname += 1;
  printf("Client connecting: %s\n", client);
  onion_response_set_header(resp, "Content-Type", "audio/mpeg");
  onion_response_set_header(resp, "Connection", "keep-alive");
  onion_response_set_header(resp, "icy-name", cname);
  channel *chan = find_channel(channels, cname);
  if (chan) {
    printf("%s channel found, %s connecting\n", chan->name, client);
    chan->listeners += 1;
    chan->streams->listeners += 1;
    stream *cached_stream = chan->streams;
    long int i = 0;
    while(true) {
      if (cached_stream->size < i + 16384) {
        int size = cached_stream->size - i;
        if (size != 0)
          if (!write_response(resp, cached_stream->stream + i, size, chan))
            break;
        i += size;
        if (cached_stream->broken) {
          printf("Stream broken, %s taking over\n", client);
          cached_stream->broken = false;
          response_object *ro = malloc( sizeof(response_object) );
          ro->resp = resp;
          ro->req = req;
          ro->channel = chan;
          start_stream(ro, true, cached_stream->size);
          break;
        } else if (cached_stream->finished) {
          printf("Switching songs\n");
          cached_stream->listeners -= 1;
          cached_stream = cached_stream->last;
          cached_stream->listeners += 1;
          if (cached_stream->next->listeners == 0)
            remove_stream(cached_stream->next, chan);
          i = 0;
        }
      } else {
        if (!write_response(resp, cached_stream->stream + i, 16384, chan))
          break;
        i += 16384;
      }
    }
  } else {
    printf("%s channel not found, %s creating\n", cname, client);
    channel *chan = malloc( sizeof(channel) );
    if (channels) {
      channels->last = malloc( sizeof(channel) );
      channels->next = malloc( sizeof(channel) );
      channels->last = chan;
      chan->next = channels;
      channels = chan;
    } else {
      channels = malloc( sizeof(channel) );
      channels = chan;
      chan->next = NULL;
    }
    chan->last = NULL;
    chan->name = cname;
    chan->listeners = 1;
    chan->streams = malloc( sizeof(stream) );
    chan->streams->stream = malloc(AVERAGE_FILE_SIZE); chan->streams->allocated_size = AVERAGE_FILE_SIZE;
    chan->streams->listeners = 1; chan->streams->size = 0; chan->streams->broken = 0; chan->streams->finished = 0;
    chan->streams->next = NULL;
    chan->streams->last = NULL;
    response_object *ro = malloc( sizeof(response_object) );
    ro->resp = resp;
    ro->req = req;
    ro->channel = chan;
    while (true) {
      if (start_stream(ro, false, 0)) {
        remove_channel(ro->channel);
        break;
      }
      stream *new_stream = malloc( sizeof(stream) );
      new_stream->stream = malloc(AVERAGE_FILE_SIZE); new_stream->allocated_size = AVERAGE_FILE_SIZE;
      new_stream->listeners = 1; new_stream->size = 0; new_stream->broken = 0; new_stream->finished = 0;
      new_stream->last = NULL;
      chan->streams->finished = 1;
      chan->streams->listeners -=1;
      if (chan->streams->listeners == 0){
        remove_stream(chan->streams, chan);}
      else {
        chan->streams->last = new_stream;
        new_stream->next = chan->streams;
      }
      chan->streams = new_stream;
    }
  }
  return OCS_PROCESSED;
}

int start_onion(void) {
  onion *server = onion_new(O_POOL);
  onion_set_timeout(server, 5000);
  onion_set_hostname(server, "0.0.0.0");
  onion_set_port(server, "1337");
  onion_url *urls = onion_url_new();
  onion_url_add_handler(urls, "^.*$", onion_handler_new(stream_handler, NULL, NULL));
  onion_handler *muuzik = onion_url_to_handler(urls);
  onion_set_root_handler(server, muuzik);
  return onion_listen(server);
}

int main(int argc, char **argv) {
  curl_global_init(CURL_GLOBAL_NOTHING);
  start_onion();
  exit(EXIT_FAILURE);
}
