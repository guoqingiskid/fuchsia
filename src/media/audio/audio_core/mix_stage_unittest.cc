// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/mix_stage.h"

#include <gmock/gmock.h>

#include "src/media/audio/audio_core/audio_clock.h"
#include "src/media/audio/audio_core/mixer/gain.h"
#include "src/media/audio/audio_core/packet_queue.h"
#include "src/media/audio/audio_core/ring_buffer.h"
#include "src/media/audio/audio_core/testing/fake_stream.h"
#include "src/media/audio/audio_core/testing/packet_factory.h"
#include "src/media/audio/audio_core/testing/threading_model_fixture.h"
#include "src/media/audio/lib/clock/clone_mono.h"
#include "src/media/audio/lib/clock/testing/clock_test.h"

using testing::Each;
using testing::FloatEq;

namespace media::audio {
namespace {

constexpr uint32_t kDefaultNumChannels = 2;
constexpr uint32_t kDefaultFrameRate = 48000;
const Format kDefaultFormat =
    Format::Create(fuchsia::media::AudioStreamType{
                       .sample_format = fuchsia::media::AudioSampleFormat::FLOAT,
                       .channels = kDefaultNumChannels,
                       .frames_per_second = kDefaultFrameRate,
                   })
        .take_value();

enum ClockMode { SAME, WITH_OFFSET, RATE_ADJUST };

class MixStageTest : public testing::ThreadingModelFixture {
  static constexpr uint32_t kBlockSizeFrames = 240;

 protected:
  void SetUp() {
    device_clock_ =
        AudioClock::CreateAsDeviceStatic(clock::CloneOfMonotonic(), AudioClock::kMonotonicDomain);
    mix_stage_ = std::make_shared<MixStage>(kDefaultFormat, kBlockSizeFrames, timeline_function_,
                                            device_clock_);
  }

  zx::time time_until(zx::duration delta) { return zx::time(delta.to_nsecs()); }

  fbl::RefPtr<VersionedTimelineFunction> timeline_function_ =
      fbl::MakeRefCounted<VersionedTimelineFunction>(TimelineFunction(
          TimelineRate(FractionalFrames<int64_t>(kDefaultFormat.frames_per_second()).raw_value(),
                       zx::sec(1).to_nsecs())));

  // Views the memory at |ptr| as a std::array of |N| elements of |T|. If |offset| is provided, it
  // is the number of |T| sized elements to skip at the beginning of |ptr|.
  //
  // It is entirely up to the caller to ensure that values of |T|, |N|, and |offset| are chosen to
  // not overflow |ptr|.
  template <typename T, size_t N>
  std::array<T, N>& as_array(void* ptr, size_t offset = 0) {
    return reinterpret_cast<std::array<T, N>&>(static_cast<T*>(ptr)[offset]);
  }

  AudioClock CreateClientClock() { return AudioClock::CreateAsCustom(clock::CloneOfMonotonic()); }

  AudioClock SetPacketFactoryWithOffsetAudioClock(zx::duration clock_offset,
                                                  testing::PacketFactory& factory);
  void TestMixStageTrim(ClockMode clock_mode);
  void TestMixStageUniformFormats(ClockMode clock_mode);
  void TestMixStageSingleInput(ClockMode clock_mode);
  void TestMixPosition(ClockMode clock_mode);

  std::shared_ptr<MixStage> mix_stage_;

  AudioClock device_clock_;
};

// TODO(50004): Add tests to verify we can read from other mix stages with unaligned frames.

AudioClock MixStageTest::SetPacketFactoryWithOffsetAudioClock(zx::duration clock_offset,
                                                              testing::PacketFactory& factory) {
  auto custom_clock_result =
      clock::testing::CreateCustomClock({.start_val = zx::clock::get_monotonic() + clock_offset});
  EXPECT_TRUE(custom_clock_result.is_ok());

  zx::clock custom_clock = custom_clock_result.take_value();
  EXPECT_TRUE(custom_clock.is_valid());

  auto offset_result = clock::testing::GetOffsetFromMonotonic(custom_clock);
  EXPECT_TRUE(offset_result.is_ok());
  auto actual_offset = offset_result.take_value();

  int64_t seek_frame = round(
      static_cast<double>(kDefaultFormat.frames_per_second() * actual_offset.get()) / ZX_SEC(1));
  factory.SeekToFrame(seek_frame);

  return AudioClock::CreateAsCustom(std::move(custom_clock));
}

void MixStageTest::TestMixStageTrim(ClockMode clock_mode) {
  // Set timeline rate to match our format.
  auto timeline_function = fbl::MakeRefCounted<VersionedTimelineFunction>(TimelineFunction(
      TimelineRate(FractionalFrames<uint32_t>(kDefaultFormat.frames_per_second()).raw_value(),
                   zx::sec(1).to_nsecs())));

  std::shared_ptr<PacketQueue> packet_queue;
  testing::PacketFactory packet_factory(dispatcher(), kDefaultFormat, PAGE_SIZE);

  if (clock_mode == ClockMode::SAME) {
    packet_queue =
        std::make_shared<PacketQueue>(kDefaultFormat, timeline_function, CreateClientClock());
  } else if (clock_mode == ClockMode::WITH_OFFSET) {
    AudioClock custom_audio_clock =
        SetPacketFactoryWithOffsetAudioClock(zx::sec(-2), packet_factory);
    ASSERT_TRUE(custom_audio_clock.is_valid());

    packet_queue = std::make_shared<PacketQueue>(kDefaultFormat, timeline_function,
                                                 std::move(custom_audio_clock));
  } else {
    ASSERT_TRUE(clock_mode == ClockMode::RATE_ADJUST) << "Unknown clock mode";
    ASSERT_TRUE(false) << "Multi-rate testing not yet implemented";
  }

  mix_stage_->AddInput(packet_queue);

  bool packet1_released = false;
  bool packet2_released = false;

  packet_queue->PushPacket(packet_factory.CreatePacket(
      1.0, zx::msec(5), [&packet1_released] { packet1_released = true; }));
  packet_queue->PushPacket(packet_factory.CreatePacket(
      0.5, zx::msec(5), [&packet2_released] { packet2_released = true; }));

  constexpr zx::duration kToleranceInterval = zx::sec(1) / kDefaultFrameRate;
  // Before 5ms: packet1 is not yet entirely consumed; we should still retain both packets.
  mix_stage_->Trim(time_until(zx::msec(5) - kToleranceInterval));
  RunLoopUntilIdle();
  EXPECT_FALSE(packet1_released);

  // After 5ms: packet1 is consumed and should have been released. We should still retain packet2.
  mix_stage_->Trim(time_until(zx::msec(5) + kToleranceInterval));
  RunLoopUntilIdle();
  EXPECT_TRUE(packet1_released);
  EXPECT_FALSE(packet2_released);

  // Before 10ms: packet2 is not yet entirely consumed; we should still retain it.
  mix_stage_->Trim(time_until(zx::msec(10) - kToleranceInterval));
  RunLoopUntilIdle();
  EXPECT_FALSE(packet2_released);

  // After 10ms: packet2 is consumed and should have been released.
  mix_stage_->Trim(time_until(zx::msec(10) + kToleranceInterval));
  RunLoopUntilIdle();
  EXPECT_TRUE(packet2_released);

  // Upon any fail, slab_allocator asserts at exit. Clear all allocations, so testing can continue.
  mix_stage_->Trim(zx::time::infinite());
}

TEST_F(MixStageTest, Trim) { TestMixStageTrim(ClockMode::SAME); }
TEST_F(MixStageTest, Trim_ClockOffset) { TestMixStageTrim(ClockMode::WITH_OFFSET); }

void MixStageTest::TestMixStageUniformFormats(ClockMode clock_mode) {
  // Set timeline rate to match our format.
  auto timeline_function = fbl::MakeRefCounted<VersionedTimelineFunction>(TimelineFunction(
      TimelineRate(FractionalFrames<uint32_t>(kDefaultFormat.frames_per_second()).raw_value(),
                   zx::sec(1).to_nsecs())));

  // Create 2 PacketQueues that we mix together. One may have a clock with an offset, so create a
  // seperate PacketFactory for it, that can set timestamps appropriately.
  testing::PacketFactory packet_factory1(dispatcher(), kDefaultFormat, PAGE_SIZE);
  testing::PacketFactory packet_factory2(dispatcher(), kDefaultFormat, PAGE_SIZE);

  auto packet_queue1 =
      std::make_shared<PacketQueue>(kDefaultFormat, timeline_function, CreateClientClock());
  std::shared_ptr<PacketQueue> packet_queue2;

  if (clock_mode == ClockMode::SAME) {
    packet_queue2 =
        std::make_shared<PacketQueue>(kDefaultFormat, timeline_function, CreateClientClock());
  } else if (clock_mode == ClockMode::WITH_OFFSET) {
    AudioClock custom_audio_clock;
    custom_audio_clock = SetPacketFactoryWithOffsetAudioClock(zx::sec(10), packet_factory2);
    ASSERT_TRUE(custom_audio_clock.is_valid());

    packet_queue2 = std::make_shared<PacketQueue>(kDefaultFormat, timeline_function,
                                                  std::move(custom_audio_clock));
  } else {
    ASSERT_TRUE(clock_mode == ClockMode::RATE_ADJUST) << "Unknown clock mode";
    ASSERT_TRUE(false) << "Multi-rate testing not yet implemented";
  }

  mix_stage_->AddInput(packet_queue1);
  mix_stage_->AddInput(packet_queue2);

  // Mix 2 packet queues with the following samples and expected outputs. We'll feed this data
  // through the mix stage in 3 passes of 2ms windows:
  //
  //       -----------------------------------
  // q1   | 0.1 | 0.2 | 0.2 | 0.3 | 0.3 | 0.3 |
  //       -----------------------------------
  // q2   | 0.7 | 0.7 | 0.7 | 0.5 | 0.5 | 0.3 |
  //       -----------------------------------
  // mix  | 0.8 | 0.9 | 0.9 | 0.8 | 0.8 | 0.6 |
  //       -----------------------------------
  // pass |     1     |     2     |     3     |
  //       -----------------------------------

  {
    packet_queue1->PushPacket(packet_factory1.CreatePacket(0.1, zx::msec(1)));
    packet_queue1->PushPacket(packet_factory1.CreatePacket(0.2, zx::msec(2)));
    packet_queue1->PushPacket(packet_factory1.CreatePacket(0.3, zx::msec(3)));
  }
  {
    packet_queue2->PushPacket(packet_factory2.CreatePacket(0.7, zx::msec(3)));
    packet_queue2->PushPacket(packet_factory2.CreatePacket(0.5, zx::msec(2)));
    packet_queue2->PushPacket(packet_factory2.CreatePacket(0.3, zx::msec(1)));
  }

  int64_t output_frame_start = 0;
  uint32_t output_frame_count = 96;
  {  // Mix frames 0-2ms. Expect 1 ms of 0.8 values, then 1 ms of 0.9 values.
    auto buf =
        mix_stage_->ReadLock(time_until(zx::msec(2)), output_frame_start, output_frame_count);
    // 1ms @ 48000hz == 48 frames. 2ms == 96 (frames).
    ASSERT_TRUE(buf);
    ASSERT_EQ(buf->length().Floor(), 96u);
    // Each frame is 2 channels, so 1ms will be 96 samples.
    auto& arr1 = as_array<float, 96>(buf->payload(), 0);
    EXPECT_THAT(arr1, Each(FloatEq(0.8f)))
        << std::setprecision(5) << "[0] " << arr1[0] << ", [1] " << arr1[1] << ", [94] " << arr1[94]
        << ", [95] " << arr1[95];
    auto& arr2 = as_array<float, 96>(buf->payload(), 96);
    EXPECT_THAT(arr2, Each(FloatEq(0.9f)))
        << std::setprecision(5) << "[0] " << arr2[0] << ", [1] " << arr2[1] << ", [94] " << arr2[94]
        << ", [95] " << arr2[95];
  }

  output_frame_start += output_frame_count;
  {  // Mix frames 2-4ms. Expect 1 ms of 0.9 samples, then 1 ms of 0.8 values.
    auto buf =
        mix_stage_->ReadLock(time_until(zx::msec(4)), output_frame_start, output_frame_count);
    ASSERT_TRUE(buf);
    ASSERT_EQ(buf->length().Floor(), 96u);

    auto& arr1 = as_array<float, 96>(buf->payload(), 0);
    EXPECT_THAT(arr1, Each(FloatEq(0.9f)))
        << std::setprecision(5) << "[0] " << arr1[0] << ", [1] " << arr1[1] << ", [94] " << arr1[94]
        << ", [95] " << arr1[95];
    auto& arr2 = as_array<float, 96>(buf->payload(), 96);
    EXPECT_THAT(arr2, Each(FloatEq(0.8f)))
        << std::setprecision(5) << "[0] " << arr2[0] << ", [1] " << arr2[1] << ", [94] " << arr2[94]
        << ", [95] " << arr2[95];
    ;
  }

  output_frame_start += output_frame_count;
  {  // Mix frames 4-6ms. Expect 1 ms of 0.8 values, then 1 ms of 0.6 values.
    auto buf =
        mix_stage_->ReadLock(time_until(zx::msec(6)), output_frame_start, output_frame_count);
    ASSERT_TRUE(buf);
    ASSERT_EQ(buf->length().Floor(), 96u);

    auto& arr1 = as_array<float, 96>(buf->payload(), 0);
    EXPECT_THAT(arr1, Each(FloatEq(0.8f)))
        << std::setprecision(5) << "[0] " << arr1[0] << ", [1] " << arr1[1] << ", [94] " << arr1[94]
        << ", [95] " << arr1[95];
    auto& arr2 = as_array<float, 96>(buf->payload(), 96);
    EXPECT_THAT(arr2, Each(FloatEq(0.6f)))
        << std::setprecision(5) << "[0] " << arr2[0] << ", [1] " << arr2[1] << ", [94] " << arr2[94]
        << ", [95] " << arr2[95];
  }

  // Upon any fail, slab_allocator asserts at exit. Clear all allocations, so testing can continue.
  mix_stage_->Trim(zx::time::infinite());
}

TEST_F(MixStageTest, MixUniformFormats) { TestMixStageUniformFormats(ClockMode::SAME); }
TEST_F(MixStageTest, MixUniformFormats_ClockOffset) {
  TestMixStageUniformFormats(ClockMode::WITH_OFFSET);
}

// Validate that a mixer with significant filter width can pull from a source buffer in pieces
// (assuming there is sufficient additional read-ahead data to satisfy the filter width!).
TEST_F(MixStageTest, MixFromRingBuffersSinc) {
  // Note: there are non-obvious constraints on the size of this ring because of how we test below.
  // In ReadLock we specify both a number of frames AND a source reference time to not read beyond.
  // We specify to read at most 1 msec of source, while specifying a number-of-frames well less than
  // that. However, filter width is included in these calculations, which means that:
  // *** Half of the ring duration, PLUS the mixer filter width, must not exceed 1 msec of source.
  // Currently SincSampler's positive_width is 13 frames so (at 48k) our ring must be <= 70 frames.
  // This test should be adjusted if SincSampler's filter width increases.
  constexpr uint32_t kRingSizeFrames = 64;
  constexpr uint32_t kRingSizeSamples = kRingSizeFrames * kDefaultNumChannels;

  // Create a new RingBuffer and add it to our mix stage.
  auto ring_buffer_endpoints = BaseRingBuffer::AllocateSoftwareBuffer(
      kDefaultFormat, timeline_function_, device_clock_, kRingSizeFrames);

  // We explictly request a SincSampler here to get a non-trivial filter width.
  mix_stage_->AddInput(ring_buffer_endpoints.reader, Mixer::Resampler::WindowedSinc);

  // Fill up the ring buffer with non-empty samples so we can observe them in the mix output.
  // The first half of the ring is one value, the second half is another.
  constexpr float kRingBufferSampleValue1 = 0.5;
  constexpr float kRingBufferSampleValue2 = 0.7;
  float* ring_buffer_samples = reinterpret_cast<float*>(ring_buffer_endpoints.writer->virt());
  for (size_t sample = 0; sample < kRingSizeSamples / 2; ++sample) {
    ring_buffer_samples[sample] = kRingBufferSampleValue1;
    ring_buffer_samples[kRingSizeSamples / 2 + sample] = kRingBufferSampleValue2;
  }

  // Read the ring in two halves, each is assigned a different source value in the ring above.
  constexpr uint32_t kRequestedFrames = kRingSizeFrames / 2;
  {
    auto buf = mix_stage_->ReadLock(time_until(zx::msec(1)), 0, kRequestedFrames);
    ASSERT_TRUE(buf);
    ASSERT_EQ(buf->start().Floor(), 0u);
    ASSERT_EQ(buf->length().Floor(), kRequestedFrames);

    auto& arr = as_array<float, kRequestedFrames * kDefaultNumChannels>(buf->payload(), 0);
    EXPECT_THAT(arr, Each(FloatEq(kRingBufferSampleValue1)))
        << std::setprecision(5) << "[0] " << arr[0] << ", [" << (arr.size() - 1) << "] "
        << arr[arr.size() - 1];
  }

  {
    auto buf = mix_stage_->ReadLock(time_until(zx::msec(2)), kRequestedFrames, kRequestedFrames);
    ASSERT_TRUE(buf);
    ASSERT_EQ(buf->start().Floor(), kRequestedFrames);
    ASSERT_EQ(buf->length().Floor(), kRequestedFrames);

    auto& arr = as_array<float, kRequestedFrames * kDefaultNumChannels>(buf->payload(), 0);
    EXPECT_THAT(arr, Each(FloatEq(kRingBufferSampleValue2)))
        << std::setprecision(5) << "[0] " << arr[0] << ", [" << (arr.size() - 1) << "] "
        << arr[arr.size() - 1];
  }
}

TEST_F(MixStageTest, MixNoInputs) {
  // Set timeline rate to match our format.
  auto timeline_function = fbl::MakeRefCounted<VersionedTimelineFunction>(TimelineFunction(
      TimelineRate(FractionalFrames<uint32_t>(kDefaultFormat.frames_per_second()).raw_value(),
                   zx::sec(1).to_nsecs())));

  constexpr uint32_t kRequestedFrames = 48;
  auto buf = mix_stage_->ReadLock(zx::time(0), 0, kRequestedFrames);

  // With no inputs, we should have a muted buffer with no usages.
  ASSERT_TRUE(buf);
  EXPECT_TRUE(buf->usage_mask().is_empty());
  EXPECT_FLOAT_EQ(buf->gain_db(), fuchsia::media::audio::MUTED_GAIN_DB);
}

static constexpr auto kInputStreamUsage = StreamUsage::WithRenderUsage(RenderUsage::INTERRUPTION);
void MixStageTest::TestMixStageSingleInput(ClockMode clock_mode) {
  // Set timeline rate to match our format.
  auto timeline_function = fbl::MakeRefCounted<VersionedTimelineFunction>(TimelineFunction(
      TimelineRate(FractionalFrames<uint32_t>(kDefaultFormat.frames_per_second()).raw_value(),
                   zx::sec(1).to_nsecs())));

  testing::PacketFactory packet_factory(dispatcher(), kDefaultFormat, PAGE_SIZE);
  std::shared_ptr<PacketQueue> packet_queue;

  if (clock_mode == ClockMode::SAME) {
    packet_queue =
        std::make_shared<PacketQueue>(kDefaultFormat, timeline_function, CreateClientClock());
  } else if (clock_mode == ClockMode::WITH_OFFSET) {
    AudioClock custom_audio_clock =
        SetPacketFactoryWithOffsetAudioClock(zx::sec(5), packet_factory);
    ASSERT_TRUE(custom_audio_clock.is_valid());

    packet_queue = std::make_shared<PacketQueue>(kDefaultFormat, timeline_function,
                                                 std::move(custom_audio_clock));
  } else {
    ASSERT_TRUE(clock_mode == ClockMode::RATE_ADJUST) << "Unknown clock mode";
    ASSERT_TRUE(false) << "Multi-rate testing not yet implemented";
  }

  packet_queue->set_usage(kInputStreamUsage);
  mix_stage_->AddInput(packet_queue);

  packet_queue->PushPacket(packet_factory.CreatePacket(1.0, zx::msec(5)));

  constexpr uint32_t kRequestedFrames = 48;
  auto buf = mix_stage_->ReadLock(zx::time(0), 0, kRequestedFrames);
  ASSERT_TRUE(buf);
  EXPECT_TRUE(buf->usage_mask().contains(kInputStreamUsage));
  EXPECT_FLOAT_EQ(buf->gain_db(), Gain::kUnityGainDb);

  // Upon any fail, slab_allocator asserts at exit. Clear all allocations, so testing can continue.
  mix_stage_->Trim(zx::time::infinite());
  mix_stage_->RemoveInput(*packet_queue);
}

TEST_F(MixStageTest, MixSingleInput) { TestMixStageSingleInput(ClockMode::SAME); }
TEST_F(MixStageTest, MixSingleInput_ClockOffset) {
  TestMixStageSingleInput(ClockMode::WITH_OFFSET);
}

TEST_F(MixStageTest, MixMultipleInputs) {
  // Set timeline rate to match our format.
  auto timeline_function = TimelineFunction(
      TimelineRate(FractionalFrames<uint32_t>(kDefaultFormat.frames_per_second()).raw_value(),
                   zx::sec(1).to_nsecs()));

  auto input1 = std::make_shared<testing::FakeStream>(kDefaultFormat, PAGE_SIZE);
  input1->timeline_function()->Update(timeline_function);
  auto input2 = std::make_shared<testing::FakeStream>(kDefaultFormat, PAGE_SIZE);
  input2->timeline_function()->Update(timeline_function);
  mix_stage_->AddInput(input1);
  mix_stage_->AddInput(input2);

  constexpr uint32_t kRequestedFrames = 48;

  // The buffer should return the union of the usage mask, and the largest of the input gains.
  input1->set_usage_mask(StreamUsageMask({StreamUsage::WithRenderUsage(RenderUsage::MEDIA)}));
  input1->set_gain_db(-20);
  input2->set_usage_mask(
      StreamUsageMask({StreamUsage::WithRenderUsage(RenderUsage::COMMUNICATION)}));
  input2->set_gain_db(-15);
  {
    auto buf = mix_stage_->ReadLock(zx::time(0), 0, kRequestedFrames);
    ASSERT_TRUE(buf);
    EXPECT_EQ(buf->usage_mask(), StreamUsageMask({
                                     StreamUsage::WithRenderUsage(RenderUsage::MEDIA),
                                     StreamUsage::WithRenderUsage(RenderUsage::COMMUNICATION),
                                 }));
    EXPECT_FLOAT_EQ(buf->gain_db(), -15);
  }
}

}  // namespace
}  // namespace media::audio
