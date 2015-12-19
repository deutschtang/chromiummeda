// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/bind.h"
#include "base/message_loop/message_loop.h"
#include "media/base/decoder_buffer.h"
#include "media/base/mock_filters.h"
#include "media/base/test_helpers.h"
#include "media/base/video_frame.h"
#include "media/filters/fake_video_decoder.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace media {

static const int kTotalBuffers = 12;
static const int kDurationMs = 30;

struct FakeVideoDecoderTestParams {
  FakeVideoDecoderTestParams(int decoding_delay, int max_decode_requests)
      : decoding_delay(decoding_delay),
        max_decode_requests(max_decode_requests) {}
  int decoding_delay;
  int max_decode_requests;
};

class FakeVideoDecoderTest
    : public testing::Test,
      public testing::WithParamInterface<FakeVideoDecoderTestParams> {
 public:
  FakeVideoDecoderTest()
      : decoder_(new FakeVideoDecoder(
            GetParam().decoding_delay,
            GetParam().max_decode_requests,
            base::Bind(&FakeVideoDecoderTest::OnBytesDecoded,
                       base::Unretained(this)))),
        num_input_buffers_(0),
        num_decoded_frames_(0),
        num_bytes_decoded_(0),
        total_bytes_in_buffers_(0),
        last_decode_status_(VideoDecoder::kOk),
        pending_decode_requests_(0),
        is_reset_pending_(false) {}

  virtual ~FakeVideoDecoderTest() {
    Destroy();
  }

  void InitializeWithConfigAndExpectResult(const VideoDecoderConfig& config,
                                           bool success) {
    decoder_->Initialize(
        config, false, SetCdmReadyCB(), NewExpectedBoolCB(success),
        base::Bind(&FakeVideoDecoderTest::FrameReady, base::Unretained(this)));
    message_loop_.RunUntilIdle();
    current_config_ = config;
  }

  void Initialize() {
    InitializeWithConfigAndExpectResult(TestVideoConfig::Normal(), true);
  }

  void EnterPendingInitState() {
    decoder_->HoldNextInit();
    Initialize();
  }

  void SatisfyInit() {
    decoder_->SatisfyInit();
    message_loop_.RunUntilIdle();
  }

  // Callback for VideoDecoder::Decode().
  void DecodeDone(VideoDecoder::Status status) {
    DCHECK_GT(pending_decode_requests_, 0);
    --pending_decode_requests_;
    last_decode_status_ = status;
  }

  void FrameReady(const scoped_refptr<VideoFrame>& frame) {
    DCHECK(!frame->metadata()->IsTrue(VideoFrameMetadata::END_OF_STREAM));
    last_decoded_frame_ = frame;
    num_decoded_frames_++;
  }

  void OnBytesDecoded(int count) {
    num_bytes_decoded_ += count;
  }

  enum CallbackResult {
    PENDING,
    OK,
    NOT_ENOUGH_DATA,
    ABORTED
  };

  void ExpectReadResult(CallbackResult result) {
    switch (result) {
      case PENDING:
        EXPECT_GT(pending_decode_requests_, 0);
        break;
      case OK:
        EXPECT_EQ(0, pending_decode_requests_);
        ASSERT_EQ(VideoDecoder::kOk, last_decode_status_);
        ASSERT_TRUE(last_decoded_frame_.get());
        break;
      case NOT_ENOUGH_DATA:
        EXPECT_EQ(0, pending_decode_requests_);
        ASSERT_EQ(VideoDecoder::kOk, last_decode_status_);
        ASSERT_FALSE(last_decoded_frame_.get());
        break;
      case ABORTED:
        EXPECT_EQ(0, pending_decode_requests_);
        ASSERT_EQ(VideoDecoder::kAborted, last_decode_status_);
        EXPECT_FALSE(last_decoded_frame_.get());
        break;
    }
  }

  void Decode() {
    scoped_refptr<DecoderBuffer> buffer;

    if (num_input_buffers_ < kTotalBuffers) {
      buffer = CreateFakeVideoBufferForTest(
          current_config_,
          base::TimeDelta::FromMilliseconds(kDurationMs * num_input_buffers_),
          base::TimeDelta::FromMilliseconds(kDurationMs));
      total_bytes_in_buffers_ += buffer->data_size();
    } else {
      buffer = DecoderBuffer::CreateEOSBuffer();
    }

    ++num_input_buffers_;
    ++pending_decode_requests_;

    decoder_->Decode(
        buffer,
        base::Bind(&FakeVideoDecoderTest::DecodeDone, base::Unretained(this)));
    message_loop_.RunUntilIdle();
  }

  void ReadOneFrame() {
    last_decoded_frame_ = NULL;
    do {
      Decode();
    } while (!last_decoded_frame_.get() && pending_decode_requests_ == 0);
  }

  void ReadAllFrames() {
    do {
      Decode();
    } while (num_input_buffers_ <= kTotalBuffers); // All input buffers + EOS.
  }

  void EnterPendingReadState() {
    // Pass the initial NOT_ENOUGH_DATA stage.
    ReadOneFrame();
    decoder_->HoldDecode();
    ReadOneFrame();
    ExpectReadResult(PENDING);
  }

  void SatisfyDecodeAndExpect(CallbackResult result) {
    decoder_->SatisfyDecode();
    message_loop_.RunUntilIdle();
    ExpectReadResult(result);
  }

  void SatisfyRead() {
    SatisfyDecodeAndExpect(OK);
  }

  // Callback for VideoDecoder::Reset().
  void OnDecoderReset() {
    DCHECK(is_reset_pending_);
    is_reset_pending_ = false;
  }

  void ExpectResetResult(CallbackResult result) {
    switch (result) {
      case PENDING:
        EXPECT_TRUE(is_reset_pending_);
        break;
      case OK:
        EXPECT_FALSE(is_reset_pending_);
        break;
      default:
        NOTREACHED();
    }
  }

  void ResetAndExpect(CallbackResult result) {
    is_reset_pending_ = true;
    decoder_->Reset(base::Bind(&FakeVideoDecoderTest::OnDecoderReset,
                               base::Unretained(this)));
    message_loop_.RunUntilIdle();
    ExpectResetResult(result);
  }

  void EnterPendingResetState() {
    decoder_->HoldNextReset();
    ResetAndExpect(PENDING);
  }

  void SatisfyReset() {
    decoder_->SatisfyReset();
    message_loop_.RunUntilIdle();
    ExpectResetResult(OK);
  }

  void Destroy() {
    decoder_.reset();
    message_loop_.RunUntilIdle();

    // All pending callbacks must have been fired.
    DCHECK_EQ(pending_decode_requests_, 0);
    DCHECK(!is_reset_pending_);
  }

  base::MessageLoop message_loop_;
  VideoDecoderConfig current_config_;

  scoped_ptr<FakeVideoDecoder> decoder_;

  int num_input_buffers_;
  int num_decoded_frames_;
  int num_bytes_decoded_;
  int total_bytes_in_buffers_;

  // Callback result/status.
  VideoDecoder::Status last_decode_status_;
  scoped_refptr<VideoFrame> last_decoded_frame_;
  int pending_decode_requests_;
  bool is_reset_pending_;

 private:
  DISALLOW_COPY_AND_ASSIGN(FakeVideoDecoderTest);
};

INSTANTIATE_TEST_CASE_P(NoParallelDecode,
                        FakeVideoDecoderTest,
                        ::testing::Values(FakeVideoDecoderTestParams(9, 1),
                                          FakeVideoDecoderTestParams(0, 1)));
INSTANTIATE_TEST_CASE_P(ParallelDecode,
                        FakeVideoDecoderTest,
                        ::testing::Values(FakeVideoDecoderTestParams(9, 3),
                                          FakeVideoDecoderTestParams(0, 3)));

TEST_P(FakeVideoDecoderTest, Initialize) {
  Initialize();
}

TEST_P(FakeVideoDecoderTest, SimulateFailureToInitialize) {
  decoder_->SimulateFailureToInit();
  InitializeWithConfigAndExpectResult(TestVideoConfig::Normal(), false);
  Decode();
  EXPECT_EQ(last_decode_status_, VideoDecoder::kDecodeError);
}

TEST_P(FakeVideoDecoderTest, Read_AllFrames) {
  Initialize();
  ReadAllFrames();
  EXPECT_EQ(kTotalBuffers, num_decoded_frames_);
  EXPECT_EQ(total_bytes_in_buffers_, num_bytes_decoded_);
}

TEST_P(FakeVideoDecoderTest, Read_DecodingDelay) {
  Initialize();

  while (num_input_buffers_ < kTotalBuffers) {
    ReadOneFrame();
    EXPECT_EQ(num_input_buffers_,
              num_decoded_frames_ + GetParam().decoding_delay);
  }
}

TEST_P(FakeVideoDecoderTest, Read_ZeroDelay) {
  decoder_.reset(new FakeVideoDecoder(
      0, 1, base::Bind(&FakeVideoDecoderTest::OnBytesDecoded,
                       base::Unretained(this))));
  Initialize();

  while (num_input_buffers_ < kTotalBuffers) {
    ReadOneFrame();
    EXPECT_EQ(num_input_buffers_, num_decoded_frames_);
  }
}

TEST_P(FakeVideoDecoderTest, Read_Pending_NotEnoughData) {
  if (GetParam().decoding_delay < 1)
    return;

  Initialize();
  decoder_->HoldDecode();
  ReadOneFrame();
  ExpectReadResult(PENDING);
  SatisfyDecodeAndExpect(NOT_ENOUGH_DATA);

  // Verify that FrameReady() hasn't been called.
  EXPECT_FALSE(last_decoded_frame_.get());
}

TEST_P(FakeVideoDecoderTest, Read_Pending_OK) {
  Initialize();
  EnterPendingReadState();
  SatisfyDecodeAndExpect(OK);
}

TEST_P(FakeVideoDecoderTest, Read_Parallel) {
  if (GetParam().max_decode_requests < 2)
    return;

  Initialize();
  decoder_->HoldDecode();
  for (int i = 0; i < GetParam().max_decode_requests; ++i) {
    ReadOneFrame();
    ExpectReadResult(PENDING);
  }
  EXPECT_EQ(GetParam().max_decode_requests, pending_decode_requests_);
  SatisfyDecodeAndExpect(
      GetParam().max_decode_requests > GetParam().decoding_delay
          ? OK
          : NOT_ENOUGH_DATA);
}

TEST_P(FakeVideoDecoderTest, ReadWithHold_DecodingDelay) {
  Initialize();

  // Hold all decodes and satisfy one decode at a time.
  decoder_->HoldDecode();
  int num_decodes_satisfied = 0;
  while (num_decoded_frames_ == 0) {
    while (pending_decode_requests_ < decoder_->GetMaxDecodeRequests())
      Decode();
    decoder_->SatisfySingleDecode();
    ++num_decodes_satisfied;
    message_loop_.RunUntilIdle();
  }

  DCHECK_EQ(num_decoded_frames_, 1);
  DCHECK_EQ(num_decodes_satisfied, GetParam().decoding_delay + 1);
}

TEST_P(FakeVideoDecoderTest, Reinitialize) {
  Initialize();
  ReadOneFrame();
  InitializeWithConfigAndExpectResult(TestVideoConfig::Large(), true);
  ReadOneFrame();
}

TEST_P(FakeVideoDecoderTest, SimulateFailureToReinitialize) {
  Initialize();
  ReadOneFrame();
  decoder_->SimulateFailureToInit();
  InitializeWithConfigAndExpectResult(TestVideoConfig::Normal(), false);
  Decode();
  EXPECT_EQ(last_decode_status_, VideoDecoder::kDecodeError);
}

// Reinitializing the decoder during the middle of the decoding process can
// cause dropped frames.
TEST_P(FakeVideoDecoderTest, Reinitialize_FrameDropped) {
  if (GetParam().decoding_delay < 1)
    return;

  Initialize();
  ReadOneFrame();
  Initialize();
  ReadAllFrames();
  EXPECT_LT(num_decoded_frames_, kTotalBuffers);
}

TEST_P(FakeVideoDecoderTest, Reset) {
  Initialize();
  ReadOneFrame();
  ResetAndExpect(OK);
}

TEST_P(FakeVideoDecoderTest, Reset_DuringPendingRead) {
  Initialize();
  EnterPendingReadState();
  ResetAndExpect(PENDING);
  SatisfyDecodeAndExpect(ABORTED);
}

TEST_P(FakeVideoDecoderTest, Reset_Pending) {
  Initialize();
  EnterPendingResetState();
  SatisfyReset();
}

TEST_P(FakeVideoDecoderTest, Reset_PendingDuringPendingRead) {
  Initialize();
  EnterPendingReadState();
  EnterPendingResetState();
  SatisfyDecodeAndExpect(ABORTED);
  SatisfyReset();
}

TEST_P(FakeVideoDecoderTest, Destroy) {
  Initialize();
  ReadOneFrame();
  ExpectReadResult(OK);
  Destroy();
}

TEST_P(FakeVideoDecoderTest, Destroy_DuringPendingInitialization) {
  EnterPendingInitState();
  Destroy();
}

TEST_P(FakeVideoDecoderTest, Destroy_DuringPendingRead) {
  Initialize();
  EnterPendingReadState();
  Destroy();
}

TEST_P(FakeVideoDecoderTest, Destroy_DuringPendingReset) {
  Initialize();
  EnterPendingResetState();
  Destroy();
}

TEST_P(FakeVideoDecoderTest, Destroy_DuringPendingReadAndPendingReset) {
  Initialize();
  EnterPendingReadState();
  EnterPendingResetState();
  Destroy();
}

}  // namespace media
