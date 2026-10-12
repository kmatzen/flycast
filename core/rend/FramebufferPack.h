#pragma once
#include "hw/pvr/pvr_regs.h"

template<int bits>
static inline u8 roundColor(u8 in)
{
	u8 out = in >> (8 - bits);
	if (out != 0xffu >> (8 - bits))
		out += (in >> (8 - bits - 1)) & 1;
	return out;
}

// 0555 KRGB 16 bit  (default)	Bit 15 is the value of fb_kval[7].
template<int Red, int Green, int Blue, int Alpha, typename PixelWriter, bool Round = false>
class FBLineWriter0555
{
public:
	FBLineWriter0555(FB_W_CTRL_type fb_w_ctrl, PixelWriter& pixWriter) : pixWriter(pixWriter) {
		kval_bit = (fb_w_ctrl.fb_kval & 0x80) << 8;
	}

	void write(int xmin, int xmax, const u8 *& pixel, int y)
	{
		for (int c = xmin; c < xmax; c++)
		{
			u8 red, green, blue;
			if constexpr (Round)
			{
				red = roundColor<5>(pixel[Red]);
				green = roundColor<5>(pixel[Green]);
				blue = roundColor<5>(pixel[Blue]);
			}
			else
			{
				red = pixel[Red] >> 3;
				green = pixel[Green] >> 3;
				blue = pixel[Blue] >> 3;
			}
			pixWriter.write((u16)((red << 10) | (green << 5) | blue | kval_bit));
			pixel += 4;
		}
	}

	static constexpr int BytesPerPixel = 2;

private:
	u16 kval_bit;
	PixelWriter& pixWriter;
};

// 565 RGB 16 bit
template<int Red, int Green, int Blue, int Alpha, typename PixelWriter, bool Round = false>
class FBLineWriter565
{
public:
	FBLineWriter565(FB_W_CTRL_type fb_w_ctrl, PixelWriter& pixWriter) : pixWriter(pixWriter) {}

	void write(int xmin, int xmax, const u8 *& pixel, int y)
	{
		for (int c = xmin; c < xmax; c++)
		{
			u8 red, green, blue;
			if constexpr (Round)
			{
				red = roundColor<5>(pixel[Red]);
				green = roundColor<6>(pixel[Green]);
				blue = roundColor<5>(pixel[Blue]);
			}
			else
			{
				red = pixel[Red] >> 3;
				green = pixel[Green] >> 2;
				blue = pixel[Blue] >> 3;
			}
			pixWriter.write((u16)((red << 11) | (green << 5) | blue));
			pixel += 4;
		}
	}

	static constexpr int BytesPerPixel = 2;

private:
	PixelWriter& pixWriter;
};

// 4444 ARGB 16 bit
template<int Red, int Green, int Blue, int Alpha, typename PixelWriter, bool Round = false>
class FBLineWriter4444
{
public:
	FBLineWriter4444(FB_W_CTRL_type fb_w_ctrl, PixelWriter& pixWriter) : pixWriter(pixWriter) {}

	void write(int xmin, int xmax, const u8 *& pixel, int y)
	{
		for (int c = xmin; c < xmax; c++)
		{
			u8 red, green, blue, alpha;
			if constexpr (Round)
			{
				red = roundColor<4>(pixel[Red]);
				green = roundColor<4>(pixel[Green]);
				blue = roundColor<4>(pixel[Blue]);
				alpha = roundColor<4>(pixel[Alpha]);
			}
			else
			{
				red = pixel[Red] >> 4;
				green = pixel[Green] >> 4;
				blue = pixel[Blue] >> 4;
				alpha = pixel[Alpha] >> 4;
			}
			pixWriter.write((u16)((red << 8) | (green << 4) | blue | (alpha << 12)));
			pixel += 4;
		}
	}

	static constexpr int BytesPerPixel = 2;

private:
	PixelWriter& pixWriter;
};

// 1555 ARGB 16 bit    The alpha value is determined by comparison with the value of fb_alpha_threshold.
template<int Red, int Green, int Blue, int Alpha, typename PixelWriter, bool Round = false>
class FBLineWriter1555
{
public:
	FBLineWriter1555(FB_W_CTRL_type fb_w_ctrl, PixelWriter& pixWriter) : pixWriter(pixWriter) {
		fb_alpha_threshold = fb_w_ctrl.fb_alpha_threshold;
	}

	void write(int xmin, int xmax, const u8 *& pixel, int y)
	{
		for (int c = xmin; c < xmax; c++)
		{
			u8 red, green, blue;
			if constexpr (Round)
			{
				red = roundColor<5>(pixel[Red]);
				green = roundColor<5>(pixel[Green]);
				blue = roundColor<5>(pixel[Blue]);
			}
			else
			{
				red = pixel[Red] >> 3;
				green = pixel[Green] >> 3;
				blue = pixel[Blue] >> 3;
			}
			pixWriter.write((u16)((red << 10) | (green << 5) | blue
					| (pixel[Alpha] >= fb_alpha_threshold ? 0x8000 : 0)));
			pixel += 4;
		}
	}

	static constexpr int BytesPerPixel = 2;

private:
	PixelWriter& pixWriter;
	u8 fb_alpha_threshold;
};

// 888 RGB 24 bit packed
template<int Red, int Green, int Blue, int Alpha, typename PixelWriter>
class FBLineWriter888
{
public:
	FBLineWriter888(FB_W_CTRL_type fb_w_ctrl, PixelWriter& pixWriter) : pixWriter(pixWriter) {}

	void write(int xmin, int xmax, const u8 *& pixel, int y)
	{
		for (int c = xmin; c < xmax; c++)
		{
			pixWriter.write(pixel[Blue]);
			pixWriter.write(pixel[Green]);
			pixWriter.write(pixel[Red]);
			pixel += 4;
		}
	}

	static constexpr int BytesPerPixel = 3;

private:
	PixelWriter& pixWriter;
};

// 0888 KRGB 32 bit (K is the value of fb_kval.)
template<int Red, int Green, int Blue, int Alpha, typename PixelWriter>
class FBLineWriter0888
{
public:
	FBLineWriter0888(FB_W_CTRL_type fb_w_ctrl, PixelWriter& pixWriter) : pixWriter(pixWriter) {
		fb_kval = fb_w_ctrl.fb_kval << 24;
	}

	void write(int xmin, int xmax, const u8 *& pixel, int y)
	{
		for (int c = xmin; c < xmax; c++)
		{
			pixWriter.write((u32)((pixel[Red] << 16) | (pixel[Green] << 8) | pixel[Blue] | fb_kval));
			pixel += 4;
		}
	}

	static constexpr int BytesPerPixel = 4;

private:
	u32 fb_kval;
	PixelWriter& pixWriter;
};

// 8888 ARGB 32 bit
template<int Red, int Green, int Blue, int Alpha, typename PixelWriter>
class FBLineWriter8888
{
public:
	FBLineWriter8888(FB_W_CTRL_type fb_w_ctrl, PixelWriter& pixWriter) : pixWriter(pixWriter) {}

	void write(int xmin, int xmax, const u8 *& pixel, int y)
	{
		for (int c = xmin; c < xmax; c++)
		{
			pixWriter.write((u32)((pixel[Red] << 16) | (pixel[Green] << 8) | pixel[Blue] | (pixel[Alpha] << 24)));
			pixel += 4;
		}
	}

	static constexpr int BytesPerPixel = 4;

private:
	PixelWriter& pixWriter;
};
