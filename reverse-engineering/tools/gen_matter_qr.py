#!/usr/bin/env python3
"""Generate a Matter onboarding QR PNG from an MT: payload.

    pip install "qrcode[pil]"
    ./gen_matter_qr.py "MT:Y.K9042C00KA0648G00" matter_qr.png
"""
import sys
import qrcode

payload = sys.argv[1] if len(sys.argv) > 1 else "MT:Y.K9042C00KA0648G00"
out = sys.argv[2] if len(sys.argv) > 2 else "matter_qr.png"

qr = qrcode.QRCode(box_size=12, border=4)
qr.add_data(payload)
qr.make(fit=True)
qr.make_image(fill_color="black", back_color="white").save(out)
print(f"wrote {out} for payload {payload}")
