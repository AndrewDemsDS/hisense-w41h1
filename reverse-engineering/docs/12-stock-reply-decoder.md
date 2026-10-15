# Stock firmware: how a received bus frame is accepted or rejected

The complete set of rules by which the vendor module takes a frame off the RS-485 bus, decides
it is the reply to the request it just sent, and drops it otherwise. It exists so that this
project's driver can drop the same frames for the same reasons and count errors the same way.
The last section compares the driver and the Python decoder against these rules.

Read [`03-rs485-ac-protocol.md`](03-rs485-ac-protocol.md) first for the envelope and
[`10-stock-fw-init-and-comms.md`](10-stock-fw-init-and-comms.md) sections 3 and 4 for where
the transaction primitive sits in the link state machine.

## Status of this page

Static analysis of `dumps/stock-w41h1-2026-08-19.bin` (slot fw1, not in the repository), done
2026-10-15 with rizin 0.8.2 (Thumb, cortex) through the REA MCP server. Addresses are file
offsets in that dump; the image is mapped at XIP = file + `0x9b6dc000`. They do not match the
addresses in doc 10, which describe an older dump; the function names and the structure in doc 10
still hold.

Every claim is tagged. **[PROVEN]** means read from the code at the offset cited. **[INFERRED]**
means deduced from it. Nothing on this page was observed on a running module. The raw listings
are in the evidence note kept with the dump (`dumps/reply-decoder-rules-analysis.md`, gitignored).

## 1. Receive side

### 1.1 Who does what

| Function | File offset | Role |
|---|---|---|
| RX byte handler | `0x247d6` | Called with one received byte in `r0`. Assembles a frame, removes the stuffing, signals completion |
| `uart_rx_start` | `0x2488c` | Arms the receiver before a request goes out |
| `uart_rx_end` | `0x2491c` | Disarms it when the window closes |
| send and wait | `0x2509c` | Sends the request, waits for the semaphore, hands out the completed frame. Logs as `dev_ota_uart_process_cmd` |
| transaction wrapper | `0x20cc4` | Wraps, sends, decodes, verifies the echo (doc 10 §4.2) |
| reply decoder | `0x20768` | Validates the frame and extracts the inner payload. Logs as `device_package_get_cmd` |

The names come from the function-name strings each one passes to the logger. **[PROVEN]**

The caller of the byte handler is not in this image: no `bl` reaches `0x247d6` in the fw1 slot,
and neither its Thumb address as a pool word nor a `movw` of its low half occurs anywhere in the
4 MB dump. Doc 10 §4.3 placed the UART interrupt handler in the other flash bank. That the
handler is called once per received byte from that interrupt, with nothing filtered before it,
is **[INFERRED]** from its shape (one byte in `r0`, no loop, a semaphore give from inside it).

### 1.2 The byte handler `0x247d6` **[PROVEN]**

State lives in RAM: a state byte (`0x1000ae98`: 0 idle, 1 armed, 2 inside a frame, 3 frame
complete), a "last byte was F4" flag (`0x1000a854`), the assembly buffer (`0x1000ac91`) with its
write index (`0x1000ae94`), the completed-frame buffer (`0x1000a85c`, 512 bytes) with its length
(`0x1000a858`), and the semaphore (`0x1000a830`). A separate 512-byte raw log (`0x1000aa5c`,
count at `0x1000ac5c`) records every byte for the `CMDRESP` hex dump that `uart_rx_end` prints at
log level 5 and above.

For each byte `b`:

1. If the raw log holds fewer than 512 bytes, append `b` to it (`0x247d8`).
2. If the previous byte was `F4` (`0x247ec`), clear the flag, then:
   - `b == F5`: write `F4 F5` to the start of the assembly buffer, index = 2, state = 2
     (`0x247f8`). This happens whatever the state was, including inside a frame or after a
     completed one: a start tag always restarts assembly.
   - `b == FB`: append `F4 FB` at the index, state = 3, copy index + 2 bytes to the
     completed-frame buffer, store that length, reset the index to 0, give the semaphore
     (`0x2481a` to `0x2483e`). The frame is complete on `F4 FB` alone: no length, no direction,
     no checksum is looked at here.
   - anything else: fall through to step 4 with `b`. The `F4` that preceded it is **not** stored.
     So `F4 F4` becomes one `F4` (the stuffing removal), and `F4 xx` for any other `xx` loses the
     `F4` silently.
3. Else if `b == F4`: set the flag, store nothing (`0x24844`).
4. Else (`0x2484e`): only when state == 2, and only when the index is below `0x1FE` (510),
   store `b` at the index and advance. Bytes outside a frame are dropped, and so are bytes beyond
   510: the frame is then closed by the next `F4 FB` with whatever fitted. A frame of at most 512
   bytes reaches the task.

The handler returns 1 when the state is 2 or 3, else 0 (`0x24808`); nothing in the image reads
that value.

### 1.3 Arming and disarming **[PROVEN]**

`uart_rx_start` (`0x2488c`) logs "uart status err, now status %d" if the state is neither 0 nor
3, then sets the state to 1 and clears the F4 flag. It does not clear the write index, the
completed-frame length or the semaphore. `uart_rx_end` (`0x2491c`) logs the same complaint if the
state is not 3, sets the state to 0, and prints and clears the raw log.

That a state of 3 is accepted silently at the next `uart_rx_start` while every other non-zero
state is logged says a frame completing between windows is a normal event to this code.
**[INFERRED]**

### 1.4 Send and wait `0x2509c` **[PROVEN]**

`int send_and_wait(u8 *frame, int len, u8 **rx_out, int *rx_len, u8 mode, int timeout_ms)`:

1. `uart_rx_start`.
2. If `mode == 0`, the bytes go out raw through `0x249e4`. Otherwise they are logged as
   `DEVWRITE` and sent through `0x22a5c`. (`0x249e4` doubles every `F4` between index 2 and
   `len - 3` inclusive, helper `0x249b8`: the start and end tags are never doubled.)
3. `xSemaphoreTake(rx_sem, timeout_ms)` (`0x250ca`). Success gives status 0. A timeout logs
   "uart_process_cmd no response" (line 480) and gives status 7.
4. `uart_rx_end`.
5. If the completed-frame length is above 0, `malloc` that many bytes, copy the completed frame
   into them, return the pointer and the length to the caller and zero the stored length
   (`0x25122` to `0x2513c`). If the length is 0, the status becomes 7 even when the semaphore
   was taken (`0x25144`).
6. Return the status.

What follows from this:

- Exactly one frame per window. The first `F4 FB` after `uart_rx_start` ends the wait. A
  second frame that arrives in the same window is not seen by this transaction: the byte handler
  overwrites the completed-frame buffer and sets its length and the semaphore again, and that is
  what the **next** transaction picks up. **[PROVEN for the buffer and semaphore writes, INFERRED
  for the ordering, which depends on when the task runs after the give.]**
- Bytes after the window are kept. `uart_rx_end` only changes the state byte, and
  a later `F4 F5` puts the handler back into state 2 regardless of that byte. A reply that arrives
  after the 500 ms deadline is assembled, stored and signalled, and the next `send_and_wait`
  returns it at once as the reply to its own request. Nothing drains the semaphore or the stored
  length before a request goes out. Whether the interrupt still calls the handler while the state
  is 0 is the one thing this image does not show (§1.1). **[PROVEN for the handler, INFERRED for
  the interrupt]**
- The transaction wrapper treats status 7 as failure and returns 0 without freeing the copy it
  may have been given (`0x20d02`, branch to `0x20d66`), a leak that only happens when a frame
  completes between the semaphore timing out and `uart_rx_end`. **[PROVEN]**
- The variant `0x24fd8`, used by the ProductType and device-ID routines, is the same with a
  3000 ms window (`0xbb8`), one extra zero byte after the copy, and a `memset` of the 512-byte
  completed-frame buffer afterwards.

### 1.5 The transaction wrapper `0x20cc4` **[PROVEN]**

As doc 10 §4.2, confirmed on this dump: wrap (`0x2037c`), `send_and_wait` with the caller's mode
and timeout, free the wrapped frame, return 0 on any non-zero status. Then the session-id capture
(`frame[9]`, `frame[10]` into the two globals, first reply only), then the decoder. The decoder's
return value `n` is the inner length or a negative code. If `n > 0` and the caller gave a buffer,
`n` inner bytes are copied into it. Then, if the caller asked for verification and passed a
payload: `n > 2`, `memcmp(payload, inner, 2) == 0`, `inner[2] == 1`, otherwise the result becomes
0 with no log line (`0x20d40` to `0x20d58`). Without verification a negative decoder code is
returned as is; every caller tests for `<= 0`.

## 2. The reply decoder `0x20768` **[PROVEN]**

`int decode(u8 *frame, int len, u8 **inner_out)`. `len` is the number of bytes the byte handler
delivered. The checks run in this order, and the first failure returns:

| # | Check | On failure: log line (all prefixed `aircond get cmd`) | Returns |
|---|---|---|---|
| 1 | `frame != NULL`, `len > 3`, `frame[0] == F4`, `frame[1] == F5`, `inner_out != NULL` | `error: invalid frame` (line 1523) | -1 |
| 2 | `frame[2] == 01` | `link resp error` (line 1537) | -2 |
| 3 | `F4 FB` at the position LEN gives (below) | `error: no end tag` (line 1563) | -3 |
| 4 | checksum over bytes 2 to `4 + LEN` equals the stored one | `crc error` (line 1583) | -4 |
| 5 | `frame[5] == 01` | `net resp error` (line 1590) | -5 |
| 6 | `frame[11] == 01` (with `frame[6] == 00`, see below) | `trans resp error` (line 1607) | -6 |

Each log line appears only when the log level byte at `0x100009b4` is above 4. Nothing is counted:
the module keeps no error counters on this path. The decoder never compares the class byte; the
class match is the wrapper's echo check (§1.5), and the only transaction without it is the DevType
probe, whose caller looks at `inner[0]` itself for `0x1D` and `0x0A` (doc 10 §4.1).

### 2.1 LEN, checksum width and the end tag

Byte 3 is CTRL, and the decoder honours all of its documented bits (`0x207fa` to `0x20824`):

- bit 0 set: one extra byte sits before LEN, so LEN is at byte 5 instead of 4.
- bit 5 set: LEN is a 16-bit big-endian value in two bytes.
- bits 6 and 7 select the checksum width through helper `0x1e39c`: `00` 1 byte, `40` 2 bytes,
  `80` 2 bytes, `C0` 4 bytes.

With CTRL `40`, the only value seen on the wire, LEN is byte 4 and the width is 2. Then:

- the checksum is read at byte `5 + LEN` (two bytes, high first), `0x20830` and `0x2083a`;
- the end tag has to be at bytes `7 + LEN` and `8 + LEN` (`0x2083e` to `0x2084e`). This is the
  **LEN + 9 rule**: a frame is LEN + 9 bytes long because the decoder looks for `F4 FB` there;
- the sum runs over bytes `2` to `4 + LEN` inclusive, LEN + 3 bytes, `ldrb r2, [r3, 1]!; add r1,
  r2` at `0x208d4`, and is compared on 16 bits after a `rev16` of the stored value (`0x208e2`).
  A 1-byte width compares the low 8 bits, a 4-byte width the full 32-bit sum against a `rev`
  of the stored word.

`len` plays no part after check 1. A frame whose LEN says it is longer than what arrived is read
past the end of its copy; the bytes found there are almost never `F4 FB`, so the practical
outcome is -3. A frame whose LEN says it is shorter fails check 3 on its own data bytes.
**[PROVEN for the reads, INFERRED for the outcome of the over-read]**

### 2.2 The sub-header: "net resp" and "trans resp"

After the checksum, the decoder walks the bytes between LEN and the class byte with pointer
arithmetic, which is what the three error strings name:

- **link resp** is byte 2, the direction byte, already checked as `01` (check 2).
- **net resp** is the byte after LEN, byte 5 (`ldrb r3, [r7, 1]` at `0x208fc` with `r7` pointing
  at LEN). It has to be `01`.
- **trans resp** is found by skipping a block whose size byte 6 encodes: helper `0x1e3fc` maps
  bits 3 and 4 of byte 6 to a count (0 → 2, 1 → 4, 2 → 6, 3 → 8) and multiplies it by 2, or by 5
  when bit 5 is set; add one more byte when bit 0 of byte 6 is set, plus one. With byte 6 equal
  to `00` that is 5, so the trans byte is byte 11, and it has to be `01` (`0x20934` to
  `0x20952`).
- The inner payload starts after the trans byte plus a step that byte 12 encodes (`0x20986` to
  `0x20998`): 1 byte, or 3 bytes when the high nibble of byte 12 is non-zero; then 1 more byte,
  or 2 when bit 0 of byte 12 is set. With byte 12 equal to `00` the inner payload starts at
  **byte 13**, the class byte.

Every reply captured so far has `01` at byte 5, `00` at byte 6, `01` at byte 11 and `00` at byte
12 (doc 03, the status and device-type replies), so the inner payload is at byte 13 in all of
them. The bytes 7 to 10 (`FE 01` and the two bytes the session capture reads) are not checked.

### 2.3 What the caller gets on success

Inner length `n = (5 + LEN) - 13 = LEN - 8` for the usual sub-header. The decoder mallocs `n`
bytes, zeroes them, copies `frame[13 .. 13 + n)` into them, stores the pointer in `*inner_out` and
returns `n` (`0x2099a` to `0x209b2`). `inner[0]` is the class byte, `inner[1]` the sub-type,
`inner[2]` the ack byte the wrapper demands to be `01`. The handlers in doc 10 §5 index this
buffer, which is why their offsets are frame byte minus 13. A LEN below 8 gives `n <= 0`, which
every caller treats as failure (`malloc` is still called with that value).

## 3. Rule summary

What the stock module needs before a received frame counts as the reply:

1. It is the **first** `F4 FB`-terminated byte run after the request went out, within the window
   (500 ms on the main path, 3000 ms for ProductType, device-ID and the slave enumerator). Any
   bytes outside `F4 F5 .. F4 FB` are dropped. Doubled `F4` collapses; a lone `F4` before any
   other byte vanishes; `F4 F5` anywhere restarts the frame.
2. `F4 F5` at 0 and 1, more than 3 bytes, `01` at byte 2.
3. `F4 FB` at bytes LEN + 7 and LEN + 8 (CTRL `40`), and the 16-bit sum of bytes 2 to LEN + 4
   matches bytes LEN + 5 and LEN + 6.
4. `01` at byte 5 and `01` at byte 11 (with byte 6 and byte 12 zero).
5. For every transaction but the DevType probe: the inner payload is longer than 2 bytes, its
   first two bytes equal the request's first two payload bytes (class and sub-type), and its third
   byte is `01`.

Any failure makes the transaction return failure, without a retry inside the primitive; the
callers decide (doc 10 §4.1, and one retry for class `0x67`). There is no counter for any of it.
What a failure means to the state machine is the same whatever the reason: a timeout, a
checksum error and a reply of the wrong class all return 0 to `uart_ctl_process_main`.

## 4. The driver and the Python decoder against these rules

Sources read: `firmware/esphome/components/hisense_ac/hisense_protocol.cpp` (`FrameAssembler`,
`frame_progress`, `status_checksum_ok`), `hisense_bus.cpp` (`listen_`, `frame_received_`,
`consume_`) and `reverse-engineering/tools/decode_ac_frames.py` (`split_frames`, `report`). The
driver's counters are `checksum_errors` (`chk_mismatch_`), `reply_timeouts` and `link_losses`
(five unanswered status polls in a row).

### 4.1 Where they agree

- Start tag, end tag, `F4 F4` to `F4`, the checksum range `[2, LEN + 5)` and its 16-bit
  big-endian compare, the `LEN + 9` length, the class at byte 13, a 500 ms window on the main
  path.
- A frame with a bad checksum is treated as no reply by both: stock returns failure; the driver
  sets `reply_sound_` false, which makes `consume_` return false and `link_miss_` advance as for
  a timeout. The driver additionally counts it in `checksum_errors`; stock counts nothing.

### 4.2 Driver accepts, stock rejects

| Case | Stock | Driver |
|---|---|---|
| Byte 5 not `01`, or byte 11 not `01` | `net resp error` / `trans resp error`, failure | not checked, frame accepted |
| Byte 6 or byte 12 non-zero (sub-header extension bytes) | inner payload moves to a later offset | class is always read at byte 13 |
| Reply class matches but sub-type (byte 14) does not, for example a `66 00` status frame as the answer to a `66 40` ProductType request | echo check fails on the second byte, transaction returns 0 | `reply_class_ok` compares byte 13 only: the status frame ends the ProductType transaction and is consumed as a status |
| Ack byte 15 not `01` on a verified transaction | echo check fails, returns 0 | not required; `command_reply_verdict` logs it for commands only |
| Any class as the answer to the heartbeat, the `07` request or a command (`CLASS_ANY`) | the heartbeat and `07` are verified: a `0x66` answer to a `0x1E` returns 0 and `handle_num30_cmd_result` does not run | accepted and consumed |
| A frame with CTRL `00` (1-byte checksum, LEN + 8 long) or CTRL `C0` (4-byte, LEN + 11 long) | accepted with the matching width | `frame_progress` expects the end tag at LEN + 9: broken, dropped |
| `F4 xx` inside a frame, `xx` not `F4`, `F5` or `FB` | the `F4` is dropped, `xx` kept; the checksum then usually fails | both bytes kept; the checksum then usually fails. Same verdict, different bytes in the logged frame |

The last row is also where the Python decoder differs from both: `split_frames` abandons the
frame on a lone `F4` and resumes scanning after the start tag.

### 4.3 Driver rejects, stock accepts

| Case | Stock | Driver |
|---|---|---|
| A frame longer than 200 bytes (`RX_FRAME_MAX`) | up to 512 bytes assembled | broken at byte 4 when LEN + 9 exceeds 200 |
| `F4 F5` inside a frame whose LEN says it continues | the frame restarts at that point, unconditionally | the frame in front keeps going; the inner start is kept as a fallback only if the front breaks. If the front frame completes by its LEN with the embedded `F4 F5`, that frame is delivered (and fails its checksum unless the bytes happen to sum right) |
| A second frame inside the same window | not seen by this transaction; picked up by the next one as its reply | after a frame of the wrong class, the driver keeps listening and takes the next matching frame in the same window |
| A reply arriving after the window | kept, returned at once to the next transaction (which usually rejects it on the echo check, or accepts it when the classes coincide, as with two heartbeats in a row) | `begin_transaction_` drains the UART and resets the assembler before each request: a late reply is dropped |

The driver handles late and extra frames more strictly than stock does. Keep that; the rows are
here so the difference is on record.

### 4.4 What this suggests for the driver

Recommended, in order of value:

1. Match the echo check for verified classes: for the status poll and the ProductType request
   require bytes 13 and 14 to equal the request's, and byte 15 to be `01`, before a frame ends the
   transaction. This is what separates a `66 40` reply from a `66 00` reply, which today both
   satisfy `CLASS_STATUS`. The heartbeat and the command slot accept any class on purpose
   (the A/C answers a heartbeat with a `0x66` sometimes, per the comments in `hisense_bus.cpp`);
   stock would reject those, so keep the current behaviour there and say so in the comment.
2. Check bytes 5 and 11 for `01` where the frame is judged (`frame_received_`), and count a
   failure as a checksum error or a new "frame format" counter. No captured frame fails it, so
   this only brings the driver level with stock.
3. Leave `RX_FRAME_MAX` at 200 unless a reply over 200 bytes is ever captured; the stock limit of
   512 comes from its buffer size.
4. Nothing to change in the Python decoder beyond a note that stock drops a lone `F4` and keeps
   going where the script abandons the frame.

## 5. Not established

- The interrupt side: which SDK handler calls `0x247d6`, and whether it is gated on the state
  byte. Not in this image.
- Whether the semaphore is binary or counting (`0x27e94` wraps the SDK give; the create call was
  not traced). A counting semaphore would make a stale frame pending for more than one later
  transaction.
- The sub-header extension bytes (6 and 12) have never been seen non-zero, so the offsets the
  helpers compute for those cases are read from the code and not checked against a frame.
- None of this was run on hardware; no bench frame was fed to a stock module to watch a log line.
