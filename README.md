# P2 - Reliable Data Transfer

- Name: Jake Dickinson
- Email: jakedickinson@u.boisestate.edu
- Class: CS425-001

## Known Bugs or Issues

No known bugs. Every transfer in testing arrived byte-identical. The notes below
cover one expected protocol behavior and the choices I made where the assignment
does not say what to do.

- **Rare give-up at the very end under heavy damage (expected behavior).** At
  20% loss, 20% corruption and 20% duplication, a FIN/ACK round trip succeeds only
  about 41% of the time. Now and then every repeated FIN or its ACK is lost for 10
  timeouts in a row. The receiver then has the whole file and exits 0 after its
  2-second linger, but the sender gives up and exits 2, exactly as Task 3
  specifies. I measured this live in 2 of 81 empty-file transfers at 20/20/20.
  It also happened in one in-memory test case (seed 42; see Testing). It never
  happened at the Task 6 settings.
- **Exit code 1 for file problems.** The assignment does not give an exit code for
  an input file that cannot be read, an output file that cannot be created, or an
  input larger than 16 MiB. I treat all three as a bad command line and exit 1.
  The output file is created before registering with the relay, so if
  registration then fails, an empty output file is left behind.
- **IPv4 only.** `<relay>` is resolved with `getaddrinfo` restricted to `AF_INET`.
  The relay binds an IPv4 socket, and with `AF_UNSPEC` the name `localhost` can
  resolve to `::1` first, which shows up only as five unanswered hellos.
- **Lenient parsing.** Probabilities are parsed with `strtod`, so `.1` and `1e-2`
  are accepted. The hello always re-sends them as plain decimals (`0.1`, `0.01`),
  because the relay rejects exponent notation. `-T` accepts 1 to 4294967295 ms.
  Options must come before `<relay> <file>`. Because `_POSIX_C_SOURCE` is
  defined, glibc's `getopt` follows strict POSIX rules and does not reorder
  arguments, so it behaves the same on Codespaces and Onyx.
- **Extra output.** On success each side prints a one-line summary to stderr,
  for example `myapp: sent 1048576 bytes: 1025 datagrams, 0 retransmitted, 0 timeouts`.

## Experience

For me this assignment was a powerful showcase of AI first development. The way in
which I went about solving this problem was first I made a new directory for this
project, then I cloned the repository into my CS 425 repos, and I added the relay.py
into the folder with claude in it. After that I started my claude session, gave it access,
to the file path with the repo in it and started architecting the plan for the project. I
almost always start off projects is I load my most powerful model that is included with my
claude pro, which in this case was Opus 5.5 on High effort and walked through the creation
of an architecture with it. I always find that if you spend the extra time at the start
making sure the model understands the problem, knows the hard constraints that it has to
follow, understands the dependencies it has to work with and systems it is expected to run on
then you are able to get some really great results from it in the planning phase! I would say that
I spent about an hour fine tuning how I wanted that architecture to work and then I had the model
commit the doc with the architecture along with any other important pieces of context to its memory
and then make me a hand off prompt to pass into a new session that will actually end up coding the
project based on the architecture. I cleared my context and started a new session with Opus 5.5 on
High effort using the hand off prompt and architecture doc that I had made so the code could start
being generated. I made sure that the model would ask me for clarification if it was ever unsure or
ran into a hard problem plus I had it stop periodically at different milestones so I could look things
over before we continued any further in the project. Overall the model did not run into many issues,
just 1 or 2 bugs that it was able to solve in a matter of minutes. Overall I would say that the code
generation phase took about 2 hours then when it was verifying the functionality of the code at the end
which it predicted would take about 15 minutes, I went off and got some lunch. Once I came back home I did
one final look over of the code. Made sure that it compiled cleanly on codespaces and on onyx, wrote the
summary that you are reading right now, and committed all my changes.

I really have enjoyed learning more about how the CLI tools for AI models work! This was not a tool I
leveraged properly in the past and I am now starting to see how freakishly powerful it is in this form
even when compared to its highest level in the GUI based application. I think the next thing I would like
to learn about is how to break my architecture into pieces that I can farm out to agents instead of having
one model pounds away on it linearly. I think this would help me save tokens and time if I can get some
parallel work going with smaller context windows. I also throughout this really started to think about
how I want to start saving memory for these models and started figuring out how I would personally want to
incorporate an obsidian brain into my CLI workflow. That is going to be a project I work on in my own time
certainly but this project in particular did a great job sparking that interest. I think this was a well written
assignment and I do think I learned a ton about how AI driven development works and how to get some
quality results out of the models that you are using!

## Design

The program is split into the three layers from Task 4. `src/lab.h` declares
them in three labeled sections.

| Layer | Lives in | What it does |
|---|---|---|
| 1. Packets | `src/lab.c` | `pkt_checksum` (RFC 1071), `pkt_encode`, `pkt_decode`. Pure: bytes in, bytes or a struct out. |
| 2. Go-Back-N state machines | `src/lab.c` | `gbn_sender_*` and `gbn_receiver_*`. Each event function (start, ACK or datagram arrived, timer) takes the current time `now` in milliseconds as a parameter and fills an *action struct*: which datagrams to send, what payload to deliver, whether to close the file, when the timer is due, and the status (running, done, failed). |
| 3. I/O | `src/main.c` | `getopt`, `getaddrinfo`, the one UDP socket, the relay hello with retries, the `poll` loop, `clock_gettime(CLOCK_MONOTONIC)`, and reading and writing files. It reads an event, hands it to layer 2, and carries out the returned actions. |

**Why.** `src/lab.c` never calls `socket`, `sendto`, `recvfrom`, `poll`,
`clock_gettime`, `fopen`, `fread`, `fwrite`, `fclose`, `getaddrinfo`, `printf` or
`malloc` (checked with grep). That makes the protocol fully deterministic under
test. A timeout is just a bigger number for `now`, and a lost packet is just a
datagram the test does not pass along. Because of that, the unit tests cover
100% of the lines and branches of the protocol logic, and the whole transfer can
be run thousands of times in memory under heavy damage. `src/main.c` stays thin
(read an event, call layer 2, act), so it is easy to reason about. The command
line and hello logic that could be pure (`valid_session`, `parse_uint`,
`parse_probability`, `format_probability`, `hello_recv`, `hello_send`,
`hello_reply`, `gbn_poll_timeout`) is also in `lab.c`, where it is tested.

**Packets.** Fields are written one at a time at fixed offsets with
`htons`/`htonl`; a struct is never copied onto the wire. `pkt_decode` checks
the size first (`n >= 10`, `10 + length == n`, `n <= 1034`, which together imply
`length <= 1024`), then the type, the reserved byte and the checksum. It never
reads a payload byte until the length is known to match the datagram size. To
verify the checksum, the whole datagram is summed with the checksum in place; an
intact packet gives 0.

**Sender.** It keeps `base`, `next`, one timer (`timer_running`, `deadline`) and
a count of fruitless timeouts. The copy of each in-flight packet lives in a fixed
slot, `slot[seq % window]`, inside the `gbn_sender` struct (64 × 1034 bytes).
Nothing in the protocol code allocates memory, so it cannot leak, and there is no
allocation-failure branch that coverage would force into an exclusion. The window
is filled while `next < base + window`. The FIN goes only once `base == num_data`,
and is then handled like any other packet. An ACK in `(base, next]` sets
`base = ack` (cumulative), resets the fruitless count, and restarts or stops the
timer. Any other ACK is ignored. A timeout resends every slot in `[base, next)`
in order. The tenth fruitless timeout in a row sets the status to failed.

**Receiver.** It keeps `expected`, plus the linger and idle deadlines. In-order
DATA is delivered and `expected` increases. Every valid DATA or FIN is answered
with `ACK expected`, including duplicates and packets from beyond a gap, which
are discarded and never buffered. The in-order FIN closes the file and starts the
2-second linger, during which repeated FINs are re-ACKed. Invalid datagrams get
no reply and do not reset the 30-second idle timer.

**I/O loop.** There is one `SOCK_DGRAM` socket per process, used for the hello and
all data, so the relay always sees the same address and port. Datagrams that do
not come from the relay's address are dropped. The receive buffer is 2048 bytes,
larger than the biggest packet, so an oversized datagram fails validation instead
of being silently truncated. The loop calls `poll` with
`gbn_poll_timeout(...)`, the time left on the timer from `CLOCK_MONOTONIC`. After
every wakeup it hands any datagram to the state machine and then always calls
the timer function, so a steady stream of duplicate ACKs cannot starve a
timeout. The receiver carries out its actions in a fixed order: write the
payload, close the file if told to, then send the ACK. It never ACKs data it
failed to write. The only heap allocation in the program is the input file
buffer, which is freed on every exit path.

**Testing.** `tests/lab-test.c` has 50 Unity tests covering every function in
`lab.h`:

- the golden vectors (RFC 1071 `0x220d`, the "Hi!" packet, ACK 3), and every
  single-bit flip of several packets being rejected;
- short, mismatched-length, unknown-type and reserved-byte datagrams;
- receiver cases: duplicates, gaps, repeated FIN, linger, idle timeout;
- sender cases: window full, cumulative ACK, duplicate ACKs, resending the whole
  window, giving up after 10 timeouts, an empty file, an exact multiple of 1024;
- a seeded end-to-end transfer.

The end-to-end test runs both state machines through an in-memory channel that
drops, corrupts and duplicates 20% of the datagrams in each direction, using a
deterministic splitmix64 generator. It covers 5 seeds × windows 4, 8 and 16 ×
files of 0, 1, 1024 and 50000 bytes, plus window-1 and clean-channel cases, and
checks that the delivered bytes equal the sent bytes. Seed 42 was replaced by
43 after a trace showed the legitimate FIN-phase give-up described in Known
Bugs (the trace is summarized in a comment in the test file). `make report`
shows 100% line and 100% branch coverage of `src/lab.c`, with no exclusion
markers.

## Results

**Setup.** WSL2 (Ubuntu 24.04, gcc 13.3, Python 3.12) on Windows 10. The relay
was started with `--delay 50` on 127.0.0.1. The sender used the default 250 ms
timeout. The file was a 1 MiB file from `/dev/urandom`, which is 1024 DATA
packets plus a FIN, or 1025 packets. Each combination was run 3 times, the
sender's run time was measured with bash `time`, and every copy was checked with
`cmp`. All 12 copies were identical and both sides exited 0. The measurements
were made with `scripts/bench.sh`, and the raw output is in
`results/bench-20261010-115231.txt`. The relay's per-session counts are in
`results/relay-20261010-115231.log`.

| Window | Loss | Run 1 (s) | Run 2 (s) | Run 3 (s) | Mean (s) | Throughput (KiB/s) |
|---|---|---|---|---|---|---|
| 1 | 0 | 102.99 | 103.00 | 103.03 | 103.01 | 9.9 |
| 16 | 0 | 6.55 | 6.55 | 6.55 | 6.55 | 156.3 |
| 1 | 0.05 | 130.34 | 133.32 | 129.82 | 131.16 | 7.8 |
| 16 | 0.05 | 23.54 | 20.32 | 20.52 | 21.46 | 47.7 |

Throughput = 1024 KiB / mean time. Sender counters (means of the 3 runs) were:
window 1 at 5% loss, 112 timeouts and 112 retransmissions; window 16 at 5% loss,
49 timeouts and 784 retransmissions.

**Round trip time.** With window 1 and no loss, the sender waits one round trip
per packet, so RTT ≈ 103.01 s / 1025 = **100.5 ms**. The relay's delay accounts
for 100 ms (50 ms each way), so only about **0.5 ms per round trip** (0.5%) comes
from anything else. That remainder comes from:

- Each round trip crosses the loopback network stack four times: sender → relay
  → receiver → relay → sender.
- The relay is a single Python process. It handles each datagram in interpreted
  code, keeps it in a heap, and has to wake up from its event loop when the
  datagram is due, which adds scheduling latency on top of the exact 50 ms.
- Each side has its own `poll` wakeup, checksum, and system calls. The receiver
  also writes to the file before it ACKs.
- A few milliseconds of process start and hello registration are spread over the
  whole run.

**Speedup at window 16.** 103.01 s / 6.55 s = **15.7×**, very close to 16. With 16
packets in flight, every ACK releases the next packet, so the 1024 DATA packets
take 1024 / 16 = 64 round trips. The FIN is only sent after every DATA packet is
ACKed, so it adds one more stop-and-wait round trip, for 65 round trips. That
gives 65 × 100.5 ms = 6.53 s, and the measured 6.55 s is 65.2 round trips. So the
best possible speedup is 1025 / 65 ≈ 15.8×, not 16. What is left after that comes
from:

- filling the pipe at the start;
- the per-packet processing time of the relay, which forwards every datagram
  through one process;
- startup and the hello, which are a larger share of a 6.5 s run than of a 103 s
  run.

The speedup is close to 16 because the 100 ms round trip is far larger than the
time it takes to process a packet.

**Why 5% loss hurts window 16 much more.** At window 1, 5% loss makes the run
27% slower (+28.2 s). At window 16 it makes the run 228% slower: 3.3× the time,
+14.9 s. What a timeout costs differs between the two cases.

- **Window 1.** A lost DATA packet or a lost ACK costs exactly one timeout. The
  sender waits 250 ms instead of the 100 ms round trip, resends that **one**
  packet, and loses about 250 ms. The relay dropped an average of 56 DATA packets
  and 56 ACKs per run, and the sender saw exactly 112 timeouts. 112 × 0.25 s =
  28 s, which matches the measured +28.2 s. Each loss costs about 2.5 packet
  times against a baseline of 100 ms per packet.
- **Window 16.** A lost DATA packet stalls the whole pipeline. The receiver throws
  away everything after the gap and keeps repeating the same ACK, which does not
  move `base`. The sender fills its window, then sits idle until the timer
  expires. It then **goes back and resends all 16 packets**, about 15 of which
  the receiver already had and discarded. The run averaged 784 retransmissions
  for 49 timeouts, which is 16 per timeout and adds 77% more traffic. Each of
  those resent packets can be lost again, causing yet another timeout. Each
  timeout cost about 0.30 s (the 250 ms timer plus a round trip to refill the
  pipe). At full speed a packet takes only 100.5 ms / 16 ≈ 6.3 ms, so one loss
  wipes out about 48 packets' worth of progress, compared with 2.5 at window 1.
  With 16 packets per round trip, 1 − 0.95¹⁶ ≈ 56% of windows contain at least
  one lost DATA packet. That is why 49 timeouts nearly tripled a run that is only
  65 round trips long.
- **Lost ACKs behave differently.** At window 16 lost ACKs are almost free,
  because cumulative ACKs cover them: the relay dropped about 84 ACKs per run,
  and a later ACK almost always made up for them. At window 1 every lost ACK costs
  a full timeout. So window 16 has fewer timeouts (49 compared with 112), but each
  one costs about 20 times as much relative to how fast the protocol otherwise
  runs.

To reproduce: `make release`, then `bash scripts/bench.sh`. Set `RELAY=<path to
cs425_relay.py>` if the relay is somewhere else, or `PORT=<n>` on a shared
machine like Onyx. It takes about 15 minutes.
