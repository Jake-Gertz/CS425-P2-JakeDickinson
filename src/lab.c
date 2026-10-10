#define _POSIX_C_SOURCE 200809L
#include "lab.h"

#include <arpa/inet.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================
 * Layer 1: Packets
 * ====================================================================== */

/* Fixed-offset, byte-order-safe field access. A struct is never copied onto
 * the wire: each scalar is converted, then copied into its byte offset. */
static void put16(uint8_t *p, uint16_t v)
{
  uint16_t n = htons(v);
  memcpy(p, &n, sizeof n);
}

static void put32(uint8_t *p, uint32_t v)
{
  uint32_t n = htonl(v);
  memcpy(p, &n, sizeof n);
}

static uint16_t get16(const uint8_t *p)
{
  uint16_t n;
  memcpy(&n, p, sizeof n);
  return ntohs(n);
}

static uint32_t get32(const uint8_t *p)
{
  uint32_t n;
  memcpy(&n, p, sizeof n);
  return ntohl(n);
}

uint16_t pkt_checksum(const uint8_t *data, size_t len)
{
  uint64_t sum = 0; /* 64 bits: no overflow before the final fold */
  size_t i = 0;
  for (; i + 1 < len; i += 2)
  {
    sum += (uint64_t)(((unsigned)data[i] << 8) | data[i + 1]);
  }
  if (i < len)
  {
    sum += (uint64_t)data[i] << 8; /* odd length: pad with a zero byte */
  }
  while (sum >> 16)
  {
    sum = (sum & 0xFFFFu) + (sum >> 16);
  }
  return (uint16_t)~sum;
}

size_t pkt_encode(const packet *p, uint8_t *buf, size_t cap)
{
  if (p->type > PKT_FIN || p->len > PKT_MAX_PAYLOAD)
  {
    return 0;
  }
  if (p->len > 0 && p->payload == NULL)
  {
    return 0;
  }
  size_t n = PKT_HEADER_LEN + p->len;
  if (cap < n)
  {
    return 0;
  }
  buf[0] = p->type;
  buf[1] = 0;
  put16(buf + 2, 0);
  put32(buf + 4, p->seq);
  put16(buf + 8, p->len);
  if (p->len > 0)
  {
    memcpy(buf + PKT_HEADER_LEN, p->payload, p->len);
  }
  put16(buf + 2, pkt_checksum(buf, n));
  return n;
}

bool pkt_decode(const uint8_t *buf, size_t n, packet *out)
{
  /* Size first: nothing past the header is read until 10 + length == n. */
  if (n < PKT_HEADER_LEN || n > PKT_MAX_LEN)
  {
    return false;
  }
  uint16_t len = get16(buf + 8);
  if (PKT_HEADER_LEN + (size_t)len != n)
  {
    return false;
  }
  if (buf[0] > PKT_FIN || buf[1] != 0)
  {
    return false;
  }
  /* With the checksum left in place, an intact packet sums to 0xFFFF. */
  if (pkt_checksum(buf, n) != 0)
  {
    return false;
  }
  out->type = buf[0];
  out->seq = get32(buf + 4);
  out->len = len;
  out->payload = buf + PKT_HEADER_LEN;
  return true;
}

/* ======================================================================
 * Layer 2: Go-Back-N state machines
 * ====================================================================== */

uint32_t gbn_data_packets(size_t len)
{
  return (uint32_t)((len + PKT_MAX_PAYLOAD - 1u) / PKT_MAX_PAYLOAD);
}

int gbn_poll_timeout(bool timer_running, uint64_t deadline, uint64_t now)
{
  if (!timer_running)
  {
    return -1;
  }
  if (now >= deadline)
  {
    return 0;
  }
  uint64_t left = deadline - now;
  if (left > (uint64_t)INT_MAX)
  {
    return INT_MAX;
  }
  return (int)left;
}

/* ---- Receiver ---- */

static uint64_t receiver_deadline(const gbn_receiver *r)
{
  return r->lingering ? r->linger_until : r->last_valid + GBN_IDLE_MS;
}

static void receiver_out_reset(gbn_receiver_out *out)
{
  out->deliver = NULL;
  out->deliver_len = 0;
  out->close_file = false;
  out->reply.buf = NULL;
  out->reply.len = 0;
}

static void receiver_report(const gbn_receiver *r, gbn_receiver_out *out)
{
  out->deadline = receiver_deadline(r);
  out->status = r->status;
}

void gbn_receiver_init(gbn_receiver *r, uint64_t now)
{
  r->expected = 0;
  r->lingering = false;
  r->linger_until = 0;
  r->last_valid = now;
  r->status = GBN_RUNNING;
  memset(r->ack, 0, sizeof r->ack);
}

void gbn_receiver_on_datagram(gbn_receiver *r, const uint8_t *buf, size_t n,
                              uint64_t now, gbn_receiver_out *out)
{
  receiver_out_reset(out);
  packet p;
  if (r->status == GBN_RUNNING && pkt_decode(buf, n, &p))
  {
    r->last_valid = now;
    if (p.type != PKT_ACK)
    {
      /* In order and not yet finished: accept. Anything else (duplicate,
       * beyond a gap, repeated FIN) is discarded but still re-ACKed. */
      if (!r->lingering && p.seq == r->expected)
      {
        if (p.type == PKT_DATA)
        {
          out->deliver = p.payload;
          out->deliver_len = p.len;
        }
        else
        {
          out->close_file = true;
          r->lingering = true;
          r->linger_until = now + GBN_LINGER_MS;
        }
        r->expected++;
      }
      packet ack = {PKT_ACK, r->expected, 0, NULL};
      out->reply.len = pkt_encode(&ack, r->ack, sizeof r->ack);
      out->reply.buf = r->ack;
    }
  }
  receiver_report(r, out);
}

void gbn_receiver_on_timer(gbn_receiver *r, uint64_t now, gbn_receiver_out *out)
{
  receiver_out_reset(out);
  if (r->status == GBN_RUNNING && now >= receiver_deadline(r))
  {
    r->status = r->lingering ? GBN_DONE : GBN_FAILED;
  }
  receiver_report(r, out);
}

/* ---- Sender ---- */

static void sender_report(const gbn_sender *s, gbn_sender_out *out)
{
  out->timer_running = s->timer_running;
  out->deadline = s->deadline;
  out->status = s->status;
}

/* Hand the stored copy of packet `seq` to the I/O layer. */
static void sender_emit(gbn_sender *s, uint32_t seq, gbn_sender_out *out)
{
  uint32_t k = seq % s->window;
  out->send[out->nsend].buf = s->slot[k];
  out->send[out->nsend].len = s->slot_len[k];
  out->nsend++;
  s->sent++;
}

/* Encode packet `seq` (DATA, or the FIN when seq == num_data) into its slot.
 * The slot keeps the copy until base passes seq. */
static void sender_load(gbn_sender *s, uint32_t seq)
{
  packet p = {PKT_FIN, seq, 0, NULL};
  if (seq < s->num_data)
  {
    size_t off = (size_t)seq * PKT_MAX_PAYLOAD;
    size_t left = s->data_len - off;
    p.type = PKT_DATA;
    p.len = (uint16_t)(left < PKT_MAX_PAYLOAD ? left : PKT_MAX_PAYLOAD);
    p.payload = s->data + off;
  }
  uint32_t k = seq % s->window;
  s->slot_len[k] = pkt_encode(&p, s->slot[k], sizeof s->slot[k]);
}

/* Send while the window has room; the FIN goes only once every DATA packet
 * is ACKed. Starts the timer if anything went out and it was not running. */
static void sender_fill_window(gbn_sender *s, uint64_t now, gbn_sender_out *out)
{
  while (s->next < s->num_data && s->next < s->base + s->window)
  {
    sender_load(s, s->next);
    sender_emit(s, s->next, out);
    s->next++;
  }
  if (s->next == s->num_data && s->base == s->num_data)
  {
    sender_load(s, s->next);
    sender_emit(s, s->next, out);
    s->next++;
  }
  if (out->nsend > 0 && !s->timer_running)
  {
    s->timer_running = true;
    s->deadline = now + s->timeout_ms;
  }
}

int gbn_sender_init(gbn_sender *s, const uint8_t *data, size_t len,
                    uint32_t window, uint32_t timeout_ms)
{
  if (window < GBN_MIN_WINDOW || window > GBN_MAX_WINDOW || timeout_ms == 0)
  {
    return -1;
  }
  if (len > GBN_MAX_FILE || (data == NULL && len > 0))
  {
    return -1;
  }
  memset(s, 0, sizeof *s);
  s->data = data;
  s->data_len = len;
  s->window = window;
  s->timeout_ms = timeout_ms;
  s->num_data = gbn_data_packets(len);
  s->status = GBN_RUNNING;
  return 0;
}

void gbn_sender_start(gbn_sender *s, uint64_t now, gbn_sender_out *out)
{
  out->nsend = 0;
  sender_fill_window(s, now, out);
  sender_report(s, out);
}

void gbn_sender_on_ack(gbn_sender *s, uint32_t ack, uint64_t now, gbn_sender_out *out)
{
  out->nsend = 0;
  /* Only an ACK inside (base, next] is news; duplicates and bogus values are ignored. */
  if (s->status == GBN_RUNNING && ack > s->base && ack <= s->next)
  {
    s->base = ack; /* cumulative: may slide several packets at once */
    s->fruitless = 0;
    if (s->base == s->num_data + 1)
    {
      s->status = GBN_DONE; /* the FIN is ACKed */
      s->timer_running = false;
    }
    else
    {
      if (s->base < s->next)
      {
        s->timer_running = true; /* restart for what is still in flight */
        s->deadline = now + s->timeout_ms;
      }
      else
      {
        s->timer_running = false;
      }
      sender_fill_window(s, now, out);
    }
  }
  sender_report(s, out);
}

void gbn_sender_on_datagram(gbn_sender *s, const uint8_t *buf, size_t n,
                            uint64_t now, gbn_sender_out *out)
{
  packet p;
  if (pkt_decode(buf, n, &p) && p.type == PKT_ACK)
  {
    gbn_sender_on_ack(s, p.seq, now, out);
    return;
  }
  out->nsend = 0;
  sender_report(s, out);
}

void gbn_sender_on_timer(gbn_sender *s, uint64_t now, gbn_sender_out *out)
{
  out->nsend = 0;
  /* The timer only runs while RUNNING, so DONE and FAILED never get here. */
  if (s->timer_running && now >= s->deadline)
  {
    s->timeouts++;
    s->fruitless++;
    if (s->fruitless >= GBN_MAX_TIMEOUTS)
    {
      s->status = GBN_FAILED;
      s->timer_running = false;
    }
    else
    {
      /* Go back N: resend everything in [base, next), in order. */
      for (uint32_t seq = s->base; seq < s->next; seq++)
      {
        sender_emit(s, seq, out);
        s->retransmitted++;
      }
      s->deadline = now + s->timeout_ms;
    }
  }
  sender_report(s, out);
}

/* ======================================================================
 * Layer 3: pure helpers for the I/O layer (command line and relay hello)
 * ====================================================================== */

bool valid_session(const char *s)
{
  size_t n = strlen(s);
  if (n < 1 || n > SESSION_MAX_LEN)
  {
    return false;
  }
  for (size_t i = 0; i < n; i++)
  {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    if (!ok)
    {
      return false;
    }
  }
  return true;
}

bool parse_uint(const char *s, unsigned long min, unsigned long max, unsigned long *out)
{
  if (*s == '\0')
  {
    return false;
  }
  unsigned long v = 0;
  for (; *s != '\0'; s++)
  {
    if (*s < '0' || *s > '9')
    {
      return false;
    }
    unsigned long d = (unsigned long)(*s - '0');
    if (v > (ULONG_MAX - d) / 10u)
    {
      return false; /* would overflow */
    }
    v = v * 10u + d;
  }
  if (v < min || v > max)
  {
    return false;
  }
  *out = v;
  return true;
}

bool parse_probability(const char *s, double *out)
{
  char *end;
  double v = strtod(s, &end);
  if (end == s || *end != '\0')
  {
    return false;
  }
  if (!(v >= 0.0 && v <= PROB_MAX)) /* also rejects NaN */
  {
    return false;
  }
  *out = v;
  return true;
}

size_t format_probability(double p, char *buf, size_t cap)
{
  if (!(p >= 0.0 && p <= PROB_MAX))
  {
    return 0;
  }
  /* Plain decimal, never exponent notation: the relay rejects "1e-2". */
  char tmp[8] = {'0', '.', '0', '0', '0', '0', '0', '0'};
  unsigned long micro = (unsigned long)(p * 1000000.0 + 0.5);
  size_t n = 1;
  if (micro > 0)
  {
    for (size_t i = 8; i > 2; i--)
    {
      tmp[i - 1] = (char)('0' + micro % 10u);
      micro /= 10u;
    }
    n = 8;
    while (tmp[n - 1] == '0')
    {
      n--;
    }
  }
  if (n >= cap)
  {
    return 0;
  }
  memcpy(buf, tmp, n);
  buf[n] = '\0';
  return n;
}

/* Concatenate `count` strings into buf. Returns the length, or 0 if the
 * result plus its NUL does not fit in cap. */
static size_t join(char *buf, size_t cap, const char *const *parts, size_t count)
{
  size_t len = 0;
  for (size_t i = 0; i < count; i++)
  {
    size_t n = strlen(parts[i]);
    if (len + n >= cap)
    {
      return 0;
    }
    memcpy(buf + len, parts[i], n);
    len += n;
  }
  buf[len] = '\0';
  return len;
}

size_t hello_recv(char *buf, size_t cap, const char *session)
{
  const char *parts[] = {"HELLO ", session, " recv"};
  return join(buf, cap, parts, 3);
}

size_t hello_send(char *buf, size_t cap, const char *session,
                  double loss, double corrupt, double dup)
{
  char l[16], c[16], d[16];
  if (format_probability(loss, l, sizeof l) == 0 ||
      format_probability(corrupt, c, sizeof c) == 0 ||
      format_probability(dup, d, sizeof d) == 0)
  {
    return 0;
  }
  const char *parts[] = {"HELLO ", session, " send ", l, " ", c, " ", d};
  return join(buf, cap, parts, 8);
}

int hello_reply(const uint8_t *buf, size_t n, char *reason, size_t cap)
{
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
  {
    n--;
  }
  if (n == 2 && memcmp(buf, "OK", 2) == 0)
  {
    return 0;
  }
  if (n >= 3 && memcmp(buf, "ERR", 3) == 0 && (n == 3 || buf[3] == ' '))
  {
    size_t off = n > 3 ? 4u : 3u;
    size_t len = n - off;
    if (len > cap - 1)
    {
      len = cap - 1;
    }
    memcpy(reason, buf + off, len);
    reason[len] = '\0';
    return 1;
  }
  return -1;
}
