# Write the region 0x0..0x140000 (~1.27MB) + set QE. A region write leaves everything above
# 0x140000 alone: the second firmware slot and the settings areas at the top of flash. The range
# is the one the retired Matter clip image used. Transport: ch341common.
import sys
from ch341common import flash_region
if __name__=="__main__":
    flash_region(sys.argv[1], 0x0, 0x140000)
