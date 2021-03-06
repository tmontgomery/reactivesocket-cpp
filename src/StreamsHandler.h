// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

#include "src/Common.h"
#include "src/Payload.h"

namespace reactivesocket {

class FrameSerializer;

///
/// StreamsWriter is the interface for writing stream related frames
/// on the wire.
///
class StreamsWriter {
 public:
  virtual ~StreamsWriter() = default;

  virtual void writeNewStream(
      StreamId streamId,
      StreamType streamType,
      uint32_t initialRequestN,
      Payload payload,
      bool TEMP_completed) = 0;

  virtual void writeRequestN(StreamId streamId, uint32_t n) = 0;

  virtual void
  writePayload(StreamId streamId, Payload payload, bool complete) = 0;

  virtual void writeCloseStream(
      StreamId streamId,
      StreamCompletionSignal signal,
      Payload payload) = 0;

  virtual void onStreamClosed(
      StreamId streamId,
      StreamCompletionSignal signal) = 0;

  // TODO: remove, this is only a temporary workaround
  virtual FrameSerializer& frameSerializer() const = 0;
};
}
