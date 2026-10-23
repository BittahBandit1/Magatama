// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+




// Important!  All FIFO containers in this header should be 'struct' type, not class type.
// They are saved into the savestate as-is, and keeping them as struct ensures that the
// layout of their contents is reliable.

struct IPU_Fifo_Input
{
	alignas(16) u32 data[32];
	int readpos, writepos;

	int write(const u32* pMem, int size);
	int read(void *value);
	void clear();
	std::string desc() const;
};

struct IPU_Fifo_Output
{
	alignas(16) u32 data[32];
	int readpos, writepos;

	// returns number of qw read
	int write(const u32 * value, uint size);
	void read(void *value, uint size);
	void clear();
	std::string desc() const;
};

struct IPU_Fifo
{
	alignas(16) IPU_Fifo_Input in;
	alignas(16) IPU_Fifo_Output out;

	void init();
	void clear();
};

alignas(16) extern IPU_Fifo ipu_fifo;

struct IPUStatus { bool DataRequested, WaitingOnIPUFrom, WaitingOnIPUTo; };
extern IPUStatus IPUCoreStatus;
// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+




#define ipumsk( src ) ( (src) & 0xff )
#define ipucase( src ) case ipumsk(src)

//
// Bitfield Structures
//

union tIPU_CMD
{
	struct
	{
		u32 DATA;
		u32 BUSY;
	};
	u64 _u64;

	void SetBusy(bool busy = true)
	{
		BUSY = busy ? 0x80000000 : 0;
	}
};

union tIPU_CTRL {
	struct {
		u32 IFC : 4;	// Input FIFO counter
		u32 OFC : 4;	// Output FIFO counter
		u32 CBP : 6;	// Coded block pattern
		u32 ECD : 1;	// Error code pattern
		u32 SCD : 1;	// Start code detected
		u32 IDP : 2;	// Intra DC precision
		u32 resv0 : 2;
		u32 AS : 1;		// Alternate scan
		u32 IVF : 1;	// Intra VLC format
		u32 QST : 1;	// Q scale step
		u32 MP1 : 1;	// MPEG1 bit stream
		u32 PCT : 3;	// Picture Type
		u32 resv1 : 3;
		u32 RST : 1;	// Reset
		u32 BUSY : 1;	// Busy
	};
	u32 _u32;

	tIPU_CTRL() = default;
	tIPU_CTRL( u32 val ) { _u32 = val; }

    // CTRL = the first 16 bits of ctrl [0x8000ffff], + value for the next 16 bits,
    // minus the reserved bits. (18-19; 27-29) [0x47f30000]
	void write(u32 value) { _u32 = (value & 0x47f30000) | (_u32 & 0x8000ffff); }

	bool test(u32 flags) const { return !!(_u32 & flags); }
	void set_flags(u32 flags) { _u32 |= flags; }
	void clear_flags(u32 flags) { _u32 &= ~flags; }
	void reset() { _u32 &= 0x7F33F00; }
};

struct alignas(16) tIPU_BP {
	alignas(16) u128 internal_qwc[2];

	u32 BP;		// Bit stream point (0 to 128*2)
	u32 IFC;	// Input FIFO counter (8QWC) (0 to 8)
	u32 FP;		// internal FIFO (2QWC) fill status (0 to 2)

	__fi void Align()
	{
		BP = (BP + 7) & ~7;
		Advance(0);
	}

	__fi void Advance(uint bits)
	{
		FillBuffer(bits);

		BP += bits;
		pxAssume( BP <= 256 );

		if (BP >= 128)
		{
			BP -= 128;

			if (FP == 2)
			{
				// when BP is over 128 it means we're reading data from the second quadword.  Shift that one
				// to the front and load the new quadword into the second QWC (its a manualized ringbuffer!)

				CopyQWC(&internal_qwc[0], &internal_qwc[1]);
				FP = 1;
			}
			else
			{
				// if FP == 1 then the buffer has been completely drained.
				// if FP == 0 then an already-drained buffer is being advanced, and we need to drop a
				// quadword from the IPU FIFO.

				if (ipu_fifo.in.read(&internal_qwc[0]))
					FP = 1;
				else
					FP = 0;
			}
		}
	}

	__fi bool FillBuffer(u32 bits)
	{
		while ((FP * 128) < (BP + bits))
		{
			if (ipu_fifo.in.read(&internal_qwc[FP]) == 0)
			{
				// Here we *try* to fill the entire internal QWC buffer; however that may not necessarily
				// be possible -- so if the fill fails we'll only return 0 if we don't have enough
				// remaining bits in the FIFO to fill the request.
				// Used to do ((FP!=0) && (BP + bits) <= 128) if we get here there's defo not enough data now though
				IPUCoreStatus.WaitingOnIPUTo = true;
				return false;
			}

			++FP;
		}

		return true;
	}
};

union tIPU_CMD_IDEC
{
	struct
	{
		u32 FB  : 6;
		u32 UN2 :10;
		u32 QSC : 5;
		u32 UN1 : 3;
		u32 DTD : 1;
		u32 SGN : 1;
		u32 DTE : 1;
		u32 OFM : 1;
		u32 cmd : 4;
	};

	u32 _u32;

	tIPU_CMD_IDEC() = default;
	tIPU_CMD_IDEC( u32 val ) { _u32 = val; }

	bool test(u32 flags) const { return !!(_u32 & flags); }
	void set_flags(u32 flags) { _u32 |= flags; }
	void clear_flags(u32 flags) { _u32 &= ~flags; }
	void reset() { _u32 = 0; }
	void log() const;
};

union tIPU_CMD_BDEC
{
	struct
	{
		u32 FB  : 6;
		u32 UN2 :10;
		u32 QSC : 5;
		u32 UN1 : 4;
		u32 DT  : 1;
		u32 DCR : 1;
		u32 MBI : 1;
		u32 cmd : 4;
	};
	u32 _u32;

	tIPU_CMD_BDEC() = default;
	tIPU_CMD_BDEC( u32 val ) { _u32 = val; }

	bool test(u32 flags) const { return !!(_u32 & flags); }
	void set_flags(u32 flags) { _u32 |= flags; }
	void clear_flags(u32 flags) { _u32 &= ~flags; }
	void reset() { _u32 = 0; }
	void log(int s_bdec) const;
};

union tIPU_CMD_CSC
{
	struct
	{
		u32 MBC :11;
		u32 UN2 :15;
		u32 DTE : 1;
		u32 OFM : 1;
		u32 cmd : 4;
	};
	u32 _u32;

	tIPU_CMD_CSC() = default;
	tIPU_CMD_CSC( u32 val ){ _u32 = val; }

	bool test(u32 flags) const { return !!(_u32 & flags); }
	void set_flags(u32 flags) { _u32 |= flags; }
	void clear_flags(u32 flags) { _u32 &= ~flags; }
	void reset() { _u32 = 0; }
	void log_from_YCbCr() const;
	void log_from_RGB32() const;
};

enum SCE_IPU
{
	SCE_IPU_BCLR = 0x0
,	SCE_IPU_IDEC
,	SCE_IPU_BDEC
,	SCE_IPU_VDEC
,	SCE_IPU_FDEC
,	SCE_IPU_SETIQ
,	SCE_IPU_SETVQ
,	SCE_IPU_CSC
,	SCE_IPU_PACK
,	SCE_IPU_SETTH
};

struct IPUregisters {
	tIPU_CMD	cmd;
	u32			dummy0[2];

	tIPU_CTRL	ctrl;
	u32			dummy1[3];

	u32			ipubp;
	u32			dummy2[3];

	u32			top;
	u32			topbusy;
	u32			dummy3[2];

	void SetTopBusy()
	{
		topbusy = 0x80000000;
	}

	void SetDataBusy()
	{
		cmd.BUSY = 0x80000000;
		topbusy = 0x80000000;
	}

};

union tIPU_cmd
{
	struct
	{
		int index;
		int pos[6];
		union {
			struct {
				u32 OPTION : 28;
				u32 CMD : 4;
			};
			u32 current;
		};
	};

	u128 _u128[2];

	void clear();
};

inline IPUregisters ipuRegs{};

extern bool FMVstarted;
extern bool EnableFMV;

alignas(16) extern tIPU_cmd ipu_cmd;
extern u64 eecount_on_last_vdec;

extern void ipuReset();

extern u32 ipuRead32(u32 mem);
extern u64 ipuRead64(u32 mem);
extern bool ipuWrite32(u32 mem,u32 value);
extern bool ipuWrite64(u32 mem,u64 value);

extern void IPUCMD_WRITE(u32 val);
extern void ipuSoftReset();
extern void IPUProcessInterrupt();

// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+




#ifdef _MSC_VER
#endif

#ifdef _MSC_VER
#define BigEndian(in) _byteswap_ulong(in)
#else
#define BigEndian(in) __builtin_bswap32(in) // or we could use the asm function bswap...
#endif

#ifdef _MSC_VER
#define BigEndian64(in) _byteswap_uint64(in)
#else
#define BigEndian64(in) __builtin_bswap64(in) // or we could use the asm function bswap...
#endif

struct macroblock_8{
	u8 Y[16][16];		//0
	u8 Cb[8][8];		//1
	u8 Cr[8][8];		//2
};

struct macroblock_16{
	s16 Y[16][16];			//0
	s16 Cb[8][8];			//1
	s16 Cr[8][8];			//2
};

struct macroblock_rgb32{
	struct {
		u8 r, g, b, a;
	} c[16][16];
};

struct rgb16_t{
	u16 r:5, g:5, b:5, a:1;
};

struct macroblock_rgb16{
	rgb16_t	c[16][16];
};

struct decoder_t {
	/* first, state that carries information from one macroblock to the */
	/* next inside a slice, and is never used outside of mpeg2_slice() */

	/* DCT coefficients - should be kept aligned ! */
	s16 DCTblock[64];

	u8 niq[64];			//non-intraquant matrix (sequence header)
	u8 iq[64];			//intraquant matrix (sequence header)

	macroblock_8 mb8;
	macroblock_16 mb16;
	macroblock_rgb32 rgb32;
	macroblock_rgb16 rgb16;

	uint ipu0_data;		// amount of data in the output macroblock (in QWC)
	uint ipu0_idx;

	int quantizer_scale;

	/* now non-slice-specific information */

	/* picture header stuff */

	/* what type of picture this is (I, P, B, D) */
	int coding_type;

	/* picture coding extension stuff */

	/* predictor for DC coefficients in intra blocks */
	s16 dc_dct_pred[3];

	/* quantization factor for intra dc coefficients */
	int intra_dc_precision;
	/* top/bottom/both fields */
	int picture_structure;
	/* bool to indicate all predictions are frame based */
	int frame_pred_frame_dct;
	/* bool to indicate whether intra blocks have motion vectors */
	/* (for concealment) */
	int concealment_motion_vectors;
	/* bit to indicate which quantization table to use */
	int q_scale_type;
	/* bool to use different vlc tables */
	int intra_vlc_format;
	/* used for DMV MC */
	int top_field_first;
	// Pseudo Sign Offset
	int sgn;
	// Dither Enable
	int dte;
	// Output Format
	int ofm;
	// Macroblock type
	int macroblock_modes;
	// DC Reset
	int dcr;
	// Coded block pattern
	int coded_block_pattern;

	/* stuff derived from bitstream */

	/* the zigzag scan we're supposed to be using, true for alt, false for normal */
	bool scantype;

	int mpeg1;

	template< typename T >
	void SetOutputTo( T& obj )
	{
		uint mb_offset = ((uptr)&obj - (uptr)&mb8);
		pxAssume( (mb_offset & 15) == 0 );
		ipu0_idx	= mb_offset / 16;
		ipu0_data	= sizeof(obj)/16;
	}

	u128* GetIpuDataPtr()
	{
		return ((u128*)&mb8) + ipu0_idx;
	}

	void AdvanceIpuDataBy(uint amt)
	{
		pxAssertMsg(ipu0_data>=amt, "IPU FIFO Overflow on advance!" );
		ipu0_idx  += amt;
		ipu0_data -= amt;
	}
};

alignas(16) extern decoder_t decoder;
alignas(16) extern tIPU_BP g_BP;


	extern void ipu_dither(const macroblock_rgb32& rgb32, macroblock_rgb16& rgb16, int dte);

	void IPUWorker();

// Quantization matrix
extern rgb16_t g_ipu_vqclut[16]; //clut conversion table
extern u16 g_ipu_thresh[2]; //thresholds for color conversions

alignas(16) extern u8 g_ipu_indx4[16*16/2];
alignas(16) extern const int non_linear_quantizer_scale[32];
extern int coded_block_pattern;

struct mpeg2_scan_pack
{
	u8 norm[64];
	u8 alt[64];
};

alignas(16) extern const std::array<u8, 1024> g_idct_clip_lut;
alignas(16) extern const mpeg2_scan_pack mpeg2_scan;
