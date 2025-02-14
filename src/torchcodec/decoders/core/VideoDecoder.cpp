// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "src/torchcodec/decoders/core/VideoDecoder.h"
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include "torch/types.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include "src/torchcodec/decoders/core/FFMPEGCommon.h"

namespace facebook::torchcodec {
namespace {

struct AVInput {
  UniqueAVFormatContext formatContext;
  std::unique_ptr<AVIOBytesContext> ioBytesContext;
};

AVInput createAVFormatContextFromFilePath(const std::string& videoFilePath) {
  AVFormatContext* formatContext = nullptr;
  if (avformat_open_input(
          &formatContext, videoFilePath.c_str(), nullptr, nullptr) != 0) {
    throw std::invalid_argument("Could not open input file: " + videoFilePath);
  }
  TORCH_CHECK(formatContext != nullptr);
  AVInput toReturn;
  toReturn.formatContext.reset(formatContext);
  return toReturn;
}

AVInput createAVFormatContextFromBuffer(const void* buffer, size_t length) {
  AVInput toReturn;
  toReturn.formatContext.reset(avformat_alloc_context());
  TORCH_CHECK(
      toReturn.formatContext.get() != nullptr,
      "Unable to alloc avformat context");
  // TODO(ahmads): Add an option to control this size.
  constexpr int kAVIOInternalTemporaryBufferSize = 1024 * 1024;
  toReturn.ioBytesContext.reset(
      new AVIOBytesContext(buffer, length, kAVIOInternalTemporaryBufferSize));
  if (!toReturn.ioBytesContext) {
    throw std::runtime_error("Failed to create AVIOBytesContext");
  }
  toReturn.formatContext->pb = toReturn.ioBytesContext->getAVIO();
  AVFormatContext* tempFormatContext = toReturn.formatContext.release();
  int open_ret =
      avformat_open_input(&tempFormatContext, nullptr, nullptr, nullptr);
  toReturn.formatContext.reset(tempFormatContext);
  if (open_ret != 0) {
    throw std::runtime_error(
        std::string("Failed to open input buffer: ") +
        getFFMPEGErrorStringFromErrorCode(open_ret));
  }
  return toReturn;
}

std::vector<std::string> splitStringWithDelimiters(
    const std::string& str,
    const std::string& delims) {
  std::vector<std::string> result;
  if (str.empty()) {
    return result;
  }

  std::string::size_type start = 0, end = 0;
  while ((end = str.find_first_of(delims, start)) != std::string::npos) {
    result.push_back(str.substr(start, end - start));
    start = end + 1;
  }
  result.push_back(str.substr(start));
  return result;
}

} // namespace

VideoDecoder::VideoStreamDecoderOptions::VideoStreamDecoderOptions(
    const std::string& optionsString) {
  std::vector<std::string> tokens =
      splitStringWithDelimiters(optionsString, ",");
  for (auto token : tokens) {
    std::vector<std::string> pairs = splitStringWithDelimiters(token, "=");
    if (pairs.size() != 2) {
      throw std::runtime_error(
          "Invalid option: " + token +
          ". Options must be in the form 'option=value'.");
    }
    std::string key = pairs[0];
    std::string value = pairs[1];
    if (key == "ffmpeg_thread_count") {
      ffmpegThreadCount = std::stoi(value);
      if (ffmpegThreadCount < 0) {
        throw std::runtime_error(
            "Invalid ffmpeg_thread_count=" + value +
            ". ffmpeg_thread_count must be >= 0.");
      }
    } else if (key == "shape") {
      if (value != "NHWC" && value != "NCHW") {
        throw std::runtime_error(
            "Invalid shape=" + value + ". shape must be either NHWC or NCHW.");
      }
      shape = value;
    } else if (key == "width") {
      width = std::stoi(value);
    } else if (key == "height") {
      height = std::stoi(value);
    } else {
      throw std::runtime_error(
          "Invalid option: " + key +
          ". Valid options are: ffmpeg_thread_count=<int>,shape=<string>");
    }
  }
}

VideoDecoder::VideoDecoder() {}

void VideoDecoder::initializeDecoder() {
  // Some formats don't store enough info in the header so we read/decode a few
  // frames to grab that. This is needed for the filter graph. TODO: If this
  // takes a long time, consider initializing the filter graph after the first
  // frame decode.
  int ffmpegStatus = avformat_find_stream_info(formatContext_.get(), nullptr);
  if (ffmpegStatus < 0) {
    throw std::runtime_error(
        "Failed to find stream info: " +
        getFFMPEGErrorStringFromErrorCode(ffmpegStatus));
  }
  containerMetadata_.streams.resize(0);
  for (int i = 0; i < formatContext_->nb_streams; i++) {
    AVStream* stream = formatContext_->streams[i];
    containerMetadata_.streams.resize(containerMetadata_.streams.size() + 1);
    auto& curr = containerMetadata_.streams.back();
    curr.streamIndex = i;
    curr.mediaType = stream->codecpar->codec_type;
    curr.codecName = avcodec_get_name(stream->codecpar->codec_id);
    curr.bitRate = stream->codecpar->bit_rate;

    int64_t frameCount = stream->nb_frames;
    if (frameCount > 0) {
      curr.numFrames = frameCount;
    }
    if (stream->duration > 0 && stream->time_base.den > 0) {
      curr.durationSeconds = av_q2d(stream->time_base) * stream->duration;
    }
    double fps = av_q2d(stream->r_frame_rate);
    if (fps > 0) {
      curr.averageFps = fps;
    }

    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      containerMetadata_.numVideoStreams++;
    } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      containerMetadata_.numAudioStreams++;
    }
  }
  if (formatContext_->duration > 0) {
    containerMetadata_.durationSeconds =
        1.0 * formatContext_->duration / AV_TIME_BASE;
  }
  if (formatContext_->bit_rate > 0) {
    containerMetadata_.bitRate = formatContext_->bit_rate;
  }
  int bestVideoStream = getBestStreamIndex(AVMEDIA_TYPE_VIDEO);
  if (bestVideoStream >= 0) {
    containerMetadata_.bestVideoStreamIndex = bestVideoStream;
  }
  int bestAudioStream = getBestStreamIndex(AVMEDIA_TYPE_VIDEO);
  if (bestAudioStream >= 0) {
    containerMetadata_.bestAudioStreamIndex = bestAudioStream;
  }
}

std::unique_ptr<VideoDecoder> VideoDecoder::createFromFilePath(
    const std::string& videoFilePath,
    const VideoDecoder::DecoderOptions& options) {
  AVInput input = createAVFormatContextFromFilePath(videoFilePath);
  std::unique_ptr<VideoDecoder> decoder(new VideoDecoder());
  decoder->formatContext_ = std::move(input.formatContext);
  decoder->options_ = options;
  decoder->initializeDecoder();
  return decoder;
}

std::unique_ptr<VideoDecoder> VideoDecoder::createFromBuffer(
    const void* buffer,
    size_t length,
    const VideoDecoder::DecoderOptions& options) {
  AVInput input = createAVFormatContextFromBuffer(buffer, length);
  std::unique_ptr<VideoDecoder> decoder(new VideoDecoder());
  decoder->formatContext_ = std::move(input.formatContext);
  decoder->ioBytesContext_ = std::move(input.ioBytesContext);
  decoder->options_ = options;
  decoder->initializeDecoder();
  return decoder;
}

void VideoDecoder::initializeFilterGraphForStream(
    int streamIndex,
    const VideoStreamDecoderOptions& options) {
  FilterState& filterState = streams_[streamIndex].filterState;
  if (filterState.filterGraph) {
    return;
  }
  filterState.filterGraph.reset(avfilter_graph_alloc());
  TORCH_CHECK(filterState.filterGraph.get() != nullptr);
  const AVFilter* buffersrc = avfilter_get_by_name("buffer");
  const AVFilter* buffersink = avfilter_get_by_name("buffersink");
  enum AVPixelFormat pix_fmts[] = {AV_PIX_FMT_RGB24, AV_PIX_FMT_NONE};
  const StreamInfo& activeStream = streams_[streamIndex];

  AVCodecContext* codecContext = activeStream.codecContext.get();
  char args[512];
  snprintf(
      args,
      sizeof(args),
      "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
      codecContext->width,
      codecContext->height,
      codecContext->pix_fmt,
      activeStream.stream->time_base.num,
      activeStream.stream->time_base.den,
      codecContext->sample_aspect_ratio.num,
      codecContext->sample_aspect_ratio.den);

  int ffmpegStatus = avfilter_graph_create_filter(
      &filterState.sourceContext,
      buffersrc,
      "in",
      args,
      nullptr,
      filterState.filterGraph.get());
  if (ffmpegStatus < 0) {
    throw std::runtime_error(
        std::string("Failed to create filter graph: ") + args + ": " +
        getFFMPEGErrorStringFromErrorCode(ffmpegStatus));
  }
  ffmpegStatus = avfilter_graph_create_filter(
      &filterState.sinkContext,
      buffersink,
      "out",
      nullptr,
      nullptr,
      filterState.filterGraph.get());
  if (ffmpegStatus < 0) {
    throw std::runtime_error(
        "Failed to create filter graph: " +
        getFFMPEGErrorStringFromErrorCode(ffmpegStatus));
  }
  ffmpegStatus = av_opt_set_int_list(
      filterState.sinkContext,
      "pix_fmts",
      pix_fmts,
      AV_PIX_FMT_NONE,
      AV_OPT_SEARCH_CHILDREN);
  if (ffmpegStatus < 0) {
    throw std::runtime_error(
        "Failed to set output pixel formats: " +
        getFFMPEGErrorStringFromErrorCode(ffmpegStatus));
  }
  UniqueAVFilterInOut outputs(avfilter_inout_alloc());
  UniqueAVFilterInOut inputs(avfilter_inout_alloc());
  outputs->name = av_strdup("in");
  outputs->filter_ctx = filterState.sourceContext;
  outputs->pad_idx = 0;
  outputs->next = nullptr;
  inputs->name = av_strdup("out");
  inputs->filter_ctx = filterState.sinkContext;
  inputs->pad_idx = 0;
  inputs->next = nullptr;
  char description[512];
  int width = activeStream.codecContext->width;
  int height = activeStream.codecContext->height;
  if (options.height.has_value() && options.width.has_value()) {
    width = *options.width;
    height = *options.height;
  }
  std::snprintf(description, sizeof(description), "scale=%d:%d", width, height);
  AVFilterInOut* outputsTmp = outputs.release();
  AVFilterInOut* inputsTmp = inputs.release();
  ffmpegStatus = avfilter_graph_parse_ptr(
      filterState.filterGraph.get(),
      description,
      &inputsTmp,
      &outputsTmp,
      nullptr);
  outputs.reset(outputsTmp);
  inputs.reset(inputsTmp);
  if (ffmpegStatus < 0) {
    throw std::runtime_error(
        "Failed to parse filter description: " +
        getFFMPEGErrorStringFromErrorCode(ffmpegStatus));
  }
  ffmpegStatus = avfilter_graph_config(filterState.filterGraph.get(), nullptr);
  if (ffmpegStatus < 0) {
    throw std::runtime_error(
        "Failed to configure filter graph: " +
        getFFMPEGErrorStringFromErrorCode(ffmpegStatus));
  }
}

int VideoDecoder::getBestStreamIndex(AVMediaType mediaType) {
  AVCodecPtr codec = nullptr;
  int streamNumber =
      av_find_best_stream(formatContext_.get(), mediaType, -1, -1, &codec, 0);
  return streamNumber;
}

void VideoDecoder::addVideoStreamDecoder(
    int preferredStreamNumber,
    const VideoStreamDecoderOptions& options) {
  if (activeStreamIndices_.count(preferredStreamNumber) > 0) {
    throw std::invalid_argument(
        "Stream with index " + std::to_string(preferredStreamNumber) +
        " is already active.");
  }
  TORCH_CHECK(formatContext_.get() != nullptr);
  AVCodecPtr codec = nullptr;
  int streamNumber = av_find_best_stream(
      formatContext_.get(),
      AVMEDIA_TYPE_VIDEO,
      preferredStreamNumber,
      -1,
      &codec,
      0);
  if (streamNumber < 0) {
    throw std::invalid_argument("No valid stream found in input file.");
  }
  TORCH_CHECK(codec != nullptr);
  StreamInfo& streamInfo = streams_[streamNumber];
  streamInfo.streamIndex = streamNumber;
  streamInfo.timeBase = formatContext_->streams[streamNumber]->time_base;
  streamInfo.stream = formatContext_->streams[streamNumber];
  if (streamInfo.stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
    throw std::invalid_argument(
        "Stream with index " + std::to_string(streamNumber) +
        " is not a video stream.");
  }
  AVCodecContext* codecContext = avcodec_alloc_context3(codec);
  codecContext->thread_count = options.ffmpegThreadCount.value_or(0);
  TORCH_CHECK(codecContext != nullptr);
  streamInfo.codecContext.reset(codecContext);
  int retVal = avcodec_parameters_to_context(
      streamInfo.codecContext.get(), streamInfo.stream->codecpar);
  TORCH_CHECK_EQ(retVal, AVSUCCESS);
  retVal = avcodec_open2(streamInfo.codecContext.get(), codec, nullptr);
  if (retVal < AVSUCCESS) {
    throw std::invalid_argument(getFFMPEGErrorStringFromErrorCode(retVal));
  }
  codecContext->time_base = streamInfo.stream->time_base;
  activeStreamIndices_.insert(streamNumber);
  updateMetadataWithCodecContext(streamInfo.streamIndex, codecContext);
  streamInfo.options = options;
  initializeFilterGraphForStream(streamNumber, options);
}

void VideoDecoder::updateMetadataWithCodecContext(
    int streamIndex,
    AVCodecContext* codecContext) {
  containerMetadata_.streams[streamIndex].width = codecContext->width;
  containerMetadata_.streams[streamIndex].height = codecContext->height;
  auto codedId = codecContext->codec_id;
  containerMetadata_.streams[streamIndex].codecName =
      std::string(avcodec_get_name(codedId));
}

VideoDecoder::ContainerMetadata VideoDecoder::getContainerMetadata() const {
  return containerMetadata_;
}

int VideoDecoder::getKeyFrameIndexForPtsUsingEncoderIndex(
    AVStream* stream,
    int64_t pts) const {
  int currentKeyFrameIndex =
      av_index_search_timestamp(stream, pts, AVSEEK_FLAG_BACKWARD);
  return currentKeyFrameIndex;
}

int VideoDecoder::getKeyFrameIndexForPtsUsingScannedIndex(
    const std::vector<VideoDecoder::FrameInfo>& keyFrames,
    int64_t pts) const {
  auto upperBound = std::upper_bound(
      keyFrames.begin(),
      keyFrames.end(),
      pts,
      [](int64_t pts, const VideoDecoder::FrameInfo& frameInfo) {
        return pts < frameInfo.pts;
      });
  if (upperBound == keyFrames.begin()) {
    return -1;
  }
  return upperBound - 1 - keyFrames.begin();
}

void VideoDecoder::scanFileAndUpdateMetadataAndIndex() {
  while (true) {
    UniqueAVPacket packet(av_packet_alloc());
    int ffmpegStatus = av_read_frame(formatContext_.get(), packet.get());
    if (ffmpegStatus == AVERROR_EOF) {
      break;
    }
    if (ffmpegStatus != AVSUCCESS) {
      throw std::runtime_error(
          "Failed to read frame from input file: " +
          getFFMPEGErrorStringFromErrorCode(ffmpegStatus));
    }
    int streamIndex = packet->stream_index;

    if (packet->flags & AV_PKT_FLAG_DISCARD) {
      continue;
    }
    auto& stream = containerMetadata_.streams[streamIndex];
    stream.minPtsFromScan =
        std::min(stream.minPtsFromScan.value_or(INT64_MAX), packet->pts);
    stream.maxPtsFromScan = std::max(
        stream.maxPtsFromScan.value_or(INT64_MIN),
        packet->pts + packet->duration);
    stream.numFramesFromScan = stream.numFramesFromScan.value_or(0) + 1;

    FrameInfo frameInfo;
    frameInfo.pts = packet->pts;

    if (packet->flags & AV_PKT_FLAG_KEY) {
      streams_[streamIndex].keyFrames.push_back(frameInfo);
    }
    streams_[streamIndex].allFrames.push_back(frameInfo);
  }
  for (int i = 0; i < containerMetadata_.streams.size(); ++i) {
    auto& streamMetadata = containerMetadata_.streams[i];
    auto stream = formatContext_->streams[i];
    if (streamMetadata.minPtsFromScan.has_value()) {
      streamMetadata.minPtsSecondsFromScan =
          *streamMetadata.minPtsFromScan * av_q2d(stream->time_base);
    }
    if (streamMetadata.maxPtsFromScan.has_value()) {
      streamMetadata.maxPtsSecondsFromScan =
          *streamMetadata.maxPtsFromScan * av_q2d(stream->time_base);
    }
  }
  int ffmepgStatus =
      avformat_seek_file(formatContext_.get(), 0, INT64_MIN, 0, 0, 0);
  if (ffmepgStatus < 0) {
    throw std::runtime_error(
        "Could not seek file to pts=0: " +
        getFFMPEGErrorStringFromErrorCode(ffmepgStatus));
  }
  for (auto& [streamIndex, stream] : streams_) {
    std::sort(
        stream.keyFrames.begin(),
        stream.keyFrames.end(),
        [](const FrameInfo& frameInfo1, const FrameInfo& frameInfo2) {
          return frameInfo1.pts < frameInfo2.pts;
        });
    std::sort(
        stream.allFrames.begin(),
        stream.allFrames.end(),
        [](const FrameInfo& frameInfo1, const FrameInfo& frameInfo2) {
          return frameInfo1.pts < frameInfo2.pts;
        });
  }
}

int VideoDecoder::getKeyFrameIndexForPts(
    const StreamInfo& streamInfo,
    int64_t pts) const {
  if (streamInfo.keyFrames.empty()) {
    return getKeyFrameIndexForPtsUsingEncoderIndex(streamInfo.stream, pts);
  }
  return getKeyFrameIndexForPtsUsingScannedIndex(streamInfo.keyFrames, pts);
}
/*
Videos have I frames and non-I frames (P and B frames). Non-I frames need data
from the previous I frame to be decoded.

Imagine the cursor is at a random frame with PTS=x and we wish to seek to a
user-specified PTS=y.

If y < x, we don't have a choice but to seek backwards to the highest I frame
before y.

If y > x, we have two choices:

1. We could keep decoding forward until we hit y. Illustrated below:

I    P     P    P    I    P    P    P    I    P    P    I    P    P    I    P
                          x         y

2. We could try to jump to an I frame between x and y (indicated by j below).
And then start decoding until we encounter y. Illustrated below:

I    P     P    P    I    P    P    P    I    P    P    I    P    P    I    P
                          x              j         y

(2) is more efficient than (1) if there is an I frame between x and y.

We use av_index_search_timestamp to see if there is an I frame between x and y.
*/
bool VideoDecoder::canWeAvoidSeekingForStream(
    const StreamInfo& streamInfo,
    int64_t currentPts,
    int64_t targetPts) const {
  if (targetPts < currentPts) {
    // We can never skip a seek if we are seeking backwards.
    return false;
  }
  if (currentPts == targetPts) {
    // We are seeking to the exact same frame as we are currently at. Without
    // caching we have to rewind back and decode the frame again.
    // TODO(ahmads): We can avoid a seek+decode by caching the previous frame.
    return false;
  }
  // We are seeking forwards.
  // We can only skip a seek if both currentPts and targetPts share the same
  // keyframe.
  int currentKeyFrameIndex = getKeyFrameIndexForPts(streamInfo, currentPts);
  int targetKeyFrameIndex = getKeyFrameIndexForPts(streamInfo, targetPts);
  return currentKeyFrameIndex >= 0 && targetKeyFrameIndex >= 0 &&
      currentKeyFrameIndex == targetKeyFrameIndex;
}

// This method looks at currentPts and desiredPts and seeks in the
// AVFormatContext if it is needed. We can skip seeking in certain cases. See
// the comment of canWeAvoidSeeking() for details.
void VideoDecoder::maybeSeekToBeforeDesiredPts() {
  if (activeStreamIndices_.size() == 0) {
    return;
  }
  for (int streamIndex : activeStreamIndices_) {
    StreamInfo& streamInfo = streams_[streamIndex];
    streamInfo.discardFramesBeforePts =
        *maybeDesiredPts_ * streamInfo.timeBase.den;
  }

  decodeStats_.numSeeksAttempted++;
  // See comment for canWeAvoidSeeking() for details on why this optimization
  // works.
  bool mustSeek = false;
  for (int streamIndex : activeStreamIndices_) {
    StreamInfo& streamInfo = streams_[streamIndex];
    int64_t desiredPtsForStream = *maybeDesiredPts_ * streamInfo.timeBase.den;
    if (!canWeAvoidSeekingForStream(
            streamInfo, streamInfo.currentPts, desiredPtsForStream)) {
      VLOG(5) << "Seeking is needed for streamIndex=" << streamIndex
              << " desiredPts=" << desiredPtsForStream
              << " currentPts=" << streamInfo.currentPts;
      mustSeek = true;
      break;
    }
  }
  if (!mustSeek) {
    decodeStats_.numSeeksSkipped++;
    return;
  }
  int firstActiveStreamIndex = *activeStreamIndices_.begin();
  const auto& firstStreamInfo = streams_[firstActiveStreamIndex];
  int64_t desiredPts = *maybeDesiredPts_ * firstStreamInfo.timeBase.den;
  int ffmepgStatus = avformat_seek_file(
      formatContext_.get(),
      firstStreamInfo.streamIndex,
      INT64_MIN,
      desiredPts,
      desiredPts,
      0);
  if (ffmepgStatus < 0) {
    throw std::runtime_error(
        "Could not seek file to pts=" + std::to_string(desiredPts) + ": " +
        getFFMPEGErrorStringFromErrorCode(ffmepgStatus));
  }
  decodeStats_.numFlushes++;
  for (int streamIndex : activeStreamIndices_) {
    StreamInfo& streamInfo = streams_[streamIndex];
    avcodec_flush_buffers(streamInfo.codecContext.get());
  }
}

VideoDecoder::DecodedOutput VideoDecoder::getDecodedOutputWithFilter(
    std::function<bool(int, AVFrame*)> filterFunction) {
  if (activeStreamIndices_.size() == 0) {
    throw std::runtime_error("No active streams configured.");
  }
  VLOG(9) << "Starting getNextDecodedOutput()";
  resetDecodeStats();
  if (maybeDesiredPts_.has_value()) {
    VLOG(9) << "maybeDesiredPts_=" << *maybeDesiredPts_;
    maybeSeekToBeforeDesiredPts();
    maybeDesiredPts_ = std::nullopt;
    VLOG(9) << "seeking done";
  }
  // Need to get the next frame or error from PopFrame.
  UniqueAVFrame frame(av_frame_alloc());
  int ffmpegStatus = AVSUCCESS;
  bool reachedEOF = false;
  int frameStreamIndex = -1;
  while (true) {
    frameStreamIndex = -1;
    bool gotPermanentErrorOnAnyActiveStream = false;
    for (int streamIndex : activeStreamIndices_) {
      StreamInfo& streamInfo = streams_[streamIndex];
      ffmpegStatus =
          avcodec_receive_frame(streamInfo.codecContext.get(), frame.get());
      VLOG(9) << "received frame" << " status=" << ffmpegStatus
              << " streamIndex=" << streamInfo.stream->index;
      bool gotNonRetriableError =
          ffmpegStatus != AVSUCCESS && ffmpegStatus != AVERROR(EAGAIN);
      if (gotNonRetriableError) {
        VLOG(9) << "Got non-retriable error from decoder: "
                << getFFMPEGErrorStringFromErrorCode(ffmpegStatus);
        gotPermanentErrorOnAnyActiveStream = true;
        break;
      }
      if (ffmpegStatus == AVSUCCESS) {
        frameStreamIndex = streamIndex;
        break;
      }
    }
    if (gotPermanentErrorOnAnyActiveStream) {
      break;
    }
    decodeStats_.numFramesReceivedByDecoder++;
    bool gotNeededFrame = ffmpegStatus == AVSUCCESS &&
        filterFunction(frameStreamIndex, frame.get());
    if (gotNeededFrame) {
      break;
    } else if (ffmpegStatus == AVSUCCESS) {
      // No need to send more packets here as the decoder may have frames in
      // its buffer.
      continue;
    }
    if (reachedEOF) {
      // We don't have any more packets to send to the decoder. So keep on
      // pulling frames from its internal buffers.
      continue;
    }
    UniqueAVPacket packet(av_packet_alloc());
    ffmpegStatus = av_read_frame(formatContext_.get(), packet.get());
    decodeStats_.numPacketsRead++;
    VLOG(9) << "av_read_frame returned status: " << ffmpegStatus;
    if (ffmpegStatus == AVERROR_EOF) {
      // End of file reached. We must drain all codecs by sending a nullptr
      // packet.
      for (int streamIndex : activeStreamIndices_) {
        StreamInfo& streamInfo = streams_[streamIndex];
        ffmpegStatus = avcodec_send_packet(
            streamInfo.codecContext.get(), /*avpkt=*/nullptr);
        if (ffmpegStatus < AVSUCCESS) {
          throw std::runtime_error(
              "Could not flush decoder: " +
              getFFMPEGErrorStringFromErrorCode(ffmpegStatus));
        }
      }
      reachedEOF = true;
      continue;
    }
    if (ffmpegStatus < AVSUCCESS) {
      // TODO: Handle EAGAIN gracefully.
      throw std::runtime_error(
          "Could not read frame from input file: " +
          getFFMPEGErrorStringFromErrorCode(ffmpegStatus));
    }
    VLOG(9) << "Got packet: stream_index=" << packet->stream_index
            << " pts=" << packet->pts << " size=" << packet->size;
    if (activeStreamIndices_.count(packet->stream_index) == 0) {
      // This packet is not for any of the active streams.
      continue;
    }
    ffmpegStatus = avcodec_send_packet(
        streams_[packet->stream_index].codecContext.get(), packet.get());
    decodeStats_.numPacketsSentToDecoder++;
    if (ffmpegStatus < AVSUCCESS) {
      throw std::runtime_error(
          "Could not push packet to decoder: " +
          getFFMPEGErrorStringFromErrorCode(ffmpegStatus));
    }
  }
  if (ffmpegStatus < AVSUCCESS) {
    throw std::runtime_error(
        "Could not receive frame from decoder: " +
        getFFMPEGErrorStringFromErrorCode(ffmpegStatus));
  }
  // Note that we don't flush the decoder when we reach EOF (even though that's
  // mentioned in https://ffmpeg.org/doxygen/trunk/group__lavc__encdec.html).
  // This is because we may have packets internally in the decoder that we
  // haven't received as frames. Eventually we will either hit AVERROR_EOF from
  // av_receive_frame() or the user will have seeked to a different location in
  // the file and that will flush the decoder.
  StreamInfo& activeStream = streams_[frameStreamIndex];
  activeStream.currentPts = frame->pts;
  activeStream.currentDuration = frame->pkt_duration;
  VLOG(3) << "Got frame: stream_index=" << activeStream.stream->index
          << " pts=" << frame->pts << " stats=" << decodeStats_;
  // Convert the frame to tensor.
  return convertAVFrameToDecodedOutput(frameStreamIndex, std::move(frame));
}

VideoDecoder::DecodedOutput VideoDecoder::convertAVFrameToDecodedOutput(
    int streamIndex,
    UniqueAVFrame frame) {
  // Convert the frame to tensor.
  DecodedOutput output;
  output.streamIndex = streamIndex;
  output.streamType = streams_[streamIndex].stream->codecpar->codec_type;
  output.pts = frame->pts;
  output.ptsSeconds =
      1.0 * frame->pts / formatContext_->streams[streamIndex]->time_base.den;
  if (output.streamType == AVMEDIA_TYPE_VIDEO) {
    output.frame =
        convertFrameToTensorUsingFilterGraph(streamIndex, frame.get());
  } else if (output.streamType == AVMEDIA_TYPE_AUDIO) {
    // TODO: implement audio AVFrame to Tensor conversion here.
    throw std::runtime_error("Audio is not supported yet.");
  }
  return output;
}

VideoDecoder::DecodedOutput VideoDecoder::getFrameDisplayedAtTimestamp(
    double seconds) {
  for (auto& [streamIndex, stream] : streams_) {
    double frameStartTime = 1.0 * stream.currentPts / stream.timeBase.den;
    double frameEndTime = 1.0 * (stream.currentPts + stream.currentDuration) /
        stream.timeBase.den;
    if (seconds >= frameStartTime && seconds < frameEndTime) {
      // We are in the same frame as the one we just returned. However, since we
      // don't cache it locally, we have to rewind back.
      // TODO: add caching so we don't have to do this.
      seconds = frameStartTime;
      break;
    }
  }
  setCursorPtsInSeconds(seconds);
  return getDecodedOutputWithFilter(
      [seconds, this](int frameStreamIndex, AVFrame* frame) {
        StreamInfo& stream = streams_[frameStreamIndex];
        double frameStartTime = 1.0 * frame->pts / stream.timeBase.den;
        double frameEndTime =
            1.0 * (frame->pts + frame->pkt_duration) / stream.timeBase.den;
        return seconds >= frameStartTime && seconds < frameEndTime;
      });
}

VideoDecoder::DecodedOutput VideoDecoder::getFrameAtIndex(
    int streamIndex,
    int64_t frameIndex) {
  if (streamIndex < 0 || streamIndex >= containerMetadata_.streams.size()) {
    throw std::runtime_error(
        "Invalid stream index=" + std::to_string(streamIndex));
  }
  if (streams_.count(streamIndex) == 0) {
    throw std::runtime_error(
        "streamIndex=" + std::to_string(streamIndex) + " not added to decoder");
  }
  const auto& stream = streams_[streamIndex];
  if (frameIndex < 0 || frameIndex >= stream.allFrames.size()) {
    throw std::runtime_error(
        "Invalid frame index=" + std::to_string(frameIndex) +
        " for streamIndex=" + std::to_string(streamIndex) +
        " numFrames=" + std::to_string(streams_[streamIndex].allFrames.size()));
  }
  int64_t pts = stream.allFrames[frameIndex].pts;
  setCursorPtsInSeconds(1.0 * pts / stream.timeBase.den);
  return getNextDecodedOutput();
}

VideoDecoder::BatchDecodedOutput VideoDecoder::getFramesAtIndexes(
    int streamIndex,
    const std::vector<int64_t>& frameIndexes) {
  if (streamIndex < 0 || streamIndex >= containerMetadata_.streams.size()) {
    throw std::runtime_error(
        "Invalid stream index=" + std::to_string(streamIndex));
  }
  BatchDecodedOutput output;
  const auto& streamMetadata = containerMetadata_.streams[streamIndex];
  const auto& options = streams_[streamIndex].options;
  if (options.shape == "NHWC") {
    output.frames = torch::empty(
        {(long)frameIndexes.size(),
         options.height.value_or(*streamMetadata.height),
         options.width.value_or(*streamMetadata.width),
         3},
        {torch::kUInt8});
  } else if (options.shape == "NCHW") {
    output.frames = torch::empty(
        {(long)frameIndexes.size(),
         3,
         options.height.value_or(*streamMetadata.height),
         options.width.value_or(*streamMetadata.width)},
        {torch::kUInt8});
  } else {
    throw std::runtime_error("Unsupported frame shape=" + options.shape);
  }
  int i = 0;
  if (streams_.count(streamIndex) == 0) {
    throw std::runtime_error(
        "Invalid stream index=" + std::to_string(streamIndex));
  }
  const auto& stream = streams_[streamIndex];
  for (int64_t frameIndex : frameIndexes) {
    if (frameIndex < 0 || frameIndex >= stream.allFrames.size()) {
      throw std::runtime_error(
          "Invalid frame index=" + std::to_string(frameIndex));
    }
    int64_t pts = stream.allFrames[frameIndex].pts;
    setCursorPtsInSeconds(1.0 * pts / stream.timeBase.den);
    torch::Tensor frame = getNextDecodedOutput().frame;
    output.frames[i++] = frame;
  }
  return output;
}

VideoDecoder::DecodedOutput VideoDecoder::getNextDecodedOutput() {
  return getDecodedOutputWithFilter(
      [this](int frameStreamIndex, AVFrame* frame) {
        StreamInfo& activeStream = streams_[frameStreamIndex];
        return frame->pts >=
            activeStream.discardFramesBeforePts.value_or(INT64_MIN);
      });
}

void VideoDecoder::setCursorPtsInSeconds(double seconds) {
  maybeDesiredPts_ = seconds;
}

VideoDecoder::DecodeStats VideoDecoder::getDecodeStats() const {
  return decodeStats_;
}

void VideoDecoder::resetDecodeStats() {
  decodeStats_ = DecodeStats{};
}

torch::Tensor VideoDecoder::convertFrameToTensorUsingFilterGraph(
    int streamIndex,
    const AVFrame* frame) {
  FilterState& filterState = streams_[streamIndex].filterState;
  int ffmpegStatus = av_buffersrc_write_frame(filterState.sourceContext, frame);
  if (ffmpegStatus < AVSUCCESS) {
    throw std::runtime_error("Failed to add frame to buffer source context");
  }
  UniqueAVFrame filteredFrame(av_frame_alloc());
  ffmpegStatus =
      av_buffersink_get_frame(filterState.sinkContext, filteredFrame.get());
  TORCH_CHECK_EQ(filteredFrame->format, AV_PIX_FMT_RGB24);
  std::vector<int64_t> shape = {filteredFrame->height, filteredFrame->width, 3};
  std::vector<int64_t> strides = {filteredFrame->linesize[0], 3, 1};
  AVFrame* filteredFramePtr = filteredFrame.release();
  auto deleter = [filteredFramePtr](void*) {
    UniqueAVFrame frameToDelete(filteredFramePtr);
  };
  torch::Tensor tensor = torch::from_blob(
      filteredFramePtr->data[0], shape, strides, deleter, {torch::kUInt8});
  StreamInfo& activeStream = streams_[streamIndex];
  if (activeStream.options.shape == "NCHW") {
    tensor = tensor.permute({2, 0, 1});
  }
  return tensor;
}

std::ostream& operator<<(
    std::ostream& os,
    const VideoDecoder::DecodeStats& stats) {
  os << "DecodeStats{"
     << "numFramesReceivedByDecoder=" << stats.numFramesReceivedByDecoder
     << ", numPacketsRead=" << stats.numPacketsRead
     << ", numPacketsSentToDecoder=" << stats.numPacketsSentToDecoder
     << ", numSeeksAttempted=" << stats.numSeeksAttempted
     << ", numSeeksSkipped=" << stats.numSeeksSkipped
     << ", numFlushes=" << stats.numFlushes << "}";

  return os;
}

} // namespace facebook::torchcodec
