# Whole-chip 4MB write + set QE: every byte of the chip is replaced, per-unit data included.
# Use it to put a unit's own full dump back. Transport: ch341common.
import sys
from ch341common import flash_region
if __name__=="__main__":
    flash_region(sys.argv[1], 0x0, 0x400000)
