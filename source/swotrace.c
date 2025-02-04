/*
 * Shared code for SWO Trace for the bmtrace and bmdebug utilities. It uses
 * WinUSB or libusbK on Microsoft Windows, and libusb 1.0 on Linux.
 *
 * Copyright 2019-2022 CompuPhase
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined WIN32 || defined _WIN32
# define STRICT
# define WIN32_LEAN_AND_MEAN
# include <windows.h>
# include <winsock2.h>
# include <tchar.h>
# include <initguid.h>
# include <setupapi.h>
# include <malloc.h>
# include "usb-support.h"
# if defined __MINGW32__ || defined __MINGW64__ || defined _MSC_VER
#   include "strlcpy.h"
# endif
# if defined _MSC_VER
#   define memicmp(p1,p2,c)  _memicmp((p1),(p2),(c))
# endif
#elif defined __linux__
# include <alloca.h>
# include <errno.h>
# include <pthread.h>
# include <unistd.h>
# include <bsd/string.h>
# include <sys/stat.h>
# include <sys/socket.h>
# include <arpa/inet.h>
# include <libusb-1.0/libusb.h>
  typedef int SOCKET;
# define INVALID_SOCKET (-1)
#endif

#include "bmp-scan.h"
#include "guidriver.h"
#include "nuklear_style.h"
#include "parsetsdl.h"
#include "decodectf.h"
#include "swotrace.h"

#if defined FORTIFY
# include <alloc/fortify.h>
#endif


#if defined __linux__ || defined __FreeBSD__ || defined __APPLE__
# define stricmp(s1,s2)    strcasecmp((s1),(s2))
  static int memicmp(const unsigned char *p1, const unsigned char *p2, size_t count);
#elif defined _MSC_VER
# define stricmp(a,b)      _stricmp((a),(b))
#endif

#if !defined sizearray
# define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif


#define CHANNEL_NAMELENGTH  30
typedef struct tagCHANNELINFO {
  bool enabled;
  struct nk_color color;
  char name[CHANNEL_NAMELENGTH];
} CHANNELINFO;
static CHANNELINFO channels[NUM_CHANNELS];

void channel_set(int index, bool enabled, const char *name, struct nk_color color)
{
  assert(index >= 0 && index < NUM_CHANNELS);
  channels[index].enabled = enabled;
  channels[index].color = color;
  if (name == NULL)
    sprintf(channels[index].name, "%d", index);
  else
    strlcpy(channels[index].name, name, sizearray(channels[index].name));
}

bool channel_getenabled(int index)
{
  assert(index >= 0 && index < NUM_CHANNELS);
  return channels[index].enabled;
}

void channel_setenabled(int index, bool enabled)
{
  assert(index >= 0 && index < NUM_CHANNELS);
  channels[index].enabled = enabled;
}

/** channel_getname() returns the name of the channel and optionally copies
 *  it into the parameter.
 *  \param index    The channel index, 0 to 32
 *  \param name     The channel name is copied into this parameter, unless
 *                  this parameter is NULL.
 *  \param size     The buffer size reserved for parameter name.
 *  \return A pointer to the name in the channel structure. If parameter "name"
 *          is NULL, one can use this pointer. If parameter "name" is not NULL,
 *          note that the returned pointer to the name in the channel structure
 *          (so, not to the "name" parameter).
 */
const char *channel_getname(int index, char *name, size_t size)
{
  assert(index >= 0 && index < NUM_CHANNELS);
  if (name != NULL && size > 0)
    strlcpy(name, channels[index].name, size);
  return channels[index].name;
}

void channel_setname(int index, const char *name)
{
  assert(index >= 0 && index < NUM_CHANNELS);
  if (name == NULL)
    sprintf(channels[index].name, "%d", index);
  else
    strlcpy(channels[index].name, name, sizearray(channels[index].name));
}

struct nk_color channel_getcolor(int index)
{
  assert(index >= 0 && index < NUM_CHANNELS);
  return channels[index].color;
}

void channel_setcolor(int index, struct nk_color color)
{
  assert(index >= 0 && index < NUM_CHANNELS);
  channels[index].color = color;
}


#define PACKET_SIZE 64
#define PACKET_NUM  128
typedef struct tagPACKET {
  unsigned char data[PACKET_SIZE];
  size_t length;
  double timestamp;
} PACKET;
static PACKET trace_queue[PACKET_NUM];
static int tracequeue_head = 0, tracequeue_tail = 0;
static int tracequeue_overflow = 0;

typedef struct tagTRACESTRING {
  struct tagTRACESTRING *next;
  char *text;
  double timestamp;       /* in seconds */
  char timefmt[15];       /* formatted string with time stamp */
  unsigned short timefmt_len;
  unsigned short length, size;  /* text length & text buffer size (length <= size) */
  unsigned char channel;
  short flags;            /* used to keep state while decoding plain trace messages */
} TRACESTRING;

static SOCKET TraceSocket = INVALID_SOCKET;

#define TRACESTRING_MAXLENGTH 256
#define TRACESTRING_INITSIZE  32
static TRACESTRING tracestring_root = { NULL, NULL };
static TRACESTRING *tracestring_tail = NULL;

static unsigned char itm_cache[5]; /**< we may need to cache an ITM data packet that does
                                        not fit completely in an USB packet; ITM data
                                        packets are 5 bytes max. */
static size_t itm_cachefilled = 0;
static unsigned short itm_datasize = 1;    /* size in bytes (not bits) */
static short itm_datasz_auto = 0;
static unsigned itm_packet_errors = 0;

#define ITM_VALIDHDR(b)   (((b) & 0x07) >= 1 && ((b) & 0x07) <= 3)
#define ITM_CHANNEL(b)    (unsigned)(((b) >> 3) & 0x1f) /* get channel number from ITM packet header */
#define ITM_LENGTH(b)     (unsigned)(((b) & 0x07) == 3 ? 4 : (b) & 0x07)

static void tracestring_add(unsigned channel, const unsigned char *buffer, size_t length, double timestamp)
{
  assert(channel < NUM_CHANNELS);
  assert(buffer != NULL);
  assert(length > 0);

  /* check whether the channel is enabled (in passive mode, the target that
     sends the trace messages is oblivious of the settings in this viewer,
     so it may send trace messages for disabled channels) */
  if (!channels[channel].enabled)
    return;

  if (stream_isactive(channel)) {
    /* CTF mode */
    int count = ctf_decode(buffer, length, channel);
    if (count > 0) {
      uint16_t streamid;
      double tstamp, tstamp_relative;
      const char *message;
      while (msgstack_peek(&streamid, &tstamp, &message)) {
        TRACESTRING *item = malloc(sizeof(TRACESTRING));
        if (item != NULL) {
          memset(item, 0, sizeof(TRACESTRING));
          item->length = (unsigned short)strlen(message);
          item->size = item->length + 1;
          item->text = malloc(item->size * sizeof(unsigned char));
          if (item->text != NULL) {
            strcpy(item->text, message);
            item->length = item->size - 1;
            item->channel = (unsigned char)streamid;
            if (tstamp > 0.001)
              timestamp = tstamp; /* use precision timestamp from remote host */
            item->timestamp = timestamp;
            /* create formatted timestamp */
            if (tracestring_root.next != NULL)
              tstamp_relative = timestamp - tracestring_root.next->timestamp;
            else
              tstamp_relative = 0.0;
            if (tstamp > 0.001)
              sprintf(item->timefmt, "%.6f", tstamp_relative);
            else
              sprintf(item->timefmt, "%.3f", tstamp_relative);
            item->timefmt_len = (unsigned short)strlen(item->timefmt);
            assert(item->timefmt_len < sizearray(item->timefmt));
            /* append to tail */
            if (tracestring_tail != NULL)
              tracestring_tail->next = item;
            else
              tracestring_root.next = item;
            tracestring_tail = item;
          } else {
            free((void*)item);
          }
        }
        msgstack_pop(NULL, NULL, NULL, 0);
      }
    }
  } else {
    /* plain text mode */
    double tstamp_relative;
    unsigned idx;
    while (length > 0 && buffer[length - 1] == '\0')
      length--; /* this can happen with expansion from zero-compression */
    for (idx = 0; idx < length; idx++) {
      /* see whether to append to the recent string, or to add a new string */
      if (tracestring_tail != NULL) {
        if (buffer[idx] == '\r' || buffer[idx] == '\n') {
          tracestring_tail->flags |= 0x01;  /* on newline, create a new string */
          continue;
        } else if (tracestring_tail->channel != channel) {
          tracestring_tail->flags |= 0x01;  /* different channel, terminate previous string */
        } else if (tracestring_tail->length >= TRACESTRING_MAXLENGTH) {
          tracestring_tail->flags |= 0x01;  /* line length limit */
        }
        /* time criterion: there should not be more that 0.1 seconds between
           parts of a continued string */
        if (tracestring_tail != NULL && timestamp - tracestring_tail->timestamp > 0.1)
          tracestring_tail->flags |= 0x01;  /* interval limit */
      }

      if (tracestring_tail != NULL && (tracestring_tail->flags & 0x01) == 0) {
        /* append text to the current string */
        if (tracestring_tail->length >= tracestring_tail->size) {
          int newsize = tracestring_tail->size * 2;
          char *ptr = malloc(newsize * sizeof(unsigned char));
          if (ptr != NULL) {
            memcpy(ptr, tracestring_tail->text, tracestring_tail->length);
            free((void*)tracestring_tail->text);
            tracestring_tail->text = ptr;
            tracestring_tail->size = (unsigned short)newsize;
          }
        }
        if (tracestring_tail->length < tracestring_tail->size)
          tracestring_tail->text[tracestring_tail->length++] = buffer[idx];
      } else {
        /* create a new string */
        TRACESTRING *item;
        if (tracestring_tail == NULL && (buffer[idx] == '\r' || buffer[idx] == '\n'))
          continue; /* don't create an empty first string */
        item = malloc(sizeof(TRACESTRING));
        if (item != NULL) {
          memset(item, 0, sizeof(TRACESTRING));
          item->size = TRACESTRING_INITSIZE;
          item->text = malloc(item->size * sizeof(unsigned char));
          if (item->text != NULL) {
            item->channel = (unsigned char)channel;
            item->timestamp = timestamp;
            /* create formatted timestamp */
            if (tracestring_root.next != NULL)
              tstamp_relative = timestamp - tracestring_root.next->timestamp;
            else
              tstamp_relative = 0.0;
            sprintf(item->timefmt, "%.3f", tstamp_relative);
            item->timefmt_len = (unsigned short)strlen(item->timefmt);
            assert(item->timefmt_len < sizearray(item->timefmt));
            /* append to tail */
            if (tracestring_tail != NULL)
              tracestring_tail->next = item;
            else
              tracestring_root.next = item;
            tracestring_tail = item;
            tracestring_tail->text[tracestring_tail->length++] = buffer[idx];
          } else {
            free(item); /* adding a new string failed */
          }
        }
      }
    }
  }
}

void tracestring_clear(void)
{
  TRACESTRING *item;
  while (tracestring_root.next != NULL) {
    item = tracestring_root.next;
    tracestring_root.next = item->next;
    assert(item->text!=NULL);
    free((void*)item->text);
    free((void*)item);
  }
  tracestring_tail = NULL;
}

int tracestring_isempty(void)
{
  return (tracestring_root.next == NULL);
}

unsigned tracestring_count(void)
{
  TRACESTRING *item;
  unsigned count = 0;
  for (item = tracestring_root.next; item != NULL; item = item->next)
    count++;
  return count;
}

int tracestring_process(bool enabled)
{
  int count = 0;
  while (tracequeue_head != tracequeue_tail) {
    if (enabled) {
      const unsigned char *pktdata = trace_queue[tracequeue_head].data;
      size_t pktlen = trace_queue[tracequeue_head].length;
      unsigned chan = ~0u;
      unsigned char buffer[PACKET_SIZE];
      size_t buflen = 0;
      unsigned len;

      if (itm_cachefilled>0) {
        int skip = 0;
        chan = ITM_CHANNEL(itm_cache[0]);
        len = ITM_LENGTH(itm_cache[0]);
        if (len > itm_datasize) {
          if (itm_datasz_auto) {
            itm_datasize = len; /* if larger data word is found, datasize must be adjusted */
          } else {
            ctf_decode_reset();
            itm_packet_errors += 1;
            goto skip_packet;   /* not a valid ITM packet, ignore it */
          }
        }
        assert(itm_cachefilled <= 4);
        if (itm_cachefilled > 1) {
          /* copy data bytes still in the cache */
          memcpy(buffer + buflen, itm_cache + 1, itm_cachefilled - 1);
          buflen += itm_cachefilled - 1;
        }
        skip = len - (itm_cachefilled - 1);
        assert(skip > 0);       /* there must be data left to copy (otherwise nothing would be cached) */
        memcpy(buffer + buflen, pktdata, skip);
        buflen += skip;
        pktdata += skip;
        pktlen -= skip;
        itm_cachefilled = 0;
      } else {
        assert(pktlen > 0);
        chan = ITM_CHANNEL(*pktdata);
      }

      while (pktlen > 0) {
        if (*pktdata == 0x17) {
          /* profile packet (PC address) */
          pktdata += 5;
          pktlen = (pktlen > 5) ? pktlen - 5 : 0;
          continue;
        } else if (!ITM_VALIDHDR(*pktdata)) {
          ctf_decode_reset();
          itm_packet_errors += 1;
          goto skip_packet;     /* not a valid ITM packet, ignore it */
        }
        /* if the channel changes in the middle of a packet, add a string and
           restart */
        if (chan != ITM_CHANNEL(*pktdata)) {
          if (chan < NUM_CHANNELS && buflen > 0)
            tracestring_add(chan, buffer, buflen, trace_queue[tracequeue_head].timestamp);
          chan = ITM_CHANNEL(*pktdata);
          buflen = 0;
        }
        len = ITM_LENGTH(*pktdata);
        if (pktlen < len + 1) {
          /* store remaining data in the packet in the cache and quit */
          memcpy(itm_cache, pktdata, pktlen);
          itm_cachefilled = pktlen;
          break;
        }
        if (len > itm_datasize) {
          if (itm_datasz_auto) {
            itm_datasize = len; /* if larger data word is found, datasize must be adjusted */
          } else {
            ctf_decode_reset();
            itm_packet_errors += 1;
            goto skip_packet;   /* not a valid ITM packet, ignore it */
          }
        }
        memcpy(buffer + buflen, pktdata + 1, len);
        buflen += len;
        pktdata += len + 1;
        pktlen -= len + 1;
      }
      if (chan < NUM_CHANNELS && buflen > 0) {
        tracestring_add(chan, buffer, buflen, trace_queue[tracequeue_head].timestamp);
        count++;
      }
    }
  skip_packet:
    tracequeue_head = (tracequeue_head + 1) % PACKET_NUM;
  }

  if (!enabled)
    tracequeue_overflow = 0;  /* ignore overflow events if not running/decoding */
  return count;
}

int tracestring_find(const char *text, int curline)
{
  TRACESTRING *item;
  int line, cur_mark, len;

  assert(curline >= 0 || curline == -1);
  assert(text != NULL);
  len = strlen(text);

  cur_mark = curline + 1;
  item = tracestring_root.next;
  line = 0;
  while (item != NULL && line < cur_mark) {
    line++;
    item = item->next;
  }
  if (item == NULL || curline < 0) {
    item = tracestring_root.next;
    line = 0;
  } else {
    item = item->next;
    line++;
  }
  while ((line != cur_mark || curline < 0) && item != NULL) {
    int idx;
    curline = cur_mark;
    idx = 0;
    while (idx < item->length) {
      while (idx < item->length && toupper(item->text[idx]) != toupper(text[0]))
        idx++;
      if (idx + len > item->length)
        break;      /* not found on this line */
      if (memicmp((const unsigned char*)item->text + idx, (const unsigned char*)text, len) == 0)
        break;      /* found on this line */
      idx++;
    }
    if (idx + len <= item->length)
      return line;  /* found, stop search */
    item = item->next;
    line++;
    if (item == NULL) {
      item = tracestring_root.next;
      line = 0;
    }
  } /* while (line != cur_mark) */

  return -1;  /* not found */
}

/** tracestring_findtimestamp() finds the line closest to the given
 *  timestamp. Note that it can return -1 when there are not strings in the
 *  list.
 */
int tracestring_findtimestamp(double timestamp)
{
  TRACESTRING *item;
  int line = 0;
  for (item = tracestring_root.next; item != NULL && item->timestamp < timestamp; item = item->next)
    line += 1;
  return line - 1;
}

int tracestring_save(const char *filename)
{
  FILE *fp;
  TRACESTRING *item;
  char *buffer;
  size_t bufsize;

  fp = fopen(filename, "wt");
  if (fp == NULL)
    return 0;

  bufsize = 0;
  for (item = tracestring_root.next; item != NULL; item = item->next)
    if (item->length > bufsize)
      bufsize = item->length;

  buffer = malloc((bufsize + 1) * sizeof(char));
  if (buffer == NULL) {
    fclose(fp);
    return 0;
  }

  fprintf(fp, "Number,Name,Timestamp,Text\n");
  for (item = tracestring_root.next; item != NULL; item = item->next) {
    memcpy(buffer, item->text, item->length);
    buffer[item->length] = '\0';
    fprintf(fp, "%d,\"%s\",%.6f,\"%s\"\n", item->channel, channels[item->channel].name,
            item->timestamp, buffer);
  }

  free((void*)buffer);
  fclose(fp);
  return 1;
}


/** trace_setdatasize() sets the data size in an ITM packet, in bytes. Valid
 *  values are 1, 2 and 4. For automatic detection, set "size" to 0.
 */
void trace_setdatasize(short size)
{
  assert(size == 0 || size == 1 || size == 2 || size == 4);
  itm_datasize = (size == 0) ? 1 : size;
  itm_datasz_auto = (size == 0);
  itm_packet_errors = 0;
}

/** trace_getdatasize() returns the datasize currently set. */
short trace_getdatasize(void)
{
  return itm_datasize;
}

int trace_getpacketerrors(bool reset)
{
  int result = itm_packet_errors;
  if (reset)
    itm_packet_errors = 0;
  return result;
}

int trace_overflowerrors(bool reset)
{
  int result = tracequeue_overflow;
  if (reset)
    tracequeue_overflow = 0;
  return result;
}

static void addsample(uint32_t pc, unsigned *sample_map, uint32_t code_base, uint32_t code_top)
{
  assert(sample_map != NULL);
  if (pc < code_base || pc >= code_top)
    pc = code_top;
  unsigned idx = Address2Index(pc, code_base);
  sample_map[idx] += 1;
}

int traceprofile_process(bool enabled, unsigned *sample_map, uint32_t code_base, uint32_t code_top,
                         unsigned *overflow)
{
  int count = 0;
  int overflow_count = 0;
  while (tracequeue_head != tracequeue_tail) {
    if (enabled && sample_map != NULL) {
      const unsigned char *pktdata = trace_queue[tracequeue_head].data;
      size_t pktlen = trace_queue[tracequeue_head].length;

      /* first handle cached data (that crosses USB packets) */
      if (itm_cachefilled > 0) {
        unsigned char buffer[5];
        memcpy(buffer, itm_cache, itm_cachefilled);
        size_t needed = ITM_VALIDHDR(itm_cache[0]) ? ITM_LENGTH(itm_cache[0]) + 1 : 5;
        assert(itm_cachefilled < needed);
        needed -= itm_cachefilled;
        if (needed > pktlen) {
          /* cached data plus new data block *still* do not make a complete packet */
          memcpy(itm_cache + itm_cachefilled, pktdata, pktlen);
          itm_cachefilled += pktlen;
          pktlen = 0; /* avoid dropping into the "while" below */
        } else {
          memcpy(buffer + itm_cachefilled, pktdata, needed);
          pktdata += needed;
          pktlen -= needed;
          /* handling the packet is simpler now, because we know that we have
             a full packet (plus: we don't care about unsupported packets,
             because we can simply drop the cache, and we don't handle the
             overflow packet because it is only one byte, so it can never
             overflow */
          if (buffer[0] == 0x17) {
            uint32_t pc;
            memcpy(&pc, buffer + 1, 4);
            addsample(pc, sample_map, code_base, code_top);
            count += 1;
          }
          itm_cachefilled = 0;
        }
      }

      while (pktlen > 0) {
        if (*pktdata == 0x17) {
          /* PC sample packet */
          if (pktlen >= 5) {
            uint32_t pc;
            memcpy(&pc, pktdata + 1, 4);
            addsample(pc, sample_map, code_base, code_top);
            pktlen -= 5;
            pktdata += 5;
            count += 1;
          } else {
            memcpy(itm_cache, pktdata, pktlen);
            itm_cachefilled = pktlen;
            pktlen = 0;
          }
        } else if (*pktdata == 0x70) {
          /* overflow packet */
          pktlen -= 1;
          pktdata += 1;
          overflow_count += 1;
        } else {
          /* unknown/unsupported packet */
          unsigned len = ITM_VALIDHDR(*pktdata) ? ITM_LENGTH(*pktdata) : 4;
          len += 1; /* include packet header */
          if (pktlen >= len) {
            pktlen -= len;
            pktdata += len;
          } else {
            memcpy(itm_cache, pktdata, pktlen);
            itm_cachefilled = pktlen;
            pktlen = 0;
          }
        }
      }
    }
    tracequeue_head = (tracequeue_head + 1) % PACKET_NUM;
  }

  if (overflow != NULL)
    *overflow = overflow_count;
  return count;
}

#if defined WIN32 || defined _WIN32

static unsigned long win_errno = 0;
static int loc_errno = 0;
static HANDLE hThread = NULL;
static HANDLE hUSBdev = INVALID_HANDLE_VALUE;
static USB_INTERFACE_HANDLE hUSBiface = INVALID_HANDLE_VALUE;
static KLST_DEVINFO *usbk_Device = NULL;
static unsigned char usbTraceEP = BMP_EP_TRACE;
static LARGE_INTEGER pcfreq;


static BOOL MakeGUID(const char *label, GUID *guid)
{
  unsigned i;
  char b[5];

  /* check whether the string has a valid format &*/
  if (strlen(label) != 38)
    return FALSE;
  for (i=0; i<strlen(label); i++) {
    char c = label[i];
    if (i == 0) {
      if (c != '{')
        return FALSE;
    } else if (i == 37) {
      if (c != '}')
        return FALSE;
    } else if (i == 9 || i == 14 || i == 19 || i == 24) {
      if (c != '-')
        return FALSE;
    } else {
      if (!(c >= '0' && c <= '9') && !(c >= 'A' && c <= 'F') && !(c >= 'a' && c <= 'f'))
        return FALSE;
    }
  }

  guid->Data1 = strtoul(label+1, NULL, 16);
  guid->Data2 = (unsigned short)strtoul(label+10, NULL, 16);
  guid->Data3 = (unsigned short)strtoul(label+15, NULL, 16);
  memset(b, 0, sizeof b);
  for (i = 0; i < 2; i++) {
    memcpy(&b[0], label+20+i*2, 2*sizeof(b[0]));
    guid->Data4[i] = (unsigned char)strtoul(&b[0], NULL, 16);
  }
  for (i = 0; i < 6; i++) {
    memcpy(&b[0], label+25+i*2, 2*sizeof(b[0]));
    guid->Data4[2+i] = (unsigned char)strtoul(&b[0], NULL, 16);
  }

  return TRUE;
}

static BOOL usb_GetDevicePath(const TCHAR *guid, TCHAR *path, size_t pathsize)
{
  GUID ClsId;
  HDEVINFO hDevInfo;
  DWORD dwSize;
  BOOL result = FALSE;

  /* convert the string to a GUID */
  MakeGUID(guid, &ClsId);

  /* get the device information set for all USB devices that have a device
     interface and are currently present on the system (plugged in). */
  hDevInfo = SetupDiGetClassDevs(&ClsId, NULL, 0, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
  if (hDevInfo != INVALID_HANDLE_VALUE) {
    SP_DEVICE_INTERFACE_DATA DevIntfData;
    /* keep calling SetupDiEnumDeviceInterfaces(..) until it fails with code
       ERROR_NO_MORE_ITEMS (with call the dwMemberIdx value needs to be
       incremented to retrieve the next device interface information */
    DevIntfData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
    loc_errno = 1;
    result = SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &ClsId, 0, &DevIntfData);

    if (result) {
      SP_DEVINFO_DATA DevData;
      PSP_DEVICE_INTERFACE_DETAIL_DATA DevIntfDetailData;
      /* get more details for each of the interfaces, including the device path
         (which contains the device's VID/PID */
      DevData.cbSize = sizeof(DevData);

      /* get the required buffer size, then allocate the memory for it and
         initialize the cbSize field */
      SetupDiGetDeviceInterfaceDetail(hDevInfo, &DevIntfData, NULL, 0,&dwSize, NULL);
      DevIntfDetailData = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwSize);
#     if defined _UNICODE
        DevIntfDetailData->cbSize = sizeof(SP_INTERFACE_DEVICE_DETAIL_DATA);
#     elif defined _WIN64
        DevIntfDetailData->cbSize = 8; // sizeof(SP_INTERFACE_DEVICE_DETAIL_DATA);
#     else
        DevIntfDetailData->cbSize = 5; // sizeof(SP_INTERFACE_DEVICE_DETAIL_DATA);
#     endif

      loc_errno = 2;
      if (SetupDiGetDeviceInterfaceDetail(hDevInfo, &DevIntfData, DevIntfDetailData, dwSize, &dwSize, &DevData)) {
        assert(path!=NULL);
        assert(pathsize>0);
#       if defined _UNICODE
          memset(path, 0, pathsize*sizeof(TCHAR));
          _tcsncpy(path, (TCHAR*)DevIntfDetailData->DevicePath, pathsize-1);
#       else
          strlcpy(path, DevIntfDetailData->DevicePath, pathsize);
#       endif
      } else {
        result = FALSE;
      }

      HeapFree(GetProcessHeap(), 0, DevIntfDetailData);
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
  }

  if (!result)
    win_errno = GetLastError();
  return result;
}

static BOOL __stdcall USBK_Enumerate(KLST_HANDLE DeviceList, KLST_DEVINFO *DeviceInfo, PVOID Context)
{
  (void)DeviceList;
  assert(DeviceInfo != NULL);
  assert(Context != NULL);
  if (DeviceInfo->DevicePath != NULL && stricmp(DeviceInfo->DevicePath, (TCHAR*)Context) == 0) {
    usbk_Device = DeviceInfo;
    return FALSE; /* no need to look further */
  }
  return TRUE;
}

static BOOL usb_OpenDevice(const TCHAR *path, HANDLE *hdevUSB, USB_INTERFACE_HANDLE *hifaceUSB)
{
  BOOL result = FALSE;

  assert(hdevUSB != NULL);
  assert(hifaceUSB != NULL);
  *hdevUSB = *hifaceUSB = INVALID_HANDLE_VALUE;  /* preset, in case initialization fails */

  /* try WinUSB first */
  if (WinUsb_Load()) {
    loc_errno = 3;
    *hdevUSB = CreateFile(path, GENERIC_WRITE | GENERIC_READ,
                          FILE_SHARE_WRITE | FILE_SHARE_READ,
                          NULL, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (*hdevUSB != INVALID_HANDLE_VALUE) {
      assert(WinUsb_IsActive());
      loc_errno = 4;
      result = _WinUsb_Initialize(*hdevUSB, hifaceUSB);
      if (!result) {
        win_errno = GetLastError();
        CloseHandle(*hdevUSB);
        WinUsb_Unload();
        *hdevUSB = *hifaceUSB = INVALID_HANDLE_VALUE;
      }
    } else {
      win_errno = GetLastError();
    }
  }

  /* try libusbK next */
  if (!result && UsbK_Load()) {
    KLST_DEVINFO *DeviceList;
    uint32_t count;
    loc_errno = 5;
    if (_LstK_Init(&DeviceList, 0) && _LstK_Count(DeviceList, &count) && count > 0) {
      usbk_Device = NULL;
      _LstK_Enumerate(DeviceList, USBK_Enumerate, (void*)path);
      if (usbk_Device != NULL) {
        *hdevUSB = usbk_Device;
        loc_errno = 6;
        result = _UsbK_Init(hifaceUSB, usbk_Device);
        if (!result) {
          win_errno = GetLastError();
          _LstK_Free(DeviceList);
          UsbK_Unload();
          *hdevUSB = *hifaceUSB = INVALID_HANDLE_VALUE;
        }
      }
      _LstK_Free(DeviceList);
    }
  }

  return result;
}

/* endpoint: use 0x85 for input endpoint #5
 */
static BOOL usb_ConfigEndpoint(USB_INTERFACE_HANDLE hUSB, unsigned char endpoint)
{
  BOOL result;
  USB_INTERFACE_DESCRIPTOR ifaceDescriptor;

  assert(hUSB != INVALID_HANDLE_VALUE);
  usbTraceEP = endpoint;

  if (WinUsb_IsActive()) {
    loc_errno = 7;
    if (_WinUsb_QueryInterfaceSettings(hUSB, 0, &ifaceDescriptor)) {
      USB_PIPE_INFORMATION pipeInfo;
      int idx;
      for (idx=0; idx<ifaceDescriptor.bNumEndpoints; idx++) {
        memset(&pipeInfo, 0, sizeof(pipeInfo));
        loc_errno = 8;
        result = _WinUsb_QueryPipe(hUSB, 0, (unsigned char)idx, &pipeInfo);
        if (result && pipeInfo.PipeId == endpoint)
          return TRUE;
      }
    }
  } else if (UsbK_IsActive()) {
    loc_errno = 9;
    if (_UsbK_QueryInterfaceSettings(hUSB, 0, &ifaceDescriptor)) {
      USB_PIPE_INFORMATION pipeInfo;
      int idx;
      for (idx=0; idx<ifaceDescriptor.bNumEndpoints; idx++) {
        memset(&pipeInfo, 0, sizeof(pipeInfo));
        loc_errno = 10;
        result = _UsbK_QueryPipe(hUSB, 0, (unsigned char)idx, &pipeInfo);
        if (result && pipeInfo.PipeId == endpoint)
          return TRUE;
      }
    }
  }

  win_errno = GetLastError();
  return FALSE;
}

/** get_timestamp() returns a precision timestamp; the returned value is in
 *  seconds, but the fractional part has a precision of at least milliseconds.
 */
double get_timestamp(void)
{
  LARGE_INTEGER t;

  if (pcfreq.LowPart == 0 && pcfreq.HighPart == 0)
    QueryPerformanceFrequency(&pcfreq);
  QueryPerformanceCounter(&t);
  return (double)t.QuadPart / (double)pcfreq.QuadPart;
}

static DWORD __stdcall trace_read(LPVOID arg)
{
  unsigned char buffer[PACKET_SIZE];

  (void)arg;

  if (TraceSocket != INVALID_SOCKET) {
    for ( ;; ) {
      int result = recv(TraceSocket, (char*)buffer, sizearray(buffer), 0);
      if (result > 0) {
        int next = (tracequeue_tail + 1) % PACKET_NUM;
        if (next != tracequeue_head) {
          memcpy(trace_queue[tracequeue_tail].data, buffer, result);
          trace_queue[tracequeue_tail].length = result;
          trace_queue[tracequeue_tail].timestamp = get_timestamp();
          tracequeue_tail = next;
          PostMessage((HWND)guidriver_apphandle(), WM_USER, 0, 0L); /* just a flag to wake up the GUI */
        } else {
          tracequeue_overflow += 1; /* notify packet queue overflow */
        }
      } else if (result < 0) {
        break;
      }
    }
  } else if (WinUsb_IsActive()) {
    for ( ;; ) {
      uint32_t numread = 0;
      if (_WinUsb_ReadPipe(hUSBiface, usbTraceEP, buffer, sizearray(buffer), &numread, NULL)) {
        /* add the packet to the queue */
        if (numread > 0) {
          int next = (tracequeue_tail + 1) % PACKET_NUM;
          if (next != tracequeue_head) {
            memcpy(trace_queue[tracequeue_tail].data, buffer, numread);
            trace_queue[tracequeue_tail].length = numread;
            trace_queue[tracequeue_tail].timestamp = get_timestamp();
            tracequeue_tail = next;
            PostMessage((HWND)guidriver_apphandle(), WM_USER, 0, 0L); /* just a flag to wake up the GUI */
          } else {
            tracequeue_overflow += 1; /* notify packet queue overflow */
          }
        }
      } else {
        Sleep(50);
      }
    }
  } else if (UsbK_IsActive()) {
    for ( ;; ) {
      uint32_t numread = 0;
      if (_UsbK_ReadPipe(hUSBiface, usbTraceEP, buffer, sizearray(buffer), &numread, NULL)) {
        /* add the packet to the queue */
        if (numread > 0) {
          int next = (tracequeue_tail + 1) % PACKET_NUM;
          if (next != tracequeue_head) {
            memcpy(trace_queue[tracequeue_tail].data, buffer, numread);
            trace_queue[tracequeue_tail].length = numread;
            trace_queue[tracequeue_tail].timestamp = get_timestamp();
            tracequeue_tail = next;
            PostMessage((HWND)guidriver_apphandle(), WM_USER, 0, 0L); /* just a flag to wake up the GUI */
          } else {
            tracequeue_overflow += 1; /* notify packet queue overflow */
          }
        }
      } else {
        Sleep(50);
      }
    }
  }

  return 0;
}

/** trace_init() opens the SWO tracing channel. If ipaddress is NULL, the USB
 *  channel is opened and endpoint is the USB endpoint. If ipaddress is a valid
 *  IP address, endpoint is the port number.
 */
int trace_init(unsigned short endpoint, const char *ipaddress)
{
  loc_errno = 0;
  win_errno = 0;
  tracequeue_overflow = 0;
  if (hThread != NULL && hUSBiface != INVALID_HANDLE_VALUE)
    return TRACESTAT_OK;            /* double initialization */

  /* if a previous initialization did not succeed completely, clean up before
     retrying */
  trace_close();

  if (ipaddress != NULL) {
    struct sockaddr_in server;
    if ((TraceSocket = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
      win_errno = WSAGetLastError();
      return TRACESTAT_NO_INTERFACE;
    }
    server.sin_addr.s_addr = inet_addr(ipaddress);
    server.sin_family = AF_INET;
    server.sin_port = htons(endpoint);
    if (connect(TraceSocket, (struct sockaddr*)&server, sizeof(server)) != 0) {
      win_errno = WSAGetLastError();
      closesocket(TraceSocket);
      TraceSocket = INVALID_SOCKET;
      return TRACESTAT_NO_PIPE;
    }
  } else {
    TCHAR guid[100], path[_MAX_PATH];
    if (!find_bmp(0, BMP_IF_TRACE, guid, sizearray(guid)))
      return TRACESTAT_NO_INTERFACE;  /* Black Magic Probe not found (trace interface not found) */
    if (!usb_GetDevicePath(guid, path, sizearray(path)))
      return TRACESTAT_NO_DEVPATH;    /* device path to trace interface not found (should not occur) */

    if (!usb_OpenDevice(path, &hUSBdev, &hUSBiface))
      return TRACESTAT_NO_ACCESS;     /* failure opening the device interface */
    if (!usb_ConfigEndpoint(hUSBiface, endpoint))
      return TRACESTAT_NO_PIPE;       /* endpoint pipe could not be found -> not a Black Magic Probe? */
  }

  hThread = CreateThread(NULL, 0, trace_read, NULL, 0, NULL);
  if (hThread == NULL) {
    loc_errno = 11;
    win_errno = GetLastError();
    return TRACESTAT_NO_THREAD;
  }
  SetThreadPriority(hThread, THREAD_PRIORITY_HIGHEST);

  return TRACESTAT_OK;
}

void trace_close(void)
{
  loc_errno = 0;
  win_errno = 0;
  if (hThread != NULL) {
    TerminateThread(hThread, 0);
    hThread = NULL;
  }
  if (hUSBiface != INVALID_HANDLE_VALUE) {
    assert(hUSBdev != INVALID_HANDLE_VALUE);  /* if hUSBiface is valid, hUSBdev must be too */
    if (WinUsb_IsActive()) {
      CloseHandle(hUSBdev);
      _WinUsb_Free(hUSBiface);
      WinUsb_Unload();
    } else if (UsbK_IsActive()) {
      _UsbK_Free(hUSBiface);
      UsbK_Unload();
    }
    hUSBdev = hUSBiface = INVALID_HANDLE_VALUE;
  }
  if (TraceSocket != INVALID_SOCKET) {
    closesocket(TraceSocket);
    TraceSocket = INVALID_SOCKET;
  }
}

unsigned long trace_errno(int *loc)
{
  if (loc != NULL)
    *loc = loc_errno;
  return win_errno;
}

#else

static pthread_t hThread;
static libusb_device_handle *hUSBiface;
static unsigned char usbTraceEP = BMP_EP_TRACE;
static volatile int force_exit;

static int memicmp(const unsigned char *p1, const unsigned char *p2, size_t count)
{
  int diff = 0;
  while (count-- > 0 && diff == 0)
    diff = toupper(*p1++) - toupper(*p2++);
  return diff;
}

/** get_timestamp() returns a precision timestamp; the returned value is in
 *  seconds, but the fractional part has a precision of at least milliseconds.
 */
double get_timestamp(void)
{
  struct timeval tv;
  gettimeofday(&tv,NULL);
  return tv.tv_sec + tv.tv_usec / 1000000.0;
}

static void *trace_read(void *arg)
{
  unsigned char buffer[PACKET_SIZE];
  int numread = 0;

  (void)arg;
  while (!force_exit && hThread != 0 && hUSBiface != NULL) {
    if (libusb_bulk_transfer(hUSBiface, usbTraceEP, buffer, sizeof(buffer), &numread, 0) == 0) {
      /* add the packet to the queue */
      if (numread > 0) {
        int next = (tracequeue_tail + 1) % PACKET_NUM;
        if (next != tracequeue_head) {
          memcpy(trace_queue[tracequeue_tail].data, buffer, numread);
          trace_queue[tracequeue_tail].length = numread;
          trace_queue[tracequeue_tail].timestamp = get_timestamp();
          tracequeue_tail = next;
        } else {
          tracequeue_overflow += 1; /* notify packet queue overflow */
        }
      }
    }
  }
  force_exit = 0;
  return 0;
}

static int usb_OpenDevice(libusb_device_handle **hUSB, const char *path)
{
  libusb_device **devs;
  libusb_device_handle *handle;
  ssize_t cnt;
  int devidx, i, res;

  /* get list of all devices */
  cnt = libusb_get_device_list(0, &devs);
  if (cnt < 0)
    return TRACESTAT_INIT_FAILED;

  /* find the BMP (this always opens the first device with the matching VID:PID) */
  devidx = -1;
  for (i = 0; devs[i] != NULL; i++) {
    struct libusb_device_descriptor desc;
    res = libusb_get_device_descriptor(devs[i], &desc);
    if (res >= 0 && desc.idVendor == BMP_VID && desc.idProduct == BMP_PID) {
      uint8_t bus = libusb_get_bus_number(devs[i]);
      uint8_t port = libusb_get_port_number(devs[i]);
      char *dash;
      assert(path != NULL);
      if (bus == strtol(path, &dash, 10) && *dash == '-' && port == strtol(dash + 1, NULL, 10)) {
        devidx = i;
        break;
      }
    }
  }
  if (devidx < 0) {
    libusb_free_device_list(devs, 1);
    return TRACESTAT_NO_DEVPATH;
  }

  res = libusb_open(devs[devidx], &handle);
  libusb_free_device_list(devs, 1);
  if (res < 0)
    return TRACESTAT_NO_ACCESS;

  /* connect to the BMP capture interface */
  res = libusb_claim_interface(handle, BMP_IF_TRACE);
  if (res < 0) {
    libusb_close(handle);
    return TRACESTAT_NO_INTERFACE;
  }

  assert(hUSB != NULL);
  *hUSB = handle;
  return TRACESTAT_OK;
}

int trace_init(unsigned short endpoint, const char *ipaddress)
{
  int result;

  tracequeue_overflow = 0;
  usbTraceEP = endpoint;

  if (hThread != 0 && hUSBiface != NULL)
    return TRACESTAT_OK;            /* double initialization */

  /* if a previous initialization did not succeed completely, clean up before
     retrying */
  trace_close();

  if (ipaddress != NULL) {
    struct sockaddr_in server;
    if ((TraceSocket = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET)
      return TRACESTAT_NO_INTERFACE;

    server.sin_addr.s_addr = inet_addr(ipaddress);
    server.sin_family = AF_INET;
    server.sin_port = htons(endpoint);
    if (connect(TraceSocket, (struct sockaddr*)&server, sizeof(server)) != 0) {
      close(TraceSocket);
      TraceSocket = INVALID_SOCKET;
      return TRACESTAT_NO_PIPE;
    }
  } else {
    char dev_id[50];
    if (!find_bmp(0, BMP_IF_TRACE, dev_id, sizearray(dev_id)))
      return TRACESTAT_NO_INTERFACE;  /* Black Magic Probe not found (trace interface not found) */

    result = libusb_init(0);
    if (result < 0)
      return TRACESTAT_INIT_FAILED;

    result = usb_OpenDevice(&hUSBiface, dev_id);
    if (result != TRACESTAT_OK)
      return result;
  }

  force_exit = 0;
  result = pthread_create(&hThread, NULL, trace_read, NULL);
  if (result != 0)
    return TRACESTAT_NO_THREAD;

  return TRACESTAT_OK;
}

void trace_close(void)
{
  if (hThread != 0) {
    force_exit = 1;
    while (force_exit)
      usleep(10*1000);
    hThread = 0;
  }
  if (hUSBiface != NULL) {
    libusb_close(hUSBiface);
    hUSBiface = NULL;
  }
}

unsigned long trace_errno(int *loc)
{
  (void)loc;  /* parameter currently only relevant for Windows */
  return errno;
}
#endif

static TRACESTRING statusmessage_root = { NULL, NULL };

void tracelog_statusmsg(int type, const char *msg, int code)
{
  TRACESTRING *item, *tail;

  assert(type == TRACESTATMSG_BMP || type == TRACESTATMSG_CTF);
  assert(msg != NULL);
  item = malloc(sizeof(TRACESTRING));
  if (item != NULL) {
    memset(item, 0, sizeof(TRACESTRING));
    item->length = (unsigned short)strlen(msg);
    item->size = item->length + 1;
    item->text = malloc(item->size * sizeof(unsigned char));
    if (item->text != NULL) {
      strcpy(item->text, msg);
      item->channel = (unsigned char)type;
      item->flags = (short)code;
      /* append to tail */
      for (tail = &statusmessage_root; tail->next != NULL; tail = tail->next)
        {}
      tail->next = item;
    } else {
      free((void*)item);
    }
  }
}

void tracelog_statusclear(void)
{
  TRACESTRING *item;
  while (statusmessage_root.next != NULL) {
    item = statusmessage_root.next;
    statusmessage_root.next = item->next;
    assert(item->text!=NULL);
    free((void*)item->text);
    free((void*)item);
  }
}

const char *tracelog_getstatusmsg(int idx)
{
  for (TRACESTRING *item = statusmessage_root.next; item != NULL; item = item->next) {
    if (idx-- == 0)
      return item->text;
  }
  return NULL;
}

float tracelog_labelwidth(float rowheight)
{
  int idx;
  float labelwidth = 0;
  for (idx = 0; idx < NUM_CHANNELS; idx++) {
    int len = strlen(channels[idx].name);
    if (labelwidth < len)
      labelwidth = len;
  }
  return labelwidth * (rowheight / 2);
}

/* tracelog_widget() draws the text in the log window and scrolls to the last line
   if new text was added */
void tracelog_widget(struct nk_context *ctx, const char *id, float rowheight, int limitlines,
                     int markline, const TRACEFILTER *filters, nk_flags widget_flags)
{
  TRACESTRING *item;
  int labelwidth, tstampwidth;
  struct nk_rect rcwidget = nk_layout_widget_bounds(ctx);
  struct nk_style_window *stwin = &ctx->style.window;
  struct nk_style_button stbtn = ctx->style.button;
  struct nk_user_font const *font = ctx->style.font;

  /* preset common parts of the new button style */
  stbtn.border = 0;
  stbtn.rounding = 0;
  stbtn.padding.x = stbtn.padding.y = 0;

  /* check the length of the longest channel name, and the longest timestamp */
  labelwidth = (int)tracelog_labelwidth(rowheight) + 10;
  tstampwidth = 0;
  for (item = tracestring_root.next; item != NULL; item = item->next)
    if (tstampwidth < item->timefmt_len)
      tstampwidth = item->timefmt_len;
  tstampwidth = (int)((tstampwidth * rowheight) / 2) + 10;

  /* (near) black background on group */
  nk_style_push_color(ctx, &stwin->fixed_background.data.color, COLOUR_BG0);
  if (nk_group_begin(ctx, id, widget_flags)) {
    static int recent_markline = -1;
    static int scrollpos = 0;
    static int linecount = 0;
    static int skiplines = 0;
    if (limitlines < 0)
      skiplines = 0;
    int skip = skiplines;
    int lines = 0;
    float lineheight = 0;
    for (item = tracestring_root.next; item != NULL; item = item->next) {
      assert(item->text != NULL);
      if (skip > 0) {
        skip -= 1;
        continue;
      }
      if (filters != NULL && filters[0].expr != NULL && filters[0].enabled) {
        /* check filters (first count how many there are) */
        int idx, match, count_enabled;
        match = 1;  /* preset to "match all except inverted filters" */
        for (idx = count_enabled = 0; filters[idx].expr != NULL; idx++) {
          if (filters[idx].enabled) {
            count_enabled += 1;
            if (filters[idx].expr[0] != '~')
              match = 0;  /* valid non-inverted filter, switch to "match only filters" */
          }
        }
        /* check normal filters */
        if (!match) {
          for (idx = 0; filters[idx].expr != NULL && !match; idx++) {
            if (filters[idx].enabled && filters[idx].expr[0] != '~')
              match = (strstr(item->text, filters[idx].expr) != NULL);
          }
        }
        /* check inverted filters */
        if (match) {
          for (idx = 0; filters[idx].expr != NULL && match; idx++) {
            if (filters[idx].enabled && filters[idx].expr[0] == '~')
              match = (strstr(item->text, filters[idx].expr + 1) == NULL);
          }
        }
        if (!match)
          continue; /* text matches none of the normal filters, or matches one of the inverted filters -> skip it */
      }
      nk_layout_row_begin(ctx, NK_STATIC, rowheight, 4);
      if (lineheight <= 0.1) {
        struct nk_rect rcline = nk_layout_widget_bounds(ctx);
        lineheight = rcline.h;
      }
      /* marker symbol */
      nk_layout_row_push(ctx, rowheight); /* width is same as height*/
      if (lines == markline) {
        stbtn.normal.data.color = stbtn.hover.data.color
          = stbtn.active.data.color = stbtn.text_background
          = COLOUR_BG0;
        stbtn.text_normal = stbtn.text_active = stbtn.text_hover = COLOUR_FG_YELLOW;
        nk_button_symbol_styled(ctx, &stbtn, NK_SYMBOL_TRIANGLE_RIGHT);
      } else {
        nk_spacing(ctx, 1);
      }
      /* channel label */
      assert(item->channel < NUM_CHANNELS);
      stbtn.normal.data.color = stbtn.hover.data.color
        = stbtn.active.data.color = stbtn.text_background
        = channels[item->channel].color;
      struct nk_color clrtxt;
      if (channels[item->channel].color.r + 2 * channels[item->channel].color.g + channels[item->channel].color.b < 700)
        clrtxt = COLOUR_HIGHLIGHT;
      else
        clrtxt = COLOUR_BG0;
      stbtn.text_normal = stbtn.text_active = stbtn.text_hover = clrtxt;
      nk_layout_row_push(ctx, labelwidth);
      nk_button_label_styled(ctx, &stbtn, channels[item->channel].name);
      /* timestamp (relative time since previous trace) */
      nk_layout_row_push(ctx, tstampwidth);
      nk_label_colored(ctx, item->timefmt, NK_TEXT_RIGHT, COLOUR_FG_YELLOW);
      /* calculate size of the text */
      assert(font != NULL && font->width != NULL);
      int textwidth = (int)font->width(font->userdata, font->height, item->text, item->length) + 10;
      nk_layout_row_push(ctx, textwidth);
      if (lines == markline)
        nk_text_colored(ctx, item->text, item->length, NK_TEXT_LEFT, COLOUR_FG_YELLOW);
      else
        nk_text(ctx, item->text, item->length, NK_TEXT_LEFT);
      nk_layout_row_end(ctx);
      lines++;
    }
    if (limitlines > 0)
      skiplines = (lines > limitlines) ? lines - limitlines : 0;
    if (lines == 0 && statusmessage_root.next != NULL) {
      for (item = statusmessage_root.next; item != NULL; item = item->next) {
        struct nk_color clr;
        if (item->flags < 0)
          clr = COLOUR_FG_RED;
        else if (item->channel == TRACESTATMSG_CTF)
          clr = COLOUR_FG_AQUA;
        else
          clr = COLOUR_FG_YELLOW;
        nk_layout_row_dynamic(ctx, rowheight, 1);
        nk_label_colored(ctx, item->text, NK_TEXT_LEFT, clr);
        lines++;
      }
    } else {
      nk_layout_row_dynamic(ctx, rowheight, 1);
      nk_spacing(ctx, 1);
    }
    nk_group_end(ctx);
    /* calculate scrolling
       1) if number of lines change, scroll to the last line
       2) if line to mark is different than last time (and valid) make that
          line visible */
    if (lineheight < 0.1)
      lineheight = 1; /* only to avoid a division by zero (for the special case of 0 text lines */
    int ypos = scrollpos;
    int widgetlines = (int)((rcwidget.h - 2 * stwin->padding.y) / lineheight);
    if (lines != linecount) {
      linecount = lines;
      ypos = (int)((lines - widgetlines + 1) * lineheight);
    } else if (markline != recent_markline) {
      recent_markline = markline;
      if (markline >= 0) {
        ypos = markline - widgetlines / 2;
        if (ypos > lines - widgetlines + 1)
          ypos = lines - widgetlines + 1;
        ypos = (int)(ypos * lineheight);
      }
    }
    if (ypos < 0)
      ypos = 0;
    if (ypos != scrollpos) {
      nk_group_set_scroll(ctx, id, 0, ypos);
      scrollpos = ypos;
    }
  }
  nk_style_pop_color(ctx);
}


typedef struct tagTLMARK {
  float pos;
  int count;
} TLMARK;
typedef struct tagTIMELINE {
  TLMARK *marks;
  size_t length, size;  /* number of entries & max. number of entries */
} TIMELINE;

#define EPSILON     0.001
#define FLTEQ(a,b)  ((a)-EPSILON<(b) && (a)+EPSILON>(b))  /* test whether floating-point values are equal within a small margin */
#define MARK_SECOND   1000000
static float mark_spacing = 100.0;              /* spacing between two mark_deltatime positions */
static unsigned long mark_scale = MARK_SECOND;  /* 1 -> us, 1000 -> ms, 1000000 -> s, 60000000 -> min, etc. */
static unsigned long mark_deltatime = 1;        /* in seconds / mark_scale */
static TRACESTRING *tracestring_tail_prev = NULL;
static TIMELINE timeline[NUM_CHANNELS];
static float timeline_maxpos = 0.0;             /* width of the timeline canvas */
static double timeoffset = 0.0;
static int timeline_maxcount = 1;               /* count of traces that collapse on the same marker line on the timeline */

void timeline_getconfig(double *spacing, unsigned long *scale, unsigned long *delta)
{
  assert(spacing != NULL);
  *spacing = mark_spacing;
  assert(scale != NULL);
  *scale = mark_scale;
  assert(delta != NULL);
  *delta = mark_deltatime;
}

void timeline_setconfig(double spacing, unsigned long scale, unsigned long delta)
{
  if (spacing > 10.0 && scale > 0 && delta > 0 && delta <= 100) {
    mark_spacing = spacing;
    mark_scale = scale;
    mark_deltatime = delta;
  }
}

void timeline_rebuild(int limitlines)
{
  static int skiplines = 0;
  if (limitlines < 0)
    skiplines = 0;

  timeline_maxpos = 0.0;  /* this variable is recalculated */
  timeoffset = 0.0;
  timeline_maxcount = 1;

  /* marks only get added, until the list is cleared completely */
  if (tracestring_root.next == NULL) {
    for (int chan = 0; chan < NUM_CHANNELS; chan++) {
      if (timeline[chan].marks != NULL) {
        free((void*)timeline[chan].marks);
        timeline[chan].marks = NULL;
        timeline[chan].length = timeline[chan].size = 0;
      }
    }
    skiplines = 0;
  } else {
    assert(tracestring_root.next != NULL);
    timeoffset = tracestring_root.next->timestamp;
    int chan;
    for (chan = 0; chan < NUM_CHANNELS; chan++)
      timeline[chan].length = 0;
    int skip = skiplines;
    for (TRACESTRING *item = tracestring_root.next; item != NULL; item = item->next) {
      int idx;
      float pos;
      chan = item->channel;
      assert(chan >= 0 && chan < NUM_CHANNELS);
      if (!channels[chan].enabled)
        continue;
      if (skip > 0) {
        skip--;
        continue;
      }
      /* make sure array is big enough for another mark */
      assert(timeline[chan].length <= timeline[chan].size);
      if (timeline[chan].length == timeline[chan].size) {
        size_t newsize;
        if (timeline[chan].marks == NULL) {
          assert(timeline[chan].size == 0);
          newsize = 32;
          timeline[chan].marks = malloc(newsize * sizeof(TLMARK));
          if (timeline[chan].marks != NULL)
            timeline[chan].size = newsize;
        } else {
          TLMARK *curptr = timeline[chan].marks; /* save, for special case of realloc fail */
          newsize = timeline[chan].size * 2;
          timeline[chan].marks = realloc(timeline[chan].marks, newsize * sizeof(TLMARK));
          if (timeline[chan].marks != NULL)
            timeline[chan].size = newsize;
          else
            timeline[chan].marks = curptr;  /* restore old pointer on realloc fail */
        }
      }
      if (timeline[chan].length == timeline[chan].size)
        continue; /* no space for another mark (growing the array failed) */
      /* convert timestamp to position */
      pos = (item->timestamp - timeoffset) * mark_spacing * MARK_SECOND / (mark_scale * mark_deltatime);
      idx = timeline[chan].length;
      /* check collapsing marks */
      assert(idx == 0 || pos >= timeline[chan].marks[idx - 1].pos);
      if (idx > 0 && (pos - timeline[chan].marks[idx - 1].pos) < 0.5) {
        idx -= 1;
        timeline[chan].marks[idx].count += 1;
        if (timeline[chan].marks[idx].count > timeline_maxcount)
          timeline_maxcount = timeline[chan].marks[idx].count;
      } else {
        timeline[chan].marks[idx].pos = pos;
        timeline[chan].marks[idx].count = 1;
        timeline[chan].length = idx + 1;
      }
      if (pos > timeline_maxpos)
        timeline_maxpos = pos;
    }
    if (limitlines > 0) {
      /* count all marks, increase the number of traces to skip, if needed */
      size_t total = 0;
      for (int chan = 0; chan < NUM_CHANNELS; chan++)
        total += timeline[chan].length;
      skiplines = (total > limitlines) ? total - limitlines : 0;
    }
  }
}

double timeline_widget(struct nk_context *ctx, const char *id, float rowheight,
                       int limitlines, nk_flags widget_flags)
{
  int labelwidth;
  double click_time = -1.0;
  struct nk_rect rcwidget;
  struct nk_style_button stbtn;

  assert(ctx != NULL);
  assert(ctx->current != NULL);
  assert(ctx->current->layout != NULL);
  if (ctx == NULL || ctx->current == NULL || ctx->current->layout == NULL)
    return click_time;

  if (tracestring_tail != tracestring_tail_prev) {
    timeline_rebuild(limitlines); /* rebuild the "trace marks" data */
    tracestring_tail_prev = tracestring_tail;
  }

  /* preset common parts of the new button style */
  stbtn = ctx->style.button;
  stbtn.padding.x = stbtn.padding.y = 0;

  /* check the length of the longest channel name */
  labelwidth = (int)tracelog_labelwidth(rowheight) + 10;
  rcwidget = nk_layout_widget_bounds(ctx);

  /* no spacing & black background on group */
  nk_style_push_vec2(ctx, &ctx->style.window.spacing, nk_vec2(0, 0));
  nk_style_push_color(ctx, &ctx->style.window.fixed_background.data.color, COLOUR_BG0);
  if (nk_group_begin(ctx, id, widget_flags | NK_WINDOW_NO_SCROLLBAR)) {
    static float timeline_maxpos_prev = 0.0f;
    struct nk_window *win = ctx->current;
    struct nk_user_font const *font = ctx->style.font;
    char valstr[60];
    float x1, x2;
    const char *unit;
    nk_uint xscroll, yscroll;

    int submark_count = 10;
    if (mark_spacing / submark_count < 20)
      submark_count = 5;
    if (mark_spacing / submark_count < 20)
      submark_count = 2;

#   define HORPADDING  4
#   define VERPADDING  1

    /* get the scroll position of the graph, because scrolling is manual */
    sprintf(valstr, "%s_graph", id);
    nk_group_get_scroll(ctx, valstr, &xscroll, &yscroll);

    /* first row: timer ticks, may not scroll vertically */
    switch (mark_scale) {
      case 1: unit = "\xC2\xB5s"; break; /* us */
      case 1000: unit = "ms";     break;
      case 1000000: unit = "s";  break;
      case 60000000: unit = "min"; break;
      default: assert(0);
    }
    nk_layout_row_begin(ctx, NK_STATIC, rowheight + VERPADDING, 3);
    nk_layout_row_push(ctx, rcwidget.w - 2 * (1.5f * rowheight));
    struct nk_rect rc = nk_layout_widget_bounds(ctx);
    nk_fill_rect(&win->buffer, rc, 0.0f, COLOUR_BG0_S);
    x2 = rc.x + rc.w;
    int submark_iter = 0;
    long mark_stamp = 0;
    long mark_inv_scale = MARK_SECOND / mark_scale;
    for (x1 = rc.x + labelwidth + HORPADDING - xscroll; x1 < x2; x1 += mark_spacing / submark_count) {
      if (submark_iter == 0) {
        struct nk_color clr;
        if (mark_stamp % mark_inv_scale == 0) {
          sprintf(valstr, "%ld s", mark_stamp / mark_inv_scale);
          clr = COLOUR_FG_YELLOW;
        } else {
          sprintf(valstr, "+%ld %s", mark_stamp, unit);
          clr = COLOUR_TEXT;
        }
        nk_stroke_line(&win->buffer, x1, rc.y, x1, rc.y + rowheight - 2, 1, clr);
        rc.x = x1 + 2;
        rc.w = x2 - rc.x;
        nk_draw_text(&win->buffer, rc, valstr, strlen(valstr), font, COLOUR_BG0, clr);
        mark_stamp += mark_deltatime;
      } else {
        nk_stroke_line(&win->buffer, x1, rc.y, x1, rc.y + rowheight / 2 - 2, 1, COLOUR_TEXT);
      }
      if (++submark_iter == submark_count)
        submark_iter = 0;
    }
    rc = nk_layout_widget_bounds(ctx);
    nk_stroke_line(&win->buffer, rc.x, rc.y + rc.h, rc.x + rc.w - HORPADDING, rc.y + rc.h, 1, COLOUR_FG_GRAY);
    rc.w = labelwidth;
    rc.h -= 1;
    nk_fill_rect(&win->buffer, rc, 0.0f, COLOUR_BG0);
    nk_spacing(ctx, 1);
    nk_layout_row_push(ctx, 1.5f * rowheight);
    if (nk_button_symbol_styled(ctx, &stbtn, NK_SYMBOL_PLUS)) {
      mark_spacing *= 1.5;
      if (mark_spacing > 700.0 && (mark_deltatime > 1 || mark_scale > 1)) {
        mark_deltatime /= 10;
        mark_spacing /= 10.0;
        if (mark_deltatime == 0 && mark_scale >= 1000) {
          mark_scale /= 1000;
          mark_deltatime = 100;
        }
      }
      timeline_rebuild(limitlines);
    }
    nk_layout_row_push(ctx, 1.5f * rowheight);
    if (nk_button_symbol_styled(ctx, &stbtn, NK_SYMBOL_MINUS)) {
      if (mark_spacing > 45.0 || mark_scale < 60000000 || mark_deltatime == 1)
        mark_spacing /= 1.5;
      if (mark_spacing < 70.0) {
        mark_deltatime *= 10;
        mark_spacing *= 10.0;
        if (mark_scale < MARK_SECOND && mark_deltatime >= 1000) {
          mark_scale *= 1000;
          mark_deltatime /= 1000;
        }
      }
      timeline_rebuild(limitlines);
    }
    nk_layout_row_end(ctx);

    /* extra (small) spacing between timeline and top of the graphs */
    nk_layout_row_dynamic(ctx, VERPADDING, 1);
    nk_spacing(ctx, 1);

    /* remaining rows collected in two groups: labels and graph */
    nk_layout_row_begin(ctx, NK_STATIC, rcwidget.h - rowheight - 2 * VERPADDING, 2);
    nk_layout_row_push(ctx, labelwidth + HORPADDING);
    sprintf(valstr, "%s_label", id);
    if (nk_group_begin(ctx, valstr, NK_WINDOW_NO_SCROLLBAR)) {
      /* labels */
      for (int chan = 0; chan < NUM_CHANNELS; chan++) {
        struct nk_color clrtxt;
        float textwidth;
        int len;
        if (!channels[chan].enabled)
          continue; /* only draw enabled channels */
        nk_layout_row_dynamic(ctx, rowheight + VERPADDING, 1);
        rc = nk_layout_widget_bounds(ctx);
        rc.x += HORPADDING;
        rc.y -= yscroll;
        rc.w -= HORPADDING;
        rc.h -= 1;
        nk_fill_rect(&win->buffer, rc, 0.0f, channels[chan].color);
        if (channels[chan].color.r + 2 * channels[chan].color.g + channels[chan].color.b < 700)
          clrtxt = COLOUR_HIGHLIGHT;
        else
          clrtxt = COLOUR_BG0;
        /* center the text in the rect */
        len = strlen(channels[chan].name);
        textwidth = font->width(font->userdata, font->height, channels[chan].name, len);
        rc.x += (rc.w - textwidth) / 2;
        nk_draw_text(&win->buffer, rc, channels[chan].name, len, font, channels[chan].color, clrtxt);
      }
      nk_group_end(ctx);
    }
    nk_layout_row_push(ctx, rcwidget.w - labelwidth - HORPADDING);
    sprintf(valstr, "%s_graph", id);
    if (nk_group_begin(ctx, valstr, 0)) {
      /* graphs */
      int row = 0;
      for (int chan = 0; chan < NUM_CHANNELS; chan++) {
        int idx;
        if (!channels[chan].enabled)
          continue; /* only draw enabled channels */
        nk_layout_row_begin(ctx, NK_STATIC, rowheight + VERPADDING, 2);
        nk_layout_row_push(ctx, timeline_maxpos);
        rc = nk_layout_widget_bounds(ctx);
        rc.y -= yscroll;
        if (row & 1)
          nk_fill_rect(&win->buffer, rc, 0.0f, COLOUR_BG0_S);
        row++;
        /* draw marks for each active channel */
        for (idx = 0; idx < (int)timeline[chan].length; idx++) {
          float x = timeline[chan].marks[idx].pos + labelwidth + 2 * HORPADDING - xscroll;
          float y = 0.75f * rowheight * (1 - (float)timeline[chan].marks[idx].count / (float)timeline_maxcount);
          nk_stroke_line(&win->buffer, x, rc.y + y, x, rc.y + rowheight, 1, COLOUR_TEXT);
        }
        nk_spacing(ctx, 1);
        nk_layout_row_end(ctx);
        /* handle mouse click in timeline, to scroll the trace view */
        if (nk_input_mouse_clicked(&ctx->input, NK_BUTTON_LEFT, rc)) {
          float pos;
          struct nk_mouse *mouse = &ctx->input.mouse;
          assert(mouse != NULL);
          assert(NK_INBOX(mouse->pos.x, mouse->pos.y, rc.x, rc.y, rc.w, rc.h));
          pos = mouse->pos.x - labelwidth - 2 * HORPADDING + xscroll;
          if (pos >= 0.0)
            click_time = pos * (mark_scale * mark_deltatime) / (mark_spacing * MARK_SECOND) + timeoffset;
        }
      }
      nk_group_end(ctx);
    }
    nk_layout_row_end(ctx);
    nk_group_end(ctx);
    /* scroll to end if more trace message markers have arrived */
    if (timeline_maxpos != timeline_maxpos_prev) {
      float x = timeline_maxpos - (rcwidget.w - labelwidth - HORPADDING);
      xscroll = (x > 0.0f) ? (nk_uint)x : 0;
      nk_group_set_scroll(ctx, valstr, xscroll, yscroll);
      timeline_maxpos_prev = timeline_maxpos;
    }
  }

  /* restore locally modified styles */
  nk_style_pop_color(ctx);
  nk_style_pop_vec2(ctx);

  return click_time;
}

