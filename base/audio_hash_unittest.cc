// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/logging.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_hash.h"
#include "media/base/fake_audio_render_callback.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace media {

static const int kChannelCount = 2;
static const int kFrameCount = 1024;

class AudioHashTest : public testing::Test {
 public:
  AudioHashTest()
      : bus_one_(AudioBus::Create(kChannelCount, kFrameCount)),
        bus_two_(AudioBus::Create(kChannelCount, kFrameCount)),
        fake_callback_(0.01) {

    // Fill each channel in each bus with unique data.
    GenerateUniqueChannels(bus_one_.get());
    GenerateUniqueChannels(bus_two_.get());
  }

  void GenerateUniqueChannels(AudioBus* audio_bus) {
    // Use an AudioBus wrapper to avoid an extra memcpy when filling channels.
    scoped_ptr<AudioBus> wrapped_bus = AudioBus::CreateWrapper(1);
    wrapped_bus->set_frames(audio_bus->frames());

    // Since FakeAudioRenderCallback generates only a single channel of unique
    // audio data, we need to fill each channel manually.
    for (int ch = 0; ch < audio_bus->channels(); ++ch) {
      wrapped_bus->SetChannelData(0, audio_bus->channel(ch));
      fake_callback_.Render(wrapped_bus.get(), 0);
    }
  }

  virtual ~AudioHashTest() {}

 protected:
  scoped_ptr<AudioBus> bus_one_;
  scoped_ptr<AudioBus> bus_two_;
  FakeAudioRenderCallback fake_callback_;

  DISALLOW_COPY_AND_ASSIGN(AudioHashTest);
};

// Ensure the same data hashes the same.
TEST_F(AudioHashTest, Equivalence) {
  AudioHash hash_one;
  hash_one.Update(bus_one_.get(), bus_one_->frames());

  AudioHash hash_two;
  hash_two.Update(bus_one_.get(), bus_one_->frames());

  EXPECT_EQ(hash_one.ToString(), hash_two.ToString());
}

// Ensure sample order matters to the hash.
TEST_F(AudioHashTest, SampleOrder) {
  AudioHash original_hash;
  original_hash.Update(bus_one_.get(), bus_one_->frames());

  // Swap a sample in the bus.
  std::swap(bus_one_->channel(0)[0], bus_one_->channel(0)[1]);

  AudioHash swapped_hash;
  swapped_hash.Update(bus_one_.get(), bus_one_->frames());

  EXPECT_NE(original_hash.ToString(), swapped_hash.ToString());
}

// Ensure channel order matters to the hash.
TEST_F(AudioHashTest, ChannelOrder) {
  AudioHash original_hash;
  original_hash.Update(bus_one_.get(), bus_one_->frames());

  // Reverse channel order for the same sample data.
  const int channels = bus_one_->channels();
  scoped_ptr<AudioBus> swapped_ch_bus = AudioBus::CreateWrapper(channels);
  swapped_ch_bus->set_frames(bus_one_->frames());
  for (int i = channels - 1; i >= 0; --i)
    swapped_ch_bus->SetChannelData(channels - (i + 1), bus_one_->channel(i));

  AudioHash swapped_hash;
  swapped_hash.Update(swapped_ch_bus.get(), swapped_ch_bus->frames());

  EXPECT_NE(original_hash.ToString(), swapped_hash.ToString());
}

// Ensure bus order matters to the hash.
TEST_F(AudioHashTest, BusOrder) {
  AudioHash original_hash;
  original_hash.Update(bus_one_.get(), bus_one_->frames());
  original_hash.Update(bus_two_.get(), bus_two_->frames());

  AudioHash reordered_hash;
  reordered_hash.Update(bus_two_.get(), bus_two_->frames());
  reordered_hash.Update(bus_one_.get(), bus_one_->frames());

  EXPECT_NE(original_hash.ToString(), reordered_hash.ToString());
}

// Ensure bus order matters to the hash even with empty buses.
TEST_F(AudioHashTest, EmptyBusOrder) {
  bus_one_->Zero();
  bus_two_->Zero();

  AudioHash one_bus_hash;
  one_bus_hash.Update(bus_one_.get(), bus_one_->frames());

  AudioHash two_bus_hash;
  two_bus_hash.Update(bus_one_.get(), bus_one_->frames());
  two_bus_hash.Update(bus_two_.get(), bus_two_->frames());

  EXPECT_NE(one_bus_hash.ToString(), two_bus_hash.ToString());
}

// Ensure approximate hashes pass verification.
TEST_F(AudioHashTest, VerifySimilarHash) {
  AudioHash hash_one;
  hash_one.Update(bus_one_.get(), bus_one_->frames());

  // Twiddle the values inside the first bus.
  float* channel = bus_one_->channel(0);
  for (int i = 0; i < bus_one_->frames(); i += bus_one_->frames() / 64)
    channel[i] += 0.0001f;

  AudioHash hash_two;
  hash_two.Update(bus_one_.get(), bus_one_->frames());

  EXPECT_EQ(hash_one.ToString(), hash_two.ToString());

  // Twiddle the values too much...
  for (int i = 0; i < bus_one_->frames(); ++i)
    channel[i] += 0.0001f;

  AudioHash hash_three;
  hash_three.Update(bus_one_.get(), bus_one_->frames());
  EXPECT_NE(hash_one.ToString(), hash_three.ToString());
}

}  // namespace media