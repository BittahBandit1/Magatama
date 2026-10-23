# Portable IPU core

Adapted from PCSX2 revision fd9d310ccbb6 (the source pinned by the local
Fukami bridge), pcsx2/IPU/{IPU.cpp,IPU.h,IPU_MultiISA.cpp,IPU_MultiISA.h,
IPU_Fifo.h,mpeg2_vlc.h,IPUdither.cpp,yuv2rgb.cpp}. Original copyright and
SPDX notices are preserved. The MPEG decoder retains its GPL-2.0+ notice;
the integration is GPL-3.0+.

The scalar decoder, IDCT, VLC tables, color conversion and dither are retained.
Emulator-specific includes, save states, logging, CPU scheduling and register
storage are replaced by rrv_ipu.h. FIFO and DMA integration belongs to this
runtime. No game assets are included.
