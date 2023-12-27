// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STANDALONE_RECEIVER_DECODER_H_
#define CAST_STANDALONE_RECEIVER_DECODER_H_

#include <stdint.h>

#include <string>
#include <vector>

#include "absl/types/span.h"
#include "cast/standalone_receiver/avcodec_glue.h"
#include "cast/streaming/frame_id.h"

namespace openscreen {
namespace cast {

// Wraps libavcodec to decode audio or video.
class Decoder {
 public:
  // A buffer backed by storage that is compatible with FFMPEG (i.e., includes
  // the required zero-padding).
  class Buffer {
   public:
    Buffer();
    ~Buffer();

    void Resize(int new_size);
    absl::Span<const uint8_t> GetSpan() const;
    absl::Span<uint8_t> GetSpan();

   private:
    std::vector<uint8_t> buffer_;
  };

  // Interface for receiving decoded frames and/or errors.
  class Client {
   public:

    virtual void OnFrameDecoded(FrameId frame_id, const AVFrame& frame) = 0;
    virtual void OnDecodeError(FrameId frame_id, std::string message) = 0;
    virtual void OnFatalError(std::string message) = 0;

   protected:
    Client();
    virtual ~Client();
  };

  // |codec_name| should be the codec_name field from an OFFER message.
  explicit Decoder(const std::string& codec_name);
  ~Decoder();

  Client* client() const { return client_; }
  void set_client(Client* client) { client_ = client; }

  // Starts decoding the data in |buffer|, which should be associated with the
  // given |frame_id|. This will synchronously call Client::OnFrameDecoded()
  // and/or Client::OnDecodeError() zero or more times with results. Note that
  // some codecs will have data dependencies that require multiple encoded
  // frame's data before the first decoded frame can be generated.
  void Decode(FrameId frame_id, const Buffer& buffer);

 private:
  // Helper to initialize the FFMPEG decoder and supporting objects. Returns
  // false if this failed (and the Client was notified).
  bool Initialize();

  // Helper to get the FrameId that is associated with the next frame coming out
  // of the FFMPEG decoder.
  FrameId DidReceiveFrameFromDecoder();

  // Helper to handle a codec initialization error and notify the Client of the
  // fatal error.
  void HandleInitializationError(const char* what, int av_errnum);

  // Called when any transient or fatal error occurs, generating an Error and
  // notifying the Client of it.
  void OnError(const char* what, int av_errnum, FrameId frame_id);

  const std::string codec_name_;
  AVCodec* codec_ = nullptr;
  AVCodecParserContextUniquePtr parser_;
  AVCodecContextUniquePtr context_;
  AVPacketUniquePtr packet_;
  AVFrameUniquePtr decoded_frame_;

  Client* client_ = nullptr;

  // Queue of frames that have been input to the libavcodec decoder, but which
  // have not yet had output generated by it.
  std::vector<FrameId> frames_decoding_;
};

}  // namespace cast
}  // namespace openscreen

#endif  // CAST_STANDALONE_RECEIVER_DECODER_H_
