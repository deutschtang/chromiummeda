// Copyright (c) 2009 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <deque>

#include "base/singleton.h"
#include "base/tuple.h"
#include "media/base/filter_host.h"
#include "media/base/filters.h"
#include "media/base/mock_filter_host.h"
#include "media/base/mock_media_filters.h"
#include "media/filters/ffmpeg_common.h"
#include "media/filters/ffmpeg_demuxer.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

// Simulates a queue of media packets that get "demuxed" when av_read_frame()
// is called.  It also tracks the number of packets read but not released,
// which lets us test for memory leaks and handling seeks.
class PacketQueue : public Singleton<PacketQueue> {
 public:
  bool IsEmpty() {
    return packets_.empty();
  }

  void Enqueue(int stream, size_t size, uint8* data) {
    packets_.push_back(PacketTuple(stream, size, data));
  }

  void Dequeue(AVPacket* packet) {
    CHECK(!packets_.empty());
    memset(packet, 0, sizeof(*packet));
    packet->stream_index = packets_.front().a;
    packet->size = packets_.front().b;
    packet->data = packets_.front().c;
    packet->destruct = &PacketQueue::DestructPacket;
    packets_.pop_front();

    // We now have an outstanding packet which must be freed at some point.
    ++outstanding_packets_;
  }

  bool WaitForOutstandingPackets(int count) {
    const base::TimeDelta kTimedWait = base::TimeDelta::FromMilliseconds(500);
    while (outstanding_packets_ != count) {
      if (!wait_for_outstanding_packets_.TimedWait(kTimedWait)) {
        return false;
      }
    }
    return true;
  }

 private:
  static void DestructPacket(AVPacket* packet) {
    PacketQueue::get()->DestructPacket();
  }

  void DestructPacket() {
    --outstanding_packets_;
    wait_for_outstanding_packets_.Signal();
  }

  // Only allow Singleton to create and delete PacketQueue.
  friend struct DefaultSingletonTraits<PacketQueue>;

  PacketQueue()
      : outstanding_packets_(0),
        wait_for_outstanding_packets_(false, false) {
  }

  ~PacketQueue() {
    CHECK(outstanding_packets_ == 0);
  }

  // Packet queue for tests to enqueue mock packets, which are dequeued when
  // FFmpegDemuxer calls av_read_frame().
  typedef Tuple3<int, size_t, uint8*> PacketTuple;
  std::deque<PacketTuple> packets_;

  // Counts the number of packets "allocated" by av_read_frame() and "released"
  // by av_free_packet().  This should always be zero after everything is
  // cleaned up.
  int outstanding_packets_;

  // Tests can wait on this event until a specific number of outstanding packets
  // have been reached.  Used to ensure other threads release their references
  // to objects so we don't get false positive test results when comparing the
  // number of outstanding packets.
  base::WaitableEvent wait_for_outstanding_packets_;

  DISALLOW_COPY_AND_ASSIGN(PacketQueue);
};

}  // namespace

// FFmpeg mocks to remove dependency on having the DLLs present.
extern "C" {
static const size_t kMaxStreams = 3;
static AVFormatContext g_format;
static AVStream g_streams[kMaxStreams];
static AVCodecContext g_audio_codec;
static AVCodecContext g_video_codec;
static AVCodecContext g_data_codec;

// FFmpeg return codes for various functions.
static int g_av_open_input_file = 0;
static int g_av_find_stream_info = 0;
static int g_av_read_frame = 0;
static int g_av_seek_frame = 0;

// Expected values when seeking.
static base::WaitableEvent* g_seek_event = NULL;
static int64_t g_expected_seek_timestamp = 0;
static int g_expected_seek_flags = 0;

int av_open_input_file(AVFormatContext** format, const char* filename,
                       AVInputFormat* input_format, int buffer_size,
                       AVFormatParameters* parameters) {
  EXPECT_FALSE(input_format) << "AVInputFormat should be NULL.";
  EXPECT_FALSE(buffer_size) << "buffer_size should be 0.";
  EXPECT_FALSE(parameters) << "AVFormatParameters should be NULL.";
  if (g_av_open_input_file < 0) {
    *format = NULL;
  } else {
    *format = &g_format;
  }
  return g_av_open_input_file;
}

int av_find_stream_info(AVFormatContext* format) {
  EXPECT_EQ(&g_format, format);
  return g_av_find_stream_info;
}

void av_free(void* ptr) {
  if (ptr) {
    EXPECT_EQ(&g_format, ptr);
  }
}

int av_read_frame(AVFormatContext* format, AVPacket* packet) {
  EXPECT_EQ(&g_format, format);
  if (g_av_read_frame == 0) {
    PacketQueue::get()->Dequeue(packet);
  }
  return g_av_read_frame;
}

int av_seek_frame(AVFormatContext *format, int stream_index, int64_t timestamp,
                  int flags) {
  EXPECT_EQ(&g_format, format);
  EXPECT_EQ(-1, stream_index);  // Should always use -1 for default stream.
  EXPECT_EQ(g_expected_seek_timestamp, timestamp);
  EXPECT_EQ(g_expected_seek_flags, flags);
  EXPECT_FALSE(g_seek_event->IsSignaled());
  g_seek_event->Signal();
  return g_av_seek_frame;
}

}  // extern "C"

using namespace media;

namespace {

void InitializeFFmpegMocks() {
  // Initialize function return codes.
  g_av_open_input_file = 0;
  g_av_find_stream_info = 0;
  g_av_read_frame = 0;

  // Initialize AVFormatContext structure.
  memset(&g_format, 0, sizeof(g_format));

  // Initialize AVStream structures.
  for (size_t i = 0; i < kMaxStreams; ++i) {
    memset(&g_streams[i], 0, sizeof(g_streams[i]));
    g_streams[i].time_base.den = 1 * base::Time::kMicrosecondsPerSecond;
    g_streams[i].time_base.num = 1;
  }

  // Initialize AVCodexContext structures.
  memset(&g_audio_codec, 0, sizeof(g_audio_codec));
  g_audio_codec.codec_type = CODEC_TYPE_AUDIO;
  g_audio_codec.codec_id = CODEC_ID_VORBIS;
  g_audio_codec.channels = 2;
  g_audio_codec.sample_rate = 44100;

  memset(&g_video_codec, 0, sizeof(g_video_codec));
  g_video_codec.codec_type = CODEC_TYPE_VIDEO;
  g_video_codec.codec_id = CODEC_ID_THEORA;
  g_video_codec.height = 720;
  g_video_codec.width = 1280;

  memset(&g_data_codec, 0, sizeof(g_data_codec));
  g_data_codec.codec_type = CODEC_TYPE_DATA;
  g_data_codec.codec_id = CODEC_ID_NONE;
}

// Ref counted object so we can create callbacks to call DemuxerStream::Read().
class TestReader : public base::RefCountedThreadSafe<TestReader> {
 public:
  TestReader()
      : called_(false),
        expecting_call_(false),
        wait_for_read_(false, false) {
  }

  virtual ~TestReader() {}

  void Reset() {
    EXPECT_FALSE(expecting_call_);
    expecting_call_ = false;
    called_ = false;
    buffer_ = NULL;
    wait_for_read_.Reset();
  }

  void Read(DemuxerStream* stream) {
    EXPECT_FALSE(expecting_call_);
    called_ = false;
    expecting_call_ = true;
    stream->Read(NewCallback(this, &TestReader::ReadComplete));
  }

  void ReadComplete(Buffer* buffer) {
    EXPECT_FALSE(called_);
    EXPECT_TRUE(expecting_call_);
    expecting_call_ = false;
    called_ = true;
    buffer_ = buffer;
    wait_for_read_.Signal();
  }

  bool WaitForRead() {
    return wait_for_read_.TimedWait(base::TimeDelta::FromMilliseconds(500));
  }

  // Mock getters/setters.
  Buffer* buffer() { return buffer_; }
  bool called() { return called_; }
  bool expecting_call() { return expecting_call_; }

 private:
  scoped_refptr<Buffer> buffer_;
  bool called_;
  bool expecting_call_;
  base::WaitableEvent wait_for_read_;
};

}  // namespace

TEST(FFmpegDemuxerTest, InitializeFailure) {
  InitializeFFmpegMocks();

  // Get FFmpegDemuxer's filter factory.
  scoped_refptr<FilterFactory> factory = FFmpegDemuxer::CreateFilterFactory();

  // Should only accept application/octet-stream type.
  MediaFormat media_format;
  media_format.SetAsString(MediaFormat::kMimeType, "foo/x-bar");
  scoped_refptr<Demuxer> demuxer(factory->Create<Demuxer>(media_format));
  ASSERT_FALSE(demuxer);
  media_format.Clear();
  media_format.SetAsString(MediaFormat::kMimeType,
                           mime_type::kApplicationOctetStream);
  demuxer = factory->Create<Demuxer>(media_format);
  ASSERT_TRUE(demuxer);

  // Prepare a filter host and data source for the demuxer.
  MockPipeline pipeline;
  scoped_ptr< MockFilterHost<Demuxer> > filter_host;
  filter_host.reset(new MockFilterHost<Demuxer>(&pipeline, demuxer));
  MockFilterConfig config;
  scoped_refptr<MockDataSource> data_source(new MockDataSource(&config));

  // Simulate av_open_input_fail failing.
  g_av_open_input_file = AVERROR_IO;
  g_av_find_stream_info = 0;
  EXPECT_FALSE(demuxer->Initialize(data_source));
  EXPECT_FALSE(filter_host->IsInitialized());
  EXPECT_EQ(DEMUXER_ERROR_COULD_NOT_OPEN, pipeline.GetError());

  // Simulate av_find_stream_info failing.
  g_av_open_input_file = 0;
  g_av_find_stream_info = AVERROR_IO;
  demuxer = factory->Create<Demuxer>(media_format);
  filter_host.reset(new MockFilterHost<Demuxer>(&pipeline, demuxer));
  EXPECT_FALSE(demuxer->Initialize(data_source));
  EXPECT_FALSE(filter_host->IsInitialized());
  EXPECT_EQ(DEMUXER_ERROR_COULD_NOT_PARSE, pipeline.GetError());

  // Simulate media with no parseable streams.
  InitializeFFmpegMocks();
  demuxer = factory->Create<Demuxer>(media_format);
  filter_host.reset(new MockFilterHost<Demuxer>(&pipeline, demuxer));
  EXPECT_FALSE(demuxer->Initialize(data_source));
  EXPECT_FALSE(filter_host->IsInitialized());
  EXPECT_EQ(DEMUXER_ERROR_NO_SUPPORTED_STREAMS, pipeline.GetError());

  // Simulate media with a data stream but no audio or video streams.
  g_format.nb_streams = 1;
  g_format.streams[0] = &g_streams[0];
  g_streams[0].codec = &g_data_codec;
  g_streams[0].duration = 10;
  demuxer = factory->Create<Demuxer>(media_format);
  filter_host.reset(new MockFilterHost<Demuxer>(&pipeline, demuxer));
  EXPECT_FALSE(demuxer->Initialize(data_source));
  EXPECT_FALSE(filter_host->IsInitialized());
  EXPECT_EQ(DEMUXER_ERROR_NO_SUPPORTED_STREAMS, pipeline.GetError());
}

TEST(FFmpegDemuxerTest, InitializeStreams) {
  // Simulate media with a data stream, a video stream and audio stream.
  InitializeFFmpegMocks();
  g_format.nb_streams = 3;
  g_format.streams[0] = &g_streams[0];
  g_format.streams[1] = &g_streams[1];
  g_format.streams[2] = &g_streams[2];
  g_streams[0].duration = 1000;
  g_streams[0].codec = &g_data_codec;
  g_streams[1].duration = 100;
  g_streams[1].codec = &g_video_codec;
  g_streams[2].duration = 10;
  g_streams[2].codec = &g_audio_codec;

  // Create our pipeline.
  MockPipeline pipeline;

  // Create our data source.
  MockFilterConfig config;
  scoped_refptr<MockDataSource> data_source = new MockDataSource(&config);
  MockFilterHost<DataSource> filter_host_a(&pipeline, data_source);
  EXPECT_TRUE(data_source->Initialize("foo"));
  EXPECT_TRUE(filter_host_a.IsInitialized());

  // Create our demuxer.
  scoped_refptr<FilterFactory> factory = FFmpegDemuxer::CreateFilterFactory();
  scoped_refptr<Demuxer> demuxer
      = factory->Create<Demuxer>(data_source->media_format());
  EXPECT_TRUE(demuxer);
  MockFilterHost<Demuxer> filter_host_b(&pipeline, demuxer);
  EXPECT_TRUE(demuxer->Initialize(data_source));
  EXPECT_TRUE(filter_host_b.IsInitialized());
  EXPECT_EQ(PIPELINE_OK, pipeline.GetError());

  // Since we ignore data streams, the duration should be equal to the video
  // stream's duration.
  EXPECT_EQ(g_streams[1].duration, pipeline.GetDuration().InMicroseconds());

  // Verify that 2 out of 3 streams were created.
  EXPECT_EQ(2, demuxer->GetNumberOfStreams());

  // First stream should be video and support FFmpegDemuxerStream interface.
  scoped_refptr<DemuxerStream> stream = demuxer->GetStream(0);
  scoped_refptr<FFmpegDemuxerStream> ffmpeg_demuxer_stream;
  ASSERT_TRUE(stream);
  std::string mime_type;
  int result;
  EXPECT_TRUE(
      stream->media_format().GetAsString(MediaFormat::kMimeType, &mime_type));
  EXPECT_STREQ(mime_type::kFFmpegVideo, mime_type.c_str());
  EXPECT_TRUE(
      stream->media_format().GetAsInteger(kFFmpegCodecID, &result));
  EXPECT_EQ(CODEC_ID_THEORA, static_cast<CodecID>(result));
  EXPECT_TRUE(
      stream->media_format().GetAsInteger(MediaFormat::kHeight, &result));
  EXPECT_EQ(g_video_codec.height, result);
  EXPECT_TRUE(
      stream->media_format().GetAsInteger(MediaFormat::kWidth, &result));
  EXPECT_EQ(g_video_codec.width, result);
  EXPECT_TRUE(stream->QueryInterface(&ffmpeg_demuxer_stream));
  EXPECT_TRUE(ffmpeg_demuxer_stream);
  EXPECT_EQ(&g_streams[1], ffmpeg_demuxer_stream->av_stream());

  // Second stream should be audio and support FFmpegDemuxerStream interface.
  stream = demuxer->GetStream(1);
  ffmpeg_demuxer_stream = NULL;
  ASSERT_TRUE(stream);
  EXPECT_TRUE(
      stream->media_format().GetAsString(MediaFormat::kMimeType, &mime_type));
  EXPECT_STREQ(mime_type::kFFmpegAudio, mime_type.c_str());
  EXPECT_TRUE(
      stream->media_format().GetAsInteger(kFFmpegCodecID, &result));
  EXPECT_EQ(CODEC_ID_VORBIS, static_cast<CodecID>(result));
  EXPECT_TRUE(
      stream->media_format().GetAsInteger(MediaFormat::kChannels, &result));
  EXPECT_EQ(g_audio_codec.channels, result);
  EXPECT_TRUE(
      stream->media_format().GetAsInteger(MediaFormat::kSampleRate, &result));
  EXPECT_EQ(g_audio_codec.sample_rate, result);
  EXPECT_TRUE(stream->QueryInterface(&ffmpeg_demuxer_stream));
  EXPECT_TRUE(ffmpeg_demuxer_stream);
  EXPECT_EQ(&g_streams[2], ffmpeg_demuxer_stream->av_stream());
}

// TODO(scherkus): as we keep refactoring and improving our mocks (both FFmpeg
// and pipeline/filters), try to break this test into two.  Big issue right now
// is that it takes ~50 lines of code just to set up FFmpegDemuxer.
TEST(FFmpegDemuxerTest, ReadAndSeek) {
  // Prepare some test data.
  const int kAudio = 0;
  const int kVideo = 1;
  const size_t kDataSize = 4;
  uint8 audio_data[kDataSize] = {0, 1, 2, 3};
  uint8 video_data[kDataSize] = {4, 5, 6, 7};

  // Simulate media with a an audio stream and video stream.
  InitializeFFmpegMocks();
  g_format.nb_streams = 2;
  g_format.streams[kAudio] = &g_streams[kAudio];
  g_format.streams[kVideo] = &g_streams[kVideo];
  g_streams[kAudio].duration = 10;
  g_streams[kAudio].codec = &g_audio_codec;
  g_streams[kVideo].duration = 10;
  g_streams[kVideo].codec = &g_video_codec;

  // Create our pipeline.
  MockPipeline pipeline;

  // Create our data source.
  MockFilterConfig config;
  scoped_refptr<MockDataSource> data_source = new MockDataSource(&config);
  MockFilterHost<DataSource> filter_host_a(&pipeline, data_source);
  EXPECT_TRUE(data_source->Initialize("foo"));
  EXPECT_TRUE(filter_host_a.IsInitialized());

  // Create our demuxer.
  scoped_refptr<FilterFactory> factory = FFmpegDemuxer::CreateFilterFactory();
  scoped_refptr<Demuxer> demuxer
      = factory->Create<Demuxer>(data_source->media_format());
  EXPECT_TRUE(demuxer);
  MockFilterHost<Demuxer> filter_host_b(&pipeline, demuxer);
  EXPECT_TRUE(demuxer->Initialize(data_source));
  EXPECT_TRUE(filter_host_b.IsInitialized());
  EXPECT_EQ(PIPELINE_OK, pipeline.GetError());

  // Verify both streams were created.
  EXPECT_EQ(2, demuxer->GetNumberOfStreams());

  // Get our streams.
  scoped_refptr<DemuxerStream> audio_stream = demuxer->GetStream(kAudio);
  scoped_refptr<DemuxerStream> video_stream = demuxer->GetStream(kVideo);
  ASSERT_TRUE(audio_stream);
  ASSERT_TRUE(video_stream);

  // Prepare our test audio packet.
  PacketQueue::get()->Enqueue(kAudio, kDataSize, audio_data);

  // Attempt a read from the audio stream and run the message loop until done.
  scoped_refptr<TestReader> reader(new TestReader());
  reader->Read(audio_stream);
  pipeline.RunAllTasks();
  EXPECT_TRUE(reader->WaitForRead());
  EXPECT_TRUE(reader->called());
  ASSERT_TRUE(reader->buffer());
  EXPECT_FALSE(reader->buffer()->IsDiscontinuous());
  EXPECT_EQ(audio_data, reader->buffer()->GetData());
  EXPECT_EQ(kDataSize, reader->buffer()->GetDataSize());

  // Prepare our test video packet.
  PacketQueue::get()->Enqueue(kVideo, kDataSize, video_data);

  // Attempt a read from the video stream and run the message loop until done.
  reader->Reset();
  reader->Read(video_stream);
  pipeline.RunAllTasks();
  EXPECT_TRUE(reader->WaitForRead());
  EXPECT_TRUE(reader->called());
  ASSERT_TRUE(reader->buffer());
  EXPECT_FALSE(reader->buffer()->IsDiscontinuous());
  EXPECT_EQ(video_data, reader->buffer()->GetData());
  EXPECT_EQ(kDataSize, reader->buffer()->GetDataSize());

  // Manually release buffer, which should release any remaining AVPackets.
  reader = NULL;
  EXPECT_TRUE(PacketQueue::get()->WaitForOutstandingPackets(0));

  //----------------------------------------------------------------------------
  // Seek tests.
  EXPECT_FALSE(g_seek_event);
  g_seek_event = new base::WaitableEvent(false, false);

  // Let's trigger a simple forward seek with no outstanding packets.
  g_expected_seek_timestamp = 1234;
  g_expected_seek_flags = 0;
  demuxer->Seek(base::TimeDelta::FromMicroseconds(g_expected_seek_timestamp));
  EXPECT_TRUE(g_seek_event->TimedWait(base::TimeDelta::FromSeconds(1)));

  // The next read from each stream should now be discontinuous, but subsequent
  // reads should not.

  // Prepare our test audio packet.
  PacketQueue::get()->Enqueue(kAudio, kDataSize, audio_data);
  PacketQueue::get()->Enqueue(kAudio, kDataSize, audio_data);

  // Audio read #1, should be discontinuous.
  reader = new TestReader();
  reader->Read(audio_stream);
  pipeline.RunAllTasks();
  EXPECT_TRUE(reader->WaitForRead());
  EXPECT_TRUE(reader->called());
  ASSERT_TRUE(reader->buffer());
  EXPECT_TRUE(reader->buffer()->IsDiscontinuous());
  EXPECT_EQ(audio_data, reader->buffer()->GetData());
  EXPECT_EQ(kDataSize, reader->buffer()->GetDataSize());

  // Audio read #2, should not be discontinuous.
  reader->Reset();
  reader->Read(audio_stream);
  pipeline.RunAllTasks();
  EXPECT_TRUE(reader->WaitForRead());
  EXPECT_TRUE(reader->called());
  ASSERT_TRUE(reader->buffer());
  EXPECT_FALSE(reader->buffer()->IsDiscontinuous());
  EXPECT_EQ(audio_data, reader->buffer()->GetData());
  EXPECT_EQ(kDataSize, reader->buffer()->GetDataSize());

  // Prepare our test video packet.
  PacketQueue::get()->Enqueue(kVideo, kDataSize, video_data);
  PacketQueue::get()->Enqueue(kVideo, kDataSize, video_data);

  // Video read #1, should be discontinuous.
  reader->Reset();
  reader->Read(video_stream);
  pipeline.RunAllTasks();
  EXPECT_TRUE(reader->WaitForRead());
  EXPECT_TRUE(reader->called());
  ASSERT_TRUE(reader->buffer());
  EXPECT_TRUE(reader->buffer()->IsDiscontinuous());
  EXPECT_EQ(video_data, reader->buffer()->GetData());
  EXPECT_EQ(kDataSize, reader->buffer()->GetDataSize());

  // Video read #2, should not be discontinuous.
  reader->Reset();
  reader->Read(video_stream);
  pipeline.RunAllTasks();
  EXPECT_TRUE(reader->WaitForRead());
  EXPECT_TRUE(reader->called());
  ASSERT_TRUE(reader->buffer());
  EXPECT_FALSE(reader->buffer()->IsDiscontinuous());
  EXPECT_EQ(video_data, reader->buffer()->GetData());
  EXPECT_EQ(kDataSize, reader->buffer()->GetDataSize());

  // Manually release buffer, which should release any remaining AVPackets.
  reader = NULL;
  EXPECT_TRUE(PacketQueue::get()->WaitForOutstandingPackets(0));

  // Let's trigger another simple forward seek, but with outstanding packets.
  // The outstanding packets should get freed after the Seek() is issued.
  PacketQueue::get()->Enqueue(kAudio, kDataSize, audio_data);
  PacketQueue::get()->Enqueue(kAudio, kDataSize, audio_data);
  PacketQueue::get()->Enqueue(kAudio, kDataSize, audio_data);
  PacketQueue::get()->Enqueue(kVideo, kDataSize, video_data);

  // Attempt a read from video stream, which will force the demuxer to queue
  // the audio packets preceding the video packet.
  reader = new TestReader();
  reader->Read(video_stream);
  pipeline.RunAllTasks();
  EXPECT_TRUE(reader->WaitForRead());
  EXPECT_TRUE(reader->called());
  ASSERT_TRUE(reader->buffer());
  EXPECT_FALSE(reader->buffer()->IsDiscontinuous());
  EXPECT_EQ(video_data, reader->buffer()->GetData());
  EXPECT_EQ(kDataSize, reader->buffer()->GetDataSize());

  // Manually release video buffer, remaining audio packets are outstanding.
  reader = NULL;
  EXPECT_TRUE(PacketQueue::get()->WaitForOutstandingPackets(3));

  // Trigger the seek.
  g_expected_seek_timestamp = 1234;
  g_expected_seek_flags = 0;
  demuxer->Seek(base::TimeDelta::FromMicroseconds(g_expected_seek_timestamp));
  EXPECT_TRUE(g_seek_event->TimedWait(base::TimeDelta::FromSeconds(1)));

  // All outstanding packets should have been freed.
  EXPECT_TRUE(PacketQueue::get()->WaitForOutstandingPackets(0));

  // Clean up.
  delete g_seek_event;
  g_seek_event = NULL;

  //----------------------------------------------------------------------------
  // End of stream tests.

  // Simulate end of stream.
  g_av_read_frame = AVERROR_IO;

  // Attempt a read from the audio stream and run the message loop until done.
  reader = new TestReader();
  reader->Read(audio_stream);
  pipeline.RunAllTasks();
  EXPECT_FALSE(reader->WaitForRead());
  EXPECT_FALSE(reader->called());
  EXPECT_FALSE(reader->buffer());

  // Manually release buffer, which should release any remaining AVPackets.
  reader = NULL;
  EXPECT_TRUE(PacketQueue::get()->WaitForOutstandingPackets(0));
}
