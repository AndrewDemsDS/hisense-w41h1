# dumps/ — raw flash dumps & analysis (LOCAL ONLY)

Everything in this directory except this README is **gitignored** — it holds the raw
SPI-flash image and derived analysis, which contain the module's provisioned **Wi-Fi
credentials + RSA device key** and the copyrighted vendor firmware. Never commit it.

| File | What |
|------|------|
| `w41h1_dump1.bin` | The canonical 4 MB GD25Q32 flash dump. Read twice from the chip; both reads were byte-identical (verification). The stock-recovery image at `../firmware/factory/STOCK-RECOVERY-w41h1_dump1.bin` is a symlink to this file. |
| `strings.txt` | `strings` output of the dump (credentials-bearing). |
| `internal1.png` | Board photo. |
| `ip.html` | Captured device page. |

The sniffer and Matter-code extractors live in `../reverse-engineering/tools/`.
