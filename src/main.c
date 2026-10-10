#define _POSIX_C_SOURCE 200809L
/* Layer 3: I/O. The only file that knows about the socket, the relay hello,
 * poll, the clock and the files. It reads an event, hands it to a Go-Back-N
 * state machine from lab.c, and carries out the actions that come back. */
#include "lab.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef TEST
#define main main_exclude
#endif

#define EXIT_BAD_ARGS 1
#define EXIT_NETWORK  2
#define RECV_BUF_LEN  2048 /* > PKT_MAX_LEN: oversize datagrams fail decode, never truncate */

static const char USAGE[] =
    "Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss]\n"
    "                  [-c corrupt] [-d dup] [-p port] <relay> <file>\n"
    "       myapp recv -s <session> [-p port] <relay> <file>\n"
    "\n"
    "  -s <session>     session name shared by the sender and the receiver\n"
    "  -w <window>      Go-Back-N window size in packets, 1 to 64 (default: 8)\n"
    "  -T <timeout-ms>  retransmission timeout in milliseconds (default: 250)\n"
    "  -l <loss>        probability the relay drops a packet (default: 0)\n"
    "  -c <corrupt>     probability the relay flips a bit (default: 0)\n"
    "  -d <dup>         probability the relay duplicates a packet (default: 0)\n"
    "  -p <port>        relay port (default: 4250)\n"
    "  <relay>          host name or address of the relay\n"
    "  <file>           file to send, or file to write what is received\n";

static void usage(FILE *out)
{
  fputs(USAGE, out);
}

static int usage_error(void)
{
  usage(stderr);
  return EXIT_BAD_ARGS;
}

/* Milliseconds on a clock that never jumps. */
static uint64_t now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* ---- Command line ---- */

typedef struct {
  const char *session, *port, *relay, *file;
  unsigned long window, timeout_ms;
  double loss, corrupt, dup;
} options;

/* Parses argv[2..] for `mode` (argv[1]). Returns 0, or EXIT_BAD_ARGS after
 * printing what was wrong and the usage text. */
static int parse_options(int argc, char **argv, bool sender, options *o)
{
  o->session = NULL;
  o->port = RELAY_DEFAULT_PORT;
  o->window = GBN_DEFAULT_WINDOW;
  o->timeout_ms = GBN_DEFAULT_TIMEOUT_MS;
  o->loss = o->corrupt = o->dup = 0.0;

  /* argv[1] (the mode) plays the program name for getopt. */
  const char *optstring = sender ? ":s:w:T:l:c:d:p:" : ":s:p:";
  unsigned long port;
  int opt;
  opterr = 0;
  while ((opt = getopt(argc - 1, argv + 1, optstring)) != -1)
  {
    switch (opt)
    {
    case 's':
      if (!valid_session(optarg))
      {
        fprintf(stderr, "myapp: session '%s' must be 1 to 32 of a-z, 0-9 and -\n", optarg);
        return usage_error();
      }
      o->session = optarg;
      break;
    case 'w':
      if (!parse_uint(optarg, GBN_MIN_WINDOW, GBN_MAX_WINDOW, &o->window))
      {
        fprintf(stderr, "myapp: window '%s' must be a whole number from 1 to 64\n", optarg);
        return usage_error();
      }
      break;
    case 'T':
      if (!parse_uint(optarg, 1, UINT32_MAX, &o->timeout_ms))
      {
        fprintf(stderr, "myapp: timeout '%s' must be a positive number of milliseconds\n",
                optarg);
        return usage_error();
      }
      break;
    case 'l':
    case 'c':
    case 'd':
    {
      double *p = opt == 'l' ? &o->loss : opt == 'c' ? &o->corrupt : &o->dup;
      if (!parse_probability(optarg, p))
      {
        fprintf(stderr, "myapp: -%c '%s' must be a probability from 0 to 0.5\n", opt, optarg);
        return usage_error();
      }
      break;
    }
    case 'p':
      if (!parse_uint(optarg, 1, 65535, &port))
      {
        fprintf(stderr, "myapp: port '%s' must be a number from 1 to 65535\n", optarg);
        return usage_error();
      }
      o->port = optarg;
      break;
    case ':':
      fprintf(stderr, "myapp: option -%c needs a value\n", optopt);
      return usage_error();
    default:
      fprintf(stderr, "myapp: unknown option -%c for %s\n", optopt, argv[1]);
      return usage_error();
    }
  }
  if (o->session == NULL)
  {
    fprintf(stderr, "myapp: -s <session> is required\n");
    return usage_error();
  }
  if (argc - 1 - optind != 2)
  {
    fprintf(stderr, "myapp: expected <relay> <file> after the options\n");
    return usage_error();
  }
  o->relay = argv[1 + optind];
  o->file = argv[2 + optind];
  return 0;
}

/* ---- Network ---- */

/* IPv4 only: the relay binds an AF_INET socket, and with AF_UNSPEC "localhost"
 * can resolve to ::1 first and fail as silence. */
static int resolve_relay(const char *host, const char *port, struct sockaddr_in *addr)
{
  struct addrinfo hints;
  struct addrinfo *res = NULL;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  int rc = getaddrinfo(host, port, &hints, &res);
  if (rc != 0)
  {
    fprintf(stderr, "myapp: cannot resolve relay %s:%s: %s\n", host, port, gai_strerror(rc));
    return -1;
  }
  memcpy(addr, res->ai_addr, sizeof *addr);
  freeaddrinfo(res);
  return 0;
}

static bool same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
  return a->sin_family == b->sin_family && a->sin_port == b->sin_port &&
         a->sin_addr.s_addr == b->sin_addr.s_addr;
}

/* A failed send is treated like a lost packet: the timer recovers it. */
static void send_dgram(int fd, const struct sockaddr_in *to, const uint8_t *buf, size_t len)
{
  (void)sendto(fd, buf, len, 0, (const struct sockaddr *)to, sizeof *to);
}

/* Waits up to `ms` for one datagram from the relay. Returns its length, 0 on
 * timeout or a datagram from anyone else, -1 on a poll failure. */
static ssize_t recv_from_relay(int fd, const struct sockaddr_in *relay, int ms, uint8_t *buf)
{
  struct pollfd pfd = {fd, POLLIN, 0};
  int r = poll(&pfd, 1, ms);
  if (r < 0)
  {
    if (errno == EINTR)
    {
      return 0;
    }
    perror("myapp: poll");
    return -1;
  }
  if (r == 0 || !(pfd.revents & POLLIN))
  {
    return 0;
  }
  struct sockaddr_in from;
  socklen_t fromlen = sizeof from;
  ssize_t n = recvfrom(fd, buf, RECV_BUF_LEN, 0, (struct sockaddr *)&from, &fromlen);
  if (n <= 0 || fromlen != sizeof from || !same_addr(&from, relay))
  {
    return 0; /* an empty datagram is never a packet either */
  }
  return n;
}

/* Sends the hello up to HELLO_ATTEMPTS times, waiting HELLO_WAIT_MS for each
 * reply. Returns 0 on OK, -1 (after saying why) on ERR, silence or failure. */
static int register_relay(int fd, const struct sockaddr_in *relay, const char *hello,
                          size_t len, const options *o)
{
  uint8_t buf[RECV_BUF_LEN];
  char reason[HELLO_MAX_LEN];
  for (int attempt = 0; attempt < HELLO_ATTEMPTS; attempt++)
  {
    send_dgram(fd, relay, (const uint8_t *)hello, len);
    uint64_t deadline = now_ms() + HELLO_WAIT_MS;
    for (uint64_t now = now_ms(); now < deadline; now = now_ms())
    {
      ssize_t n = recv_from_relay(fd, relay, (int)(deadline - now), buf);
      if (n < 0)
      {
        return -1;
      }
      if (n == 0)
      {
        continue;
      }
      int kind = hello_reply(buf, (size_t)n, reason, sizeof reason);
      if (kind == 0)
      {
        return 0;
      }
      if (kind == 1)
      {
        fprintf(stderr, "myapp: relay refused: %s\n", reason);
        return -1;
      }
      /* anything else is a stray datagram: keep waiting out this attempt */
    }
  }
  fprintf(stderr, "myapp: no reply from relay at %s:%s after %d attempts\n", o->relay, o->port,
          HELLO_ATTEMPTS);
  return -1;
}

/* Resolve the relay, open the one socket used for the whole run, register. */
static int connect_relay(const options *o, const char *hello, size_t len,
                         struct sockaddr_in *relay)
{
  if (resolve_relay(o->relay, o->port, relay) != 0)
  {
    return -1;
  }
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
  {
    perror("myapp: socket");
    return -1;
  }
  if (register_relay(fd, relay, hello, len, o) != 0)
  {
    close(fd);
    return -1;
  }
  return fd;
}

/* ---- Files ---- */

/* Reads the whole file into a malloc'd buffer the caller frees (also on error).
 * Returns 0, or -1 after saying why (unreadable, or larger than 16 MiB). */
static int read_file(const char *path, uint8_t **out, size_t *out_len)
{
  *out = NULL;
  *out_len = 0;
  FILE *f = fopen(path, "rb");
  if (f == NULL)
  {
    fprintf(stderr, "myapp: cannot open %s: %s\n", path, strerror(errno));
    return -1;
  }
  /* Grow 64 KiB, 128 KiB, ... up to 16 MiB + 1: filling that last byte means too big. */
  size_t cap = 0;
  size_t len = 0;
  uint8_t *buf = NULL;
  int rc = 0;
  for (;;)
  {
    if (len == cap)
    {
      if (cap > GBN_MAX_FILE)
      {
        fprintf(stderr, "myapp: %s is larger than 16 MiB\n", path);
        rc = -1;
        break;
      }
      size_t ncap = cap == 0 ? 64u * 1024u : cap * 2u;
      if (ncap > GBN_MAX_FILE + 1u)
      {
        ncap = GBN_MAX_FILE + 1u;
      }
      uint8_t *nbuf = realloc(buf, ncap);
      if (nbuf == NULL)
      {
        fprintf(stderr, "myapp: out of memory reading %s\n", path);
        rc = -1;
        break;
      }
      buf = nbuf;
      cap = ncap;
    }
    size_t n = fread(buf + len, 1, cap - len, f);
    len += n;
    if (n == 0)
    {
      if (ferror(f))
      {
        fprintf(stderr, "myapp: cannot read %s\n", path);
        rc = -1;
      }
      break;
    }
  }
  fclose(f);
  *out = buf;
  *out_len = len;
  return rc;
}

/* ---- Sender ---- */

static void transmit(int fd, const struct sockaddr_in *relay, const gbn_sender_out *out)
{
  for (size_t i = 0; i < out->nsend; i++)
  {
    send_dgram(fd, relay, out->send[i].buf, out->send[i].len);
  }
}

static int run_send(int argc, char **argv)
{
  static gbn_sender sender; /* ~66 KB of retransmission slots: not on the stack */
  options o;
  if (parse_options(argc, argv, true, &o) != 0)
  {
    return EXIT_BAD_ARGS;
  }

  int rc = EXIT_NETWORK;
  int fd = -1;
  uint8_t *data = NULL;
  size_t len = 0;
  if (read_file(o.file, &data, &len) != 0)
  {
    rc = EXIT_BAD_ARGS;
    goto cleanup;
  }
  char hello[HELLO_MAX_LEN];
  size_t hlen = hello_send(hello, sizeof hello, o.session, o.loss, o.corrupt, o.dup);
  struct sockaddr_in relay;
  fd = connect_relay(&o, hello, hlen, &relay);
  if (fd < 0)
  {
    goto cleanup;
  }
  if (gbn_sender_init(&sender, data, len, (uint32_t)o.window, (uint32_t)o.timeout_ms) != 0)
  {
    fprintf(stderr, "myapp: cannot start the sender\n");
    goto cleanup;
  }

  gbn_sender_out out;
  uint8_t buf[RECV_BUF_LEN];
  gbn_sender_start(&sender, now_ms(), &out);
  transmit(fd, &relay, &out);
  while (out.status == GBN_RUNNING)
  {
    int wait = gbn_poll_timeout(out.timer_running, out.deadline, now_ms());
    ssize_t n = recv_from_relay(fd, &relay, wait, buf);
    if (n < 0)
    {
      goto cleanup;
    }
    if (n > 0)
    {
      gbn_sender_on_datagram(&sender, buf, (size_t)n, now_ms(), &out);
      transmit(fd, &relay, &out);
    }
    /* Always check the timer, so a steady stream of ACKs cannot starve it. */
    gbn_sender_on_timer(&sender, now_ms(), &out);
    transmit(fd, &relay, &out);
  }

  if (out.status == GBN_DONE)
  {
    fprintf(stderr, "myapp: sent %zu bytes: %llu datagrams, %llu retransmitted, %llu timeouts\n",
            len, (unsigned long long)sender.sent, (unsigned long long)sender.retransmitted,
            (unsigned long long)sender.timeouts);
    rc = 0;
  }
  else
  {
    fprintf(stderr, "myapp: giving up after %u timeouts with no progress\n", GBN_MAX_TIMEOUTS);
  }

cleanup:
  if (fd >= 0)
  {
    close(fd);
  }
  free(data);
  return rc;
}

/* ---- Receiver ---- */

static int run_recv(int argc, char **argv)
{
  options o;
  if (parse_options(argc, argv, false, &o) != 0)
  {
    return EXIT_BAD_ARGS;
  }

  int rc = EXIT_NETWORK;
  int fd = -1;
  FILE *f = fopen(o.file, "wb");
  if (f == NULL)
  {
    fprintf(stderr, "myapp: cannot create %s: %s\n", o.file, strerror(errno));
    return EXIT_BAD_ARGS;
  }
  char hello[HELLO_MAX_LEN];
  size_t hlen = hello_recv(hello, sizeof hello, o.session);
  struct sockaddr_in relay;
  fd = connect_relay(&o, hello, hlen, &relay);
  if (fd < 0)
  {
    goto cleanup;
  }

  gbn_receiver receiver;
  gbn_receiver_out out;
  uint8_t buf[RECV_BUF_LEN];
  size_t received = 0;
  gbn_receiver_init(&receiver, now_ms());
  gbn_receiver_on_timer(&receiver, now_ms(), &out);
  while (out.status == GBN_RUNNING)
  {
    int wait = gbn_poll_timeout(true, out.deadline, now_ms());
    ssize_t n = recv_from_relay(fd, &relay, wait, buf);
    if (n < 0)
    {
      goto cleanup;
    }
    if (n > 0)
    {
      gbn_receiver_on_datagram(&receiver, buf, (size_t)n, now_ms(), &out);
      /* In order: write, then close, then ACK. Never ACK what was not written. */
      if (out.deliver_len > 0)
      {
        if (fwrite(out.deliver, 1, out.deliver_len, f) != out.deliver_len)
        {
          fprintf(stderr, "myapp: cannot write %s\n", o.file);
          goto cleanup;
        }
        received += out.deliver_len;
      }
      if (out.close_file)
      {
        int closed = fclose(f);
        f = NULL;
        if (closed != 0)
        {
          fprintf(stderr, "myapp: cannot finish writing %s\n", o.file);
          goto cleanup;
        }
      }
      if (out.reply.len > 0)
      {
        send_dgram(fd, &relay, out.reply.buf, out.reply.len);
      }
    }
    gbn_receiver_on_timer(&receiver, now_ms(), &out);
  }

  if (out.status == GBN_DONE)
  {
    fprintf(stderr, "myapp: received %zu bytes\n", received);
    rc = 0;
  }
  else
  {
    fprintf(stderr, "myapp: nothing valid received for %u seconds\n", GBN_IDLE_MS / 1000u);
  }

cleanup:
  if (fd >= 0)
  {
    close(fd);
  }
  if (f != NULL)
  {
    fclose(f);
  }
  return rc;
}

int main(int argc, char **argv)
{
  if (argc < 2)
  {
    usage(stdout);
    return 0;
  }
  if (strcmp(argv[1], "send") == 0)
  {
    return run_send(argc, argv);
  }
  if (strcmp(argv[1], "recv") == 0)
  {
    return run_recv(argc, argv);
  }
  if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
  {
    usage(stdout);
    return 0;
  }
  fprintf(stderr, "myapp: unknown mode '%s'\n", argv[1]);
  return usage_error();
}
