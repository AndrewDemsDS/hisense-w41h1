# Flash the firmware region 0x0..0x140000 (~1.27MB app) + set QE. Region write preserves
# the Matter commissioning KV (0x2FF000+) -> no re-commission. Transport: ch341common.
import sys
from ch341common import flash_region
if __name__=="__main__":
    flash_region(sys.argv[1], 0x0, 0x140000)
