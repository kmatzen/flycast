#include "rend/soft/PresentationFrame.h"
#include <gtest/gtest.h>
#include <limits>

using namespace rend::soft;

TEST(PresentationFrame, OwnsPixelsAndMetadataAcrossProducerReplacement)
{
	std::vector<Color> source(6, {1, 2, 3, 255});
	Color border {4, 5, 6, 255};
	auto producer = PresentationFrame::create(3, 2, 1.5f, border, source);
	ASSERT_TRUE(producer);
	auto consumer = producer;
	source.assign(6, {9, 8, 7, 255});
	border = {0, 0, 0, 0};
	producer = PresentationFrame::create(2, 3, 2.f / 3.f, border, source);
	ASSERT_TRUE(producer);
	EXPECT_EQ(consumer->width, 3u);
	EXPECT_EQ(consumer->height, 2u);
	EXPECT_FLOAT_EQ(consumer->aspectRatio, 1.5f);
	EXPECT_EQ(consumer->border, (Color {4, 5, 6, 255}));
	for (const auto& pixel : consumer->pixels)
		EXPECT_EQ(pixel, (Color {1, 2, 3, 255}));
	producer.reset();
	EXPECT_EQ(consumer->pixels.size(), 6u);
}

TEST(PresentationFrame, RejectsMalformedImages)
{
	EXPECT_FALSE(PresentationFrame::create(0, 2, 1.f, {}, {}));
	EXPECT_FALSE(PresentationFrame::create(2, 0, 1.f, {}, {}));
	EXPECT_FALSE(PresentationFrame::create(3, 2, 1.f, {}, std::vector<Color>(5)));
	EXPECT_FALSE(PresentationFrame::create(3, 2, 1.f, {}, std::vector<Color>(7)));
	EXPECT_FALSE(PresentationFrame::create(0xffffffffu, 0xffffffffu, 1.f, {}, {}));
	for (float aspect : {0.f, -1.f, std::numeric_limits<float>::infinity(),
			std::numeric_limits<float>::quiet_NaN()})
		EXPECT_FALSE(PresentationFrame::create(1, 1, aspect, {}, std::vector<Color>(1)));
}
