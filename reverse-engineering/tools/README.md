# Reverse-engineering tools

Small Python scripts that were used to work out the bus protocol and to read the vendor firmware.
Three of them capture or decode bus traffic and are still what you use to check a unit. Three
read a flash dump. Two belong to the vendor firmware's Matter interface, which the project no
longer uses.

A flash dump is never in the repository ([`dumps/README.md`](../../dumps/README.md)). The scripts
that need one take its path as an argument.

## Install

`decode_ac_frames.py` with `--log`, `--raw` or `--hex`, `xref.py` and
`extract_matter_setup_code.py` need Python 3 and nothing else. The others need the packages
pinned in [`requirements.txt`](requirements.txt):

```
python3 -m venv ~/.fw-analysis-venv
~/.fw-analysis-venv/bin/pip install -r reverse-engineering/tools/requirements.txt
```

## The tools

| Tool | What it does | Input | Output | Used for |
|---|---|---|---|---|
| `sniff.py` | Logs frames from a serial adapter on the bus, split at 10 ms of silence, with timestamps | A serial port (`SNIFF_PORT`, default `/dev/ttyUSB0`). `sniff.py [baud] [seconds]`, defaults 9600 and 60 | Lines of hex on stdout, appended to `capture_<baud>.log` in the current directory | The DI and RO taps of July 2026: the link frames (2026-07-06) and the command corrections (2026-07-08) of [doc 03](../docs/03-rs485-ac-protocol.md) |
| `decode_ac_frames.py` | Splits a byte stream into frames, removes doubled `F4`, checks the checksum, decodes command and status fields, and warns when a status frame's length or header is not what the first driver expected | One of `--log FILE` (a `sniff.py` log), `--raw FILE` (binary), `--hex "..."` (one pasted frame), `--port DEV` with `--baud` and `--secs` (live, needs pyserial). `--dir tx\|rx\|di\|ro` only labels the output | One block per frame: length, class, checksum verdict, decoded fields | Checking every capture. The finding that the status frame is 160 bytes and not 82. Still in use: the host tests import its `checksum` and `split_frames` (`firmware/test/run_tests.sh` layer 2, `virtual_ac.py`, `host_e2e/ac_model.py`), so a change to it runs the test suite |
| `map_diff.py` | Captures status frames for a while and reports which byte offsets changed, with the values each went through and a note when one bit flipped | A serial port on the RO tap (`PORT`, `BAUD` environment variables, defaults `/dev/ttyUSB0` and 9600). `map_diff.py [seconds]`, default 35 | The changed offsets on stdout. The raw capture is saved as `/tmp/mapdiff_<time>.bin` | The status byte map: one control toggled on the remote per run ([doc 03](../docs/03-rs485-ac-protocol.md#status-frame-byte-map-class-66-sub-00-160-bytes)) |
| `disasm_ac_protocol.py` | Finds the firmware's frame parser from its error strings, derives the load base from the pointers to them, and disassembles the code in front of the literal pool, marking compares against `F4`, `F5` and `FB` | A flash dump. Needs capstone | The detected base and a Thumb listing on stdout | How the frame envelope was first recovered ([doc 03](../docs/03-rs485-ac-protocol.md#frame-envelope), doc 10 §3.2) |
| `xref.py` | Finds the code that refers to an address: literal-pool words equal to the target with the `LDR` instructions that load them, and `MOVW`/`MOVT` pairs that build it | `xref.py <image> <base hex> <target hex>` | Pool locations and referring instructions, as file offsets and runtime addresses | The cross-reference work behind docs 10, 11 and 13 (which function uses which string or table) |
| `extract_matter_setup_code.py` | Checks a dump for the Matter test passcode, discriminator and pairing code, and for per-device commissioning data | A flash dump | A verdict: test credentials or per-device data | The finding that the vendor firmware ships the Matter test credentials ([doc 02](../docs/02-matter-local-control.md#commissioning-credentials)) |
| `gen_matter_qr.py` | Draws a Matter onboarding QR code from an `MT:` payload | `gen_matter_qr.py [payload] [out.png]`, defaults to the test payload and `matter_qr.png`. Needs qrcode | A PNG | [`../hardware/matter_qr.png`](../hardware/matter_qr.png) |

`sniff.py`, `map_diff.py`, `xref.py`, `disasm_ac_protocol.py`, `extract_matter_setup_code.py`
and `gen_matter_qr.py` read their arguments by position and have no `--help`. Running `sniff.py`
or `map_diff.py` with `--help` fails on the argument, and `gen_matter_qr.py --help` would write a
QR code of the text `--help`. Read the docstring at the top of each file.

## Status

| Tool | State |
|---|---|
| `decode_ac_frames.py` | In use and covered by the host tests |
| `sniff.py`, `map_diff.py` | Current. They need a serial adapter on a tapped bus. Not run for this revision of the page |
| `disasm_ac_protocol.py`, `xref.py` | Current for anyone with a dump. `xref.py` was run here on a synthetic image and found the planted reference. `disasm_ac_protocol.py` was not run: no dump and no capstone on the machine that wrote this page |
| `extract_matter_setup_code.py`, `gen_matter_qr.py` | Retired use. They serve the vendor firmware's own Matter interface, which was the project's first route to local control and is no longer used ([doc 02](../docs/02-matter-local-control.md)). They still work on a module with the vendor firmware: the first was run here on a synthetic blob holding the test values and reported them |

None of these tools was written for the project's own Matter firmwares (retired, tag
`matter-final`). Their tooling was removed with them.

The flash is read with [`firmware/flasher/ch341dump.py`](../../firmware/flasher/ch341dump.py),
which is not part of this directory.

## Decoding a frame by hand

```
$ python3 reverse-engineering/tools/decode_ac_frames.py \
    --hex "F4 F5 01 40 0D 01 00 FE 01 00 00 01 00 0A 04 01 01 01 01 60 F4 FB"
# 1 frames decoded
[12:33:40] #0    (22B) f4 f5 01 40 0d 01 00 fe 01 00 00 01 00 0a 04 01 01 01 01 60 f4 fb
     -> cls=0x0A chk=OK  (non 0x65/0x66 -- LINK/NET envelope?)
```

That is the device-type reply of [doc 03](../docs/03-rs485-ac-protocol.md#the-device-type-exchange).
The decoder knows the fields of command (`0x65`) and status (`0x66`) frames only. For any other
class it reports the length and the checksum.
