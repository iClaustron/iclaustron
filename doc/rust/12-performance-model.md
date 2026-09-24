# 12 Performance model

How latency and throughput follow from what each thread costs, so that a
change can be judged before it is built. Two tools: a handful of
operational laws that give bounds on the back of an envelope, and
`ic_model`, a discrete-event simulation of the client and the data nodes
that gives the whole curve. Both take their costs from the measurements
in chapter 06; neither replaces measuring.

## The path of a batch

```
user thread     define b ops (define_ns each, batch_ns once)
      │         one write per data node (write_ns + bytes)
      ▼
data node       receive thread: a packet, then a signal per operation
      │         execution threads: chunks of chunk_ops
      │         send thread: one write per chunk
      ▼
client          receive thread round: kevent if it slept, one read per
receive         node, route each signal, post once per user thread
      │
      ▼
user thread     woken if asleep (wake_latency_ns, wake_cpu_ns);
                complete b ops (complete_ns each), define the next
```

Every box is a server with a first-come queue. A data node thread that
has been idle longer than it spins sleeps, and work that finds it
asleep waits `node_wake_ns` for it: a lightly loaded cluster pays this
at every hand-over, a busy one not at all. What a batch pays once
(a write, a read, a round, a wake-up) is shared by its operations; what
it pays per operation is not. That split decides almost everything.

## Operational laws

**Little's law.** A user thread with `depth` batches of `b` operations
in flight, each taking `R` from send to completion, carries

    X = depth · b / R

operations a second. Deeper pipelines raise X until R starts to grow
with them, which happens when some server fills up.

**The bottleneck law.** A server with `m` threads, each operation
costing it `D` of busy time, can carry no more than `m / D`:

    X ≤ min over servers of  m_i / D_i

With the Mac calibration and batches of 300 (150 to each data node),
the servers' ceilings before cores are shared are:

| Server | m | D per operation | m / D |
|---|---|---|---|
| Data node receive threads, both nodes | 2 | `node_packet_ns / 150 + node_signal_ns` ≈ 163 ns | 12.3 M/s |
| Data node execution threads, both nodes | 8 | `node_exec_ns` 600 ns | 13.3 M/s |
| Client receive thread | 1 | `recv_ns / 150 + reply_signals · route_ns + recv_byte_ns · reply_bytes + …` ≈ 110 ns | 9 M/s at one read per node and round, more as rounds gather |
| One user thread | 1 | define + complete + writes ≈ 360 ns | 2.8 M/s |

Divide each by the stretch `f` below to get what the shared machine
allows. On the Mac `f` reaches 1.8 at eight user threads, which is what
brings 12 M/s down to the 7 to 9 measured. On Linux, chapter 06 found
the data node receive thread first: a different machine, different
ceilings.

**Cost per operation.** A client operation costs

    d = d_op + d_batch / b

with `d_op` = define + complete + routing ≈ 300 ns and `d_batch` = the
write per node, the read per node, the round, the post and the wake-up,
several microseconds. Doubling the batch halves the second term only;
past b ≈ 200 it is small, which is why the measured CPU per read stays
near 545 ns from depth 1 to depth 4.

**Wake-ups and spinning.** If replies come a mean gap `g` apart and a
thread spins `s` before it sleeps, a spin catches the next reply with
probability `1 − e^(−s/g)` (gaps taken as exponential). Each catch saves
one wake-up (`wake_cpu_ns` plus `wake_latency_ns` of delay) and each
spin costs up to `s` of CPU. Spinning pays only when `s` is short
against `g` yet `g` is short against the wake-up: with batches of 200
reads a gap is tens of microseconds, the spin misses, and it is pure
cost. That is what chapter 06 measured twice.

**Shared cores.** Threads that ask for more CPU than there are cores
run slower by the factor

    f = max(1, CPU asked for / cores)

which stretches every cost, and so lowers every server's `m / D`. A
spinning thread asks for CPU that someone else needed; the model sees
this, the laws above do not. The CPU asked for includes CPU the model
does not follow, `other_cores`: data node threads that spin while they
wait, the data node's other threads, the operating system. It is why
throughput bends gradually towards its limit instead of reaching it at
once: each thread added asks for more CPU and stretches everyone.

## The simulation: `ic_model`

`tools/ic_model` runs the path above event by event: every thread a
server with a queue, costs jittered by ±`jitter`, a warm-up discarded,
then `seconds` measured. With `cores` set it runs again with every cost
stretched by `f` until `f` settles (a mean-field view: it catches the
direction of oversubscription and understates its spikes).

```
cargo run --release -p ic_model -- show
cargo run --release -p ic_model -- sweep=depth:1,2,4,8
cargo run --release -p ic_model -- threads=8 sweep=node_recv_threads:1,2,4
cargo run --release -p ic_model -- depth=4 sweep=spin_ns:0,2000,5000,20000
```

Each line gives the rate, the median and 99th percentile batch latency,
client CPU per operation (user/system), wake-ups, reads and writes per
thousand operations, how busy the data node receive and execution
threads and the client receive threads were, how often a spin caught a
reply, the cores asked for and the stretch `f`.

### Parameters

| Group | Parameters |
|---|---|
| Load | `threads`, `depth`, `batch`, `nodes`, `receive_threads` |
| User thread | `define_ns`, `complete_ns`, `batch_ns`, `write_ns`, `write_byte_ns`, `request_bytes`, `spin_ns` |
| Client receive | `kevent_ns`, `kevent_latency_ns`, `recv_ns`, `recv_byte_ns`, `round_ns`, `route_ns`, `reply_signals`, `reply_bytes`, `post_ns` |
| Wake-up | `wake_latency_ns`, `wake_cpu_ns` |
| Network | `net_ns` (one way) |
| Data node | `node_recv_threads`, `node_packet_ns`, `node_signal_ns`, `node_exec_threads`, `node_exec_ns`, `chunk_ops`, `node_send_ns`, `node_send_op_ns`, `node_wake_ns`, `node_spin_ns` |
| Machine and run | `cores` (0: unlimited), `other_cores`, `seconds`, `warmup`, `jitter`, `seed` |

### Calibration

The client costs come from profiles (sample and perf) of `ic_bench`; the
data node costs and the cores from fitting the measured points below
with the client costs held near the profile. Fitted on 2026-09-24 on
the Mac (M5 Pro: 6 fast and 12 slower cores; two release data nodes on
the same machine; `t9` reads by primary key; model values from the
Python prototype the tool was ported from):

| Run | Measured M reads/s | Model M reads/s | Measured p50 | Model p50 |
|---|---|---|---|---|
| 1 thread, depth 1, batch 200 | 1.17 – 1.35 | 1.06 | | 161 µs |
| 1 thread, depth 2, batch 200 | 1.79 | 1.94 | | 179 µs |
| 1 thread, depth 4, batch 200 | 2.55 – 2.61 | 2.40 | | 262 µs |
| 4 threads, depth 2, batch 300 | 5.80 | 6.35 | 330 µs | 319 µs |
| 8 threads, depth 2, batch 300 | 7.36 – 8.88 | 7.19 | 553 µs | 617 µs |
| 16 threads, depth 2, batch 300 | 7.93 | 7.37 | 1077 µs | 1318 µs |

With `cores=10` and `other_cores=8`, within 14 % in rate (8 % on
average) and 22 % in latency. The fit came in three steps, and each
step's failure says something:

1. Fitted to the one-thread points alone, with `cores=4` and 400 ns a
   signal in the data node receive thread, the model matched them
   within 15 % and capped eight threads at 5 M/s. It had put into a
   per-signal cost the latency that comes from data node threads
   waking. Light load cannot tell a latency from a cost; a calibration
   needs a saturated point.
2. With `node_wake_ns`, 18 cores and the eight-thread point, it
   matched those four points, and predicted 9 M/s at four threads
   already, with a sharp knee. Four threads measured 5.8: throughput
   bends gradually, which a single saturated server does not do.
3. What bends it is CPU the model did not count. At four threads the
   bench saw 392 000 preemptions in five seconds, and a data node's
   threads spin while they wait. With that CPU as `other_cores`, and
   the fast and slow cores counted as 10, every point is within 14 %.

What the model gets right without being fitted to it:

- spinning in the user thread halves its wake-ups, adds 50 to 100 ns of
  CPU per read and gains at most a few percent in rate;
- batches of 50 → 400 cut client CPU per read steeply and raise the rate;
- reads per thousand operations fall as threads are added, the receive
  thread finding more in each read (measured 8.6 at four threads, 4.9
  at eight; the model 9.9 and 6.9).

What it gets wrong: depth 1 to depth 2 gains 1.8 times in the model and
1.4 times measured; client CPU per read rises with threads when
measured (495, 512, 538 ns at 4, 8, 16) and not in the model, which
counts CPU unstretched; and the harm of spinning is understated. Each
is a thing the model has no part for: caches (a woken thread on a cold
core, a spinning thread evicting a neighbour's lines), fast and slow
cores as such, the data node's own thread structure (a read passes a
receive, a TC, an LDM and a send thread, here merged into three
stages), and core contention as an average rather than a queue.

Predictions to check it by, at eight threads and batch 300: the data
node receive thread at close to 100 %; a second client receive thread
gains nothing; and data nodes that spin less (`other_cores` 8 → 4)
would give about 15 % more, 8.3 M/s, while ones that do not spin at all
would give up to 9.8.

To calibrate for another machine: measure depth 1, 2 and 4 with one
user thread, and 4, 8 and 16 threads at batch 300; take the client
costs from a profile; then adjust `node_signal_ns`, `node_exec_ns`,
`node_wake_ns`, `cores` and `other_cores` until the points are within
the run-to-run spread, and change the defaults' comment in
`tools/ic_model/src/main.rs` if it is the new reference.

## Using it

Ask the model before building: set the parameter that the change would
move to its new value, run the sweep that exposes it, and read which
server's utilisation reaches 1. A change to a server that is not the
bottleneck will not move the rate; a change that adds a per-batch cost
matters at small batches only; a change that adds CPU on a machine
already short of cores slows everything. Then build it and measure; if
the measurement and the model disagree by more than the calibration
error, the model is missing something, and that is worth knowing too.
