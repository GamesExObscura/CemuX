#include <array>
#include <bit>

static uint32 ppc_cmp_and_mask[8] = {
	0xfffffff0,
	0xffffff0f,
	0xfffff0ff,
	0xffff0fff,
	0xfff0ffff,
	0xff0fffff,
	0xf0ffffff,
	0x0fffffff,
};


#define ppc_word_rotl(_data, _n) (std::rotl<uint32>(_data,(_n)&0x1F))

static inline uint32 ppc_mask(int MB, int ME)
{
	uint32 maskMB = 0xFFFFFFFF >> MB;
	uint32 maskME = 0xFFFFFFFF << (31-ME);
	uint32 mask2 = (MB <= ME) ? maskMB & maskME : maskMB | maskME;
	return mask2;
}

static inline bool ppc_carry_3(uint32 a, uint32 b, uint32 c)
{
	if ((a+b) < a) {
		return true;
	}
	if ((a+b+c) < c) {
		return true;
	}
	return false;
}

#define PPC_getBits(__value, __index, __bitCount) ((__value>>(31-__index))&((1<<__bitCount)-1))

// GQR scales are signed six-bit exponents. Decimal-rounded constants displaced
// Sonic Lost World's CPU-generated grass and zeroed small nonzero scales.
static constexpr std::array<float, 64> PPCMakePSQScaleTable(bool isLoad)
{
	std::array<float, 64> table{};
	for (uint32 index = 0; index < table.size(); ++index)
	{
		const sint32 scale = index < 32 ? static_cast<sint32>(index) : static_cast<sint32>(index) - 64;
		const uint32 exponent = static_cast<uint32>(127 + (isLoad ? -scale : scale));
		table[index] = std::bit_cast<float>(exponent << 23);
	}
	return table;
}

static constexpr auto LD_SCALE = PPCMakePSQScaleTable(true);
static constexpr auto ST_SCALE = PPCMakePSQScaleTable(false);

static float dequantize(uint32 data, sint32 type, uint8 scale)
{
	float f;
	switch (type)
	{
	case 4: // u8
		f = (float)(uint8)data; 
		f *= LD_SCALE[scale];
		break;
	case 5: // u16
		f = (float)(uint16)data; 
		f *= LD_SCALE[scale];
		break;
	case 6: // s8
		f = (float)(sint8)data;
		f *= LD_SCALE[scale];
		break;
	case 7: // float
		f = (float)(sint16)data;
		f *= LD_SCALE[scale];
		break;
	case 0:
	default:
		f = *((float *)&data);
		// scale does not apply when loading floats
		break;
	}
	return f;
}

static uint32 quantize(float data, sint32 type, uint8 scale)
{
	uint32 val;

	switch (type)
	{
	case 4: // u8
		data *= ST_SCALE[scale];
		if (data < 0) data = 0;
		if (data > 255) data = 255;
		val = (uint8)(uint32)data; 
		break;
	case 5: // u16
		data *= ST_SCALE[scale];
		if (data < 0) data = 0;
		if (data > 65535) data = 65535;
		val = (uint16)(uint32)data; 
		break;
	case 6: // s8
		data *= ST_SCALE[scale];
		if (data < -128) data = -128;
		if (data > 127) data = 127;
		val = (sint8)(uint8)(sint32)(uint32)data;
		break;
	case 7: // s16
		data *= ST_SCALE[scale];
		if (data < -32768) data = -32768;
		if (data > 32767) data = 32767;
		val = (sint16)(uint16)(sint32)(uint32)data;
		break;
	case 0: // float
	default: 
		// scale does not apply when storing floats
		*((float*)&val) = data; 
		break;
	}
	return val;
}

#define _uint32_fastSignExtend(__v, __bits) (uint32)(((sint32)(__v)<<(31-(__bits)))>>(31-(__bits)));

static inline uint64 ConvertToDoubleNoFTZ(uint32 value)
{
	// http://www.freescale.com/files/product/doc/MPCFPE32B.pdf

	uint64 x = value;
	uint64 exp = (x >> 23) & 0xff;
	uint64 frac = x & 0x007fffff;

	if (exp > 0 && exp < 255)
	{
		uint64 y = !(exp >> 7);
		uint64 z = y << 61 | y << 60 | y << 59;
		return ((x & 0xc0000000) << 32) | z | ((x & 0x3fffffff) << 29);
	}
	else if (exp == 0 && frac != 0) // denormal
	{
		exp = 1023 - 126;
		do
		{
			frac <<= 1;
			exp -= 1;
		} while ((frac & 0x00800000) == 0);

		return ((x & 0x80000000) << 32) | (exp << 52) | ((frac & 0x007fffff) << 29);
	}
	else  // QNaN, SNaN or Zero
	{
		uint64 y = exp >> 7;
		uint64 z = y << 61 | y << 60 | y << 59;
		return ((x & 0xc0000000) << 32) | z | ((x & 0x3fffffff) << 29);
	}
}

static inline uint32 ConvertToSingleNoFTZ(uint64 x)
{
	uint32 exp = (x >> 52) & 0x7ff;
	if (exp > 896 || (x & ~0x8000000000000000ULL) == 0)
	{
		return ((x >> 32) & 0xc0000000) | ((x >> 29) & 0x3fffffff);
	}
	else if (exp >= 874)
	{
		uint32 t = (uint32)(0x80000000 | ((x & 0x000FFFFFFFFFFFFFULL) >> 21));
		t = t >> (905 - exp);
		t |= (x >> 32) & 0x80000000;
		return t;
	}
	else
	{
		return ((x >> 32) & 0xc0000000) | ((x >> 29) & 0x3fffffff);
	}
}
