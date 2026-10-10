#ifndef LAB_H
#define LAB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================
 * Layer 1: Packets. Pure functions: bytes in, bytes or a struct out.
 * ====================================================================== */

#define PKT_HEADER_LEN  10u
#define PKT_MAX_PAYLOAD 1024u
#define PKT_MAX_LEN     (PKT_HEADER_LEN + PKT_MAX_PAYLOAD) /* 1034 */

typedef enum { PKT_DATA = 0, PKT_ACK = 1, PKT_FIN = 2 } pkt_type;

/** A decoded (or to-be-encoded) packet. `payload` is borrowed, never owned:
 *  after pkt_decode it points into the caller's datagram buffer. */
typedef struct {
  uint8_t type;           /* PKT_DATA, PKT_ACK or PKT_FIN */
  uint32_t seq;
  uint16_t len;           /* 0..PKT_MAX_PAYLOAD */
  const uint8_t *payload; /* len bytes; may be NULL when len == 0 */
} packet;

/** RFC 1071 Internet checksum of `len` bytes (odd length padded with a zero byte). */
uint16_t pkt_checksum(const uint8_t *data, size_t len);

/** Serialize `p` into `buf` (capacity `cap`) in network byte order with a valid
 *  checksum. Returns the number of bytes written (10 + len), or 0 if p->type is
 *  not 0..2, p->len > 1024, payload is NULL with len > 0, or cap is too small. */
size_t pkt_encode(const packet *p, uint8_t *buf, size_t cap);

/** Validate and parse a received datagram of exactly `n` bytes. Returns true and
 *  fills `out` only if: n >= 10, 10 + length == n, length <= 1024, type in 0..2,
 *  reserved == 0, and the checksum verifies. Never reads past buf[n-1]. */
bool pkt_decode(const uint8_t *buf, size_t n, packet *out);

/* ======================================================================
 * Layer 2: Go-Back-N state machines. No sockets, clocks or files: the
 * caller passes `now` in milliseconds and carries out the returned actions.
 * ====================================================================== */

#define GBN_MIN_WINDOW         1u
#define GBN_MAX_WINDOW         64u
#define GBN_DEFAULT_WINDOW     8u
#define GBN_DEFAULT_TIMEOUT_MS 250u
#define GBN_MAX_TIMEOUTS       10u          /* fruitless timeouts in a row before giving up */
#define GBN_LINGER_MS          2000u
#define GBN_IDLE_MS            30000u
#define GBN_MAX_FILE           (16u * 1024u * 1024u)

typedef enum { GBN_RUNNING = 0, GBN_DONE, GBN_FAILED } gbn_status;

/** One datagram to transmit. `buf` is borrowed from the state machine and valid
 *  until the next call on that state machine. */
typedef struct {
  const uint8_t *buf;
  size_t len;
} gbn_datagram;

/** Number of DATA packets for a file of `len` bytes: ceil(len / 1024). */
uint32_t gbn_data_packets(size_t len);

/** Milliseconds to pass to poll(): -1 if the timer is not running, 0 if it is
 *  due or overdue, else deadline - now (capped at INT_MAX). */
int gbn_poll_timeout(bool timer_running, uint64_t deadline, uint64_t now);

/* ---- Sender ---- */

typedef struct {
  const uint8_t *data;   /* whole file, borrowed (I/O layer owns it) */
  size_t data_len;
  uint32_t window, timeout_ms, num_data;
  uint32_t base, next;   /* [base, next) is in flight */
  bool timer_running;    /* the one timer */
  uint64_t deadline;
  uint32_t fruitless;    /* consecutive timeouts that did not move base */
  gbn_status status;
  uint8_t slot[GBN_MAX_WINDOW][PKT_MAX_LEN]; /* copy of packet seq at slot[seq % window] */
  size_t slot_len[GBN_MAX_WINDOW];
  uint64_t sent, retransmitted, timeouts;    /* statistics */
} gbn_sender;

/** What the I/O layer must do after a sender event. */
typedef struct {
  gbn_datagram send[GBN_MAX_WINDOW]; /* in order */
  size_t nsend;
  bool timer_running;
  uint64_t deadline;
  gbn_status status;
} gbn_sender_out;

/** Returns 0, or -1 if window is outside 1..64, timeout_ms == 0,
 *  len > GBN_MAX_FILE, or data == NULL with len > 0. */
int  gbn_sender_init(gbn_sender *s, const uint8_t *data, size_t len,
                     uint32_t window, uint32_t timeout_ms);
/** Sends the first window (or FIN 0 for an empty file) and starts the timer. */
void gbn_sender_start(gbn_sender *s, uint64_t now, gbn_sender_out *out);
/** Cumulative ACK: ignored unless the sender is running and base < ack <= next. */
void gbn_sender_on_ack(gbn_sender *s, uint32_t ack, uint64_t now, gbn_sender_out *out);
/** Decodes; ignores anything that is not a valid ACK. */
void gbn_sender_on_datagram(gbn_sender *s, const uint8_t *buf, size_t n,
                            uint64_t now, gbn_sender_out *out);
/** Fires the timeout only if the timer is running and now >= deadline. */
void gbn_sender_on_timer(gbn_sender *s, uint64_t now, gbn_sender_out *out);

/* ---- Receiver ---- */

typedef struct {
  uint32_t expected;
  bool lingering;
  uint64_t linger_until;
  uint64_t last_valid;
  gbn_status status;
  uint8_t ack[PKT_HEADER_LEN]; /* encoded ACK handed out in gbn_receiver_out.reply */
} gbn_receiver;

/** What the I/O layer must do after a receiver event, in this order:
 *  write `deliver` (if deliver_len > 0), close the file (if close_file),
 *  then send `reply` (if reply.len > 0). */
typedef struct {
  const uint8_t *deliver;  /* borrowed from the input datagram */
  size_t deliver_len;
  bool close_file;
  gbn_datagram reply;
  uint64_t deadline;       /* call gbn_receiver_on_timer at or after this */
  gbn_status status;
} gbn_receiver_out;

/** `now` is when registration succeeded: the 30 s idle clock starts there. */
void gbn_receiver_init(gbn_receiver *r, uint64_t now);
void gbn_receiver_on_datagram(gbn_receiver *r, const uint8_t *buf, size_t n,
                              uint64_t now, gbn_receiver_out *out);
/** Ends the linger (DONE) or the idle wait (FAILED) once its deadline passes. */
void gbn_receiver_on_timer(gbn_receiver *r, uint64_t now, gbn_receiver_out *out);

/* ======================================================================
 * Layer 3: I/O. The socket, relay hello, poll loop, clock and file live
 * in src/main.c and nowhere else. Declared here are only the pure helpers
 * main.c uses, so they can be unit tested.
 * ====================================================================== */

#define RELAY_DEFAULT_PORT   "4250"
#define HELLO_ATTEMPTS       5
#define HELLO_WAIT_MS        1000
#define SESSION_MAX_LEN      32u
#define PROB_MAX             0.5
#define HELLO_MAX_LEN        128u

/** 1..32 chars, each in [a-z0-9-]. */
bool valid_session(const char *s);
/** Decimal digits only (no sign, no spaces), value in [min, max], no overflow. */
bool parse_uint(const char *s, unsigned long min, unsigned long max, unsigned long *out);
/** strtod-parsed, whole string consumed, 0 <= p <= 0.5 (rejects NaN, inf, ""). */
bool parse_probability(const char *s, double *out);
/** Canonical decimal text for p in [0, 0.5]: 6 places, trailing zeros trimmed.
 *  Returns length written (NUL-terminated), or 0 if p is out of range or the
 *  text doesn't fit. */
size_t format_probability(double p, char *buf, size_t cap);
/** Build "HELLO <s> recv" / "HELLO <s> send <l> <c> <d>" (no newline, NUL-terminated).
 *  Returns length (excluding NUL), or 0 if it doesn't fit in cap (hello_send
 *  also returns 0 if a probability is outside [0, 0.5]). */
size_t hello_recv(char *buf, size_t cap, const char *session);
size_t hello_send(char *buf, size_t cap, const char *session,
                  double loss, double corrupt, double dup);
/** Classify a relay reply of n bytes: 0 = OK; 1 = ERR (reason copied, NUL-terminated,
 *  truncated to cap-1, trailing CR/LF stripped); -1 = not a hello reply.
 *  `cap` must be at least 1. */
int hello_reply(const uint8_t *buf, size_t n, char *reason, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* LAB_H */
