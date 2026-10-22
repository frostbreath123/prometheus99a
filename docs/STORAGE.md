# Persistence and storage (milestone M3)

Status: implemented and verified on the development host only (Windows with
MSYS2, and WSL2 Ubuntu 24.04). Nothing in this document was run on a Raspberry
Pi, on the flash storage of a real device, or through a real power failure.
Section 9 lists what is and is not verified.

## 1. What this milestone changes

Before M3 every durable write was made by the thread that needed it. The UI
thread wrote the audit record of an operator write and of an acknowledgement
and waited for the disk. The alarm journal existed only in memory. The alarm
state was saved only at a clean shutdown. The audit file grew without limit and
its lines carried only the time since boot. The shutdown could give up too
early while the comms thread was still inside a PLC call.

Now one storage worker thread owns every durable file write of the runtime:

| File | Content | How it is written |
|---|---|---|
| audit file (`--audit`) | operator writes, acknowledgements, logins, start-up and configuration records | appended, one flushed record at a time, rotated at a size cap |
| alarm journal (`<--alarms file>.journal`) | every alarm event, with a CRC per line | appended, rotated at the same cap, read back at start |
| alarm state (`--alarms`) | the state of every alarm point | replaced atomically when an alarm changes, within limits, and at shutdown |

## 2. The storage worker (`hmi_store`)

`src/hmi/hmi_store.*` is a bounded queue of 64 records of up to 704 bytes, one
worker thread, and a table of the last 128 outcomes. Nothing is allocated; the
object lives inside the runtime.

- A request is accepted or refused at once. If the queue is full, the store has
  been stopped, the target is not in use, or the record is empty or too long,
  `hmi_store_enqueue` returns false and changes nothing. A record is never
  dropped after it was accepted.
- Records are written strictly in the order they were accepted. An append is
  reported done only after the platform layer wrote and flushed it
  (`hmi_fs_append_sync`). A replace is an atomic file replacement.
- A caller can ask for a callback on the worker thread, or wait for a ticket.
  A ticket that is no longer remembered reads as unknown, never as done.
- A request to replace the alarm state file that has not been taken yet is
  replaced by a newer one (the newest content wins, one write instead of two).
- A failed write sets a visible fault flag; the next success clears it. The
  count of failures is never reset. The UI shows "STORAGE FAULT: records not
  saved" while the flag is set.
- At shutdown the worker finishes what is queued, then exits. Requests made
  after the stop are refused.
- The store lock is never held while a file is touched or a callback runs. The
  lock graph stays flat (see the ADR addendum).

### Rotation

An append target is rotated before a record that would take it past the cap.
The cap is 1 MiB and four rotated files are kept (`HMI_LOG_MAX_BYTES`,
`HMI_LOG_GENERATIONS`), so the audit file and the journal each use at most
5 MiB. The live file becomes `name.1`, older ones move up (`name.2` to
`name.4`), the oldest is replaced. The audit file then starts with a
`LOG_ROTATED generations=4` line. All of this is renames, with the directory
flushed; no data is copied. If a rotation fails the record is not written, the
fault flag is set, and the next record tries again from the start.

### Torn last line

A power cut or a kill can leave the last record unfinished. Before the first
append to a file in a run, and after any failed append, the worker looks at the
last byte of the file. If it is not a line break it writes one, so the next
record never joins the damaged one. This was found while designing the kill-loop
test (section 8); without it the BOOT marker of the next start would have been
glued onto the torn line.

## 3. Audit before effect, without blocking the UI

A write and an acknowledgement are now two-phase.

- **PLC write.** `hmi_writes_submit_as` checks the request, reserves an audit
  slot and hands the RESERVED record to the worker, then returns at once with
  the request id. The request stays RESERVED ("RECORDING (not sent yet)" on
  the screen) until the worker reports the record durable; only then it becomes
  PENDING and is queued. If the record fails, the request becomes REJECTED and
  is never sent. If it does not complete within the time to live (5 s) it
  expires, unsent. Requests that are RESERVED count against the queue of 8, so
  the queue cannot overflow when their records complete.
- **Before sending.** When the comms thread takes a request it writes the SENT
  record and waits until it is durable (at most `HMI_AUDIT_WAIT_MS`, 2 s). If
  it is not durable in time, or cannot be written, the request is REJECTED and
  not sent. This is the only place where a thread waits for the worker, and it
  is the comms thread, which already blocks on PLC calls. The records after the
  transmission (CONFIRMED, UNKNOWN and so on) are queued without waiting.
- **Acknowledgement.** The core returns `HMI_ACK_PENDING`; the acknowledgement
  is applied when its record is durable, and the same checks as before apply
  at that moment (an alarm that changed in the meantime is left
  unacknowledged). If the record fails, or does not complete within 10 s,
  nothing changes. At most four acknowledgements wait at once; another is
  refused as "audit unavailable". The UI shows "ACK PENDING: recording" and
  then the result. The record of a denied attempt is queued best effort.
- **Login.** Once the runtime is up, login, logout and lockout records go
  through the worker, so they are ordered with the rest and counted against the
  size cap. A successful login still waits until its record is durable, at
  most 2 s, on the UI thread. A login is rare, and it must not succeed
  unrecorded; this is the one place where the UI thread can still wait on the
  disk.
- Without an audit path nothing changes: writes are forced off, and
  acknowledgements work and are not recorded.

## 4. Audit record format

Every line starts with the milliseconds since boot (the order within one run)
and ends with a wall-clock stamp and the clock state:

```
<ms> BOOT wall=2026-10-22T10:15:03.120Z sync=ok
<ms> <EVENT> id=<n> tag=<i> role=<r> eng=<v> raw=0x<hex> rb=0x<hex> exc=<c> user=<u> old=<v|-> oldq=<q> wall=<time> sync=ok|no
<ms> ALARM_ACK role=<r> action=ACK|ACK_ALL result=GRANTED|DENIED n=<k> points=<names> user=<u> wall=<time> sync=ok|no
<ms> LOG_ROTATED generations=4
```

- `EVENT` is RESERVED, SENT, CONFIRMED, REJECTED, EXPIRED, UNKNOWN,
  VERIFY_FAILED or REJECTED_REVALIDATION.
- `old=` and `oldq=` are the value the operator overwrote and its quality. If
  the quality was not GOOD the value is written as `-`, because it cannot be
  trusted.
- `sync=ok` means the operating system reported the clock as set from a time
  source (on Linux, the kernel NTP status read with a read-only `adjtimex`). A
  device without a battery-backed clock can start with a wrong date; a line
  with `sync=no` must not be trusted for its date. The milliseconds since boot
  and the BOOT line still order the records. `wall=- sync=no` means the clock
  could not be read. The Windows development host always reports `sync=no`.
- Login and start-up lines (`LOGIN_OK`, `CONFIG_ACCEPTED`, ...) keep their own
  format and receive the same trailing stamp.
- Lines are produced by pure functions (`src/hmi/hmi_auditfmt.*`) that take the
  clock as an argument, and are tested against fixed dates (1970, leap days
  2000 and 2024, 2038, 2100).

### Alarm journal line

```
<ms> ALARM_EVENT seq=<n> point=<name> event=<e> kind=<k> state=<s> value=<v> wall=<time> sync=<..> crc=<8 hex digits>
```

The CRC-32 covers the line up to its own field. At start the runtime reads the
end of the newest rotated journal file and then the end of the live file (up to
48 KiB of each), and puts the last 64 valid entries into the in-memory journal,
so the screen shows
the recent events and the sequence numbers continue. A line with a wrong CRC, a
missing field, an unknown tag name, or no line break is skipped.

### Alarm state file

The file keeps its earlier format and CRC (`hmi_alarm_state_serialize` and
`hmi_alarm_state_apply` are the byte form of `hmi_alarm_save` and
`hmi_alarm_load`). It is rewritten when an alarm point changes its state or
kind, with these limits so that a flapping alarm cannot wear out the flash:

- at least 10 s between two writes (`HMI_STATE_MIN_INTERVAL_MS`);
- at most 120 writes in any hour (`HMI_STATE_SAVES_PER_HOUR`), which is at most
  2880 a day; a change that is held back is written as soon as the limits
  allow, and a clean shutdown always writes the last state;
- a failed write is retried when the limits allow.

The file is small (about 210 bytes) and is written by atomic replacement, so a
reader sees the old or the new state, never a mixture.

## 5. Write rate limit and second check

- **Rate limit.** Two optional top-level configuration fields:
  `write_min_interval_ms` (100 to 3 600 000, default 1000) is the shortest time
  between two accepted writes to the same tag, and `write_max_per_minute` (1 to
  600, default 30) is the most accepted writes in any 60 s window. A refused
  request returns "too many writes, wait" and uses up no id, audit slot or
  allowance. The limits are part of the signed configuration.
- **Second check.** Just before a queued request is sent, the comms thread
  repeats the decisions of the submission that can have changed: the master
  switch, that the tag is still writable, the role, the value range and that the
  register encoding is the one that was audited. A request that fails is
  REJECTED, never sent, and recorded as `REJECTED_REVALIDATION`. The
  configuration is immutable after start, so today this guards against a
  future change of that and against a corrupted queue entry. It does not check
  the login session: a request accepted while a session was valid may still be
  sent within its 5 s time to live after the session ended.

## 6. Shutdown and timeouts

The shutdown budget was shorter than the longest legitimate stall, so a stop
during a write could leave objects allocated. The fix has three parts:

1. `hmi_runtime_worst_stall_ms` is now the longest of the three things one comms
   step can be (connect, the SENT wait plus write plus read-back, a poll) plus
   2 s: `max(connect, 2 x response + 2000) + 2000`.
2. `hmi_runtime_stop` waits for that time, with one budget for both threads. It
   then writes the last alarm state through the worker (directly, if the queue
   is full), lets the worker finish its queue (up to 3 s, `HMI_STORE_STOP_MS`)
   and returns true only if everything stopped and was released.
3. The configuration now limits `response_timeout_ms` to 2000 and
   `connect_timeout_ms` to 3000 (they were 30 000). With these limits the worst
   stall is 8 s, below the 10 s `WatchdogSec` and, with the 3 s store budget,
   below the 15 s `TimeoutStopSec` of `deploy/prometheus99.service`. With the
   old limits a worst stall of 92 s could not fit either.

## 7. Files in the service directory

With the example unit the directory `/var/lib/prometheus99` holds `audit.log`
and `audit.log.1` to `.4`, `alarms.bin`, `alarms.bin.journal` and
`alarms.bin.journal.1` to `.4`, and the user and serial files of M1 and M2. All
are covered by `ReadWritePaths`. If a `SystemCallFilter` is added to the unit
later it must still allow `adjtimex` (read only) for the clock state.

Flash use, from the limits above and not measured: the audit file receives
three records for each accepted write (RESERVED, SENT and the final one) and
one for each acknowledgement, so at the limit of 30 writes a minute about 90
records a minute plus acknowledgements, each flushed on its own (one flush per
record; records are not grouped). Together with at most 2880 state writes a day
and the alarm events this is the bound on writes; the audit file and the
journal are each capped at 5 MiB.

## 8. Evidence (host only)

Numbers from the last full run on 2026-10-22; rerun them, do not trust this
list.

| Item | Result |
|---|---|
| Core modules, Windows (`mingw32-make test-hmi`) | 2597 checks, 0 failed (was 1492 before M3) |
| Core modules, WSL2 (also the POSIX file-mode rules and the clock state) | 2635 checks, 0 failed |
| New modules | `test_store` 454, `test_auditfmt` 61 (60 on Linux), additions to the writes, alarm, runtime, config, platform and view tests |
| View-model, end to end, golden corpus | 168 and 75 checks, 0 failed; corpus changed only as described below |
| Headless UI scenario | 89 assertions, 0 failed (both hosts) |
| Executable tests on WSL2 | `liveness`, `cli-exec`, `auth-exec`, `config-exec` pass |
| Sanitizers | AddressSanitizer with UBSan, and ThreadSanitizer, over every core module and the end-to-end test: 0 reports |
| Mutation checks | 34 mutations of the new rules; 33 caught, 1 equivalent (see below) |
| Kill-loop (`make -f Makefile.linux killloop`) | 150 rounds with a different random moment each (125 SIGKILL, 25 clean stops): the verifier passed after every round; its self test of 13 checks shows that it refuses each of the faults it is meant to catch |

**Kill-loop.** A child process runs the real runtime on the mock PLC with the
audit file, journal and state in one directory, and keeps generating alarms,
acknowledgements and writes. The script kills it with SIGKILL at a random
moment (0.3 to 2.5 s), checks the files, and starts it again on the same files.
After every kill the verifier checks that: every audit line is a complete
record of a known form with a wall-clock stamp (except an allowed number of
unfinished lines, each followed by a new run or a new file); for every write,
RESERVED comes once, SENT once and only after RESERVED, and a final record only
after the record it depends on; every journal line parses and the sequence
numbers rise; the state file is absent or valid; and every write that the mock
PLC received has a SENT record. The last point is audit before send under a
kill: the mock records a write in a flushed file at the moment the request
would leave the device. The kill-loop simulates the process dying, not the
power failing; the operating system still writes what the process handed to it.
No unfinished line appeared in these runs because the records are small and the
kernel writes them whole; the unfinished-line path is tested by the store tests,
which inject torn writes.

**Mutation checks.** Each of these was broken in turn and a test failed: the
wait for the SENT record, the completion of the RESERVED record, the rate limit
(per tag, per minute, and the window boundary), each part of the second check,
a failed record still allowing a send, the expiry of reserved requests, the
queue count of reserved requests, an acknowledgement applied after its record
failed, the pending timeout, the unseen-alarm check, the limit on pending
acknowledgements, the sink running concurrently, the rotation count, the torn
line break, coalescing, unknown tickets, queue overflow, the fault flag, the
hourly and interval limits of the state file, the retry, the journal being
written, the final state at shutdown, the timeout bounds, the journal CRC, the
clock state, the old value, and the event wake-up. One mutation was not caught
and is equivalent: dropping the partial first line of the journal tail changes
nothing, because a partial line fails its CRC.

**Golden configuration corpus.** The expected output file was regenerated.
Compared with the earlier file, with the two new fields removed from every
line, exactly one earlier case changed (case 040, `response_timeout_ms` 30000,
now rejected because of the new bound), and 18 new cases were appended.

## 9. Not verified and known limits

- **No Raspberry Pi, no flash storage, no power cut.** Whether `fsync`, the
  directory flush and the atomic rename make a record durable on the file
  system and storage of the target (SD card, eMMC, overlay, mount options) is
  unknown. A real power-cut test is part of M5. The kill-loop is not a
  substitute.
- Wear figures are bounds computed from the limits, not measurements. Each
  record is flushed on its own; grouping several queued records into one flush
  would reduce the number of flushes and is not done.
- The clock state comes from the kernel and was read on WSL2 only. Behaviour
  with a real NTP client, a clock that steps, or a device without a clock was
  not tested.
- A login still waits for its record on the UI thread, for at most 2 s.
- The journal is reloaded from the live file and the newest rotated file only,
  and only the last 64 entries are kept.
- Records still in the queue when the process is killed or the power fails are
  lost: at most 64 queued records, and, for an operator action, nothing that had
  an effect. A PLC write is never sent before its SENT record is durable, and an
  acknowledgement is never applied before its record is durable. The final
  record of a write (CONFIRMED and so on) can be lost, which leaves a SENT
  without an outcome. The state file can be one change older than the last
  alarm change (up to the interval limit) after a crash.
- The state-write limits count writes queued, not writes the disk completed.
- The sanitizers were run on the core modules and the end-to-end test, not on
  the LVGL harness, the full executable, or the kill-loop processes.
- The second check does not re-check the login session (section 5).

## 10. Documentation conflicts found while doing M3

1. The earlier ADR text says an acknowledgement is recorded before it takes
   effect and is refused if it cannot be recorded. That still holds, but the
   call now returns `HMI_ACK_PENDING` and the effect and the outcome come later.
   The ADR addendum records this.
2. The roadmap says the audit record is written "via the storage worker" and
   that the UI thread must not block. A login still waits for its record
   (section 3).
3. The roadmap says "all write-safety tests unchanged". The existing write tests
   (280 checks) are unchanged and pass. The runtime and UI tests were changed
   because acknowledgement became two-phase and the line format gained the
   trailing stamp.
4. The configuration bounds for the two PLC timeouts were tightened (section 6).
   This changes what the configuration accepts, which the roadmap did not list.
5. The roadmap lists row 42 (journald logging, trace ring) under M3 without a
   task. Only the storage fault flag and counters were added; the trace ring
   is not built.
