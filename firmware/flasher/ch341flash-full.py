# Whole-chip 4MB write (erases factory data incl. the Matter KV -> recoverable from the
# stock dump). Use for a clean-slate restore/flash. Transport: ch341common.
import sys
from ch341common import flash_region
if __name__=="__main__":
    flash_region(sys.argv[1], 0x0, 0x400000)
