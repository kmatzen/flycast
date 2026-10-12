#include "rend/soft/PresentationFrame.h"
#include "rend/soft/CadenceProbe.h"
#include <gtest/gtest.h>
#include <limits>
#include <atomic>
#include <thread>

using namespace rend::soft;

TEST(PresentationFrame, OrderedQueueCoalescesDuplicatesAndReportsOverflow)
{
	OrderedPresentationQueue queue;
	auto make = [](unsigned value, unsigned sequence) {
		return PresentationFrame::create(1, 1, 1.f, {}, {{u8(value), 0, 0, 255}}, sequence);
	};
	auto first = make(1, 1);
	queue.publish(first);
	queue.publish(make(1, 2));
	queue.publish(make(2, 3));
	queue.publish(make(3, 4));
	EXPECT_EQ(queue.take(), first);
	EXPECT_EQ(queue.take()->sequence, 3u);
	EXPECT_EQ(queue.take()->sequence, 4u);
	queue.publish(make(3, 5));
	EXPECT_FALSE(queue.take());
	for (unsigned i = 4; i < 8; ++i) queue.publish(make(i, i + 2));
	EXPECT_EQ(queue.dropped(), 1u);
	EXPECT_EQ(queue.take()->sequence, 7u);
	queue.clear();
	EXPECT_FALSE(queue.take());
	EXPECT_EQ(queue.dropped(), 0u);
	queue.publish(make(7, 1));
	ASSERT_TRUE(queue.take()); // A new epoch must not coalesce with the old one.
}

TEST(PresentationFrame, OrderedQueuePreservesMetadataChanges)
{
	OrderedPresentationQueue queue;
	queue.publish(PresentationFrame::create(1, 1, 1.f, {}, {{1, 2, 3, 255}}, 1));
	queue.publish(PresentationFrame::create(1, 1, 2.f, {}, {{1, 2, 3, 255}}, 2));
	queue.publish(PresentationFrame::create(1, 1, 2.f, {1, 0, 0, 255}, {{1, 2, 3, 255}}, 3));
	for (unsigned sequence = 1; sequence <= 3; ++sequence) {
		auto frame = queue.take();
		ASSERT_TRUE(frame);
		EXPECT_EQ(frame->sequence, sequence);
	}
}

TEST(PresentationFrame, CadenceProbeIdsSurviveFramebufferConcatenation)
{
	for (unsigned concat = 0; concat < 8; ++concat)
		for (unsigned id = 0; id < 2048; ++id)
		{
			const Color color {static_cast<u8>(((id & 31) << 3) | concat),
					static_cast<u8>(((id >> 5) << 2) | (concat & 3)),
					static_cast<u8>(128 | concat), 255};
			EXPECT_EQ(decodeCadenceProbeId(color), id < 660 ? static_cast<int>(id) : -1);
		}
	EXPECT_EQ(decodeCadenceProbeId({0, 0, 0, 255}), -1);
	EXPECT_EQ(decodeCadenceProbeId({0, 0, 128, 0}), -1);
	EXPECT_EQ(decodeCadenceProbeId({1, 0, 128, 255}), -1);
	EXPECT_EQ(decodeCadenceProbeId({0, 1, 128, 255}), -1);
}

TEST(PresentationFrame, DeliveryDistinguishesGapsRepeatsAndOutOfOrder)
{
	PresentationDelivery delivery;
	delivery.complete(0);
	delivery.complete(3);
	delivery.complete(3);
	delivery.complete(2);
	delivery.complete(4);
	delivery.complete(7);
	EXPECT_EQ(delivery.unique, 3u);
	EXPECT_EQ(delivery.skipped, 4u);
	EXPECT_EQ(delivery.repeated, 1u);
	EXPECT_EQ(delivery.outOfOrder, 1u);
	EXPECT_EQ(delivery.lastSequence, 7u);
	EXPECT_EQ(delivery.unique + delivery.skipped, delivery.lastSequence);
}

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

TEST(PresentationFrame, ConcurrentPublicationRetainsCompleteSnapshots)
{
	PresentationMailbox mailbox;
	auto first = PresentationFrame::create(2, 3, 1.f, {17, 0, 0, 255},
			std::vector<Color>(6, {17, 0, 0, 255}));
	mailbox.publish(first);
	auto retained = mailbox.acquire();
	std::atomic<bool> finished {false};
	std::thread producer([&] {
		for (unsigned i = 0; i < 2000; ++i)
		{
			const u32 width = i % 7 + 1;
			const u32 height = i % 5 + 1;
			const Color color {static_cast<u8>(i), static_cast<u8>(i >> 8), 3, 255};
			mailbox.publish(PresentationFrame::create(width, height, float(width) / height,
					color, std::vector<Color>(width * height, color)));
		}
		finished = true;
	});
	do {
		const auto snapshot = mailbox.acquire();
		EXPECT_TRUE(snapshot);
		if (snapshot)
		{
			EXPECT_EQ(snapshot->pixels.size(), size_t(snapshot->width) * snapshot->height);
			for (const auto& pixel : snapshot->pixels)
				EXPECT_EQ(pixel, snapshot->border);
		}
	} while (!finished);
	producer.join();
	EXPECT_NE(mailbox.acquire(), retained);
	mailbox.clear();
	EXPECT_FALSE(mailbox.acquire());
	EXPECT_EQ(retained, first);
	EXPECT_EQ(retained->pixels.front(), (Color {17, 0, 0, 255}));
}
