// Copyright 2004-present Facebook. All Rights Reserved.

#include "src/Payload.h"
#include <folly/String.h>
#include <folly/io/Cursor.h>
#include "src/Frame.h"

namespace reactivesocket {

Payload::Payload(
    std::unique_ptr<folly::IOBuf> _data,
    std::unique_ptr<folly::IOBuf> _metadata)
    : data(std::move(_data)), metadata(std::move(_metadata)) {}

Payload::Payload(const std::string& _data, const std::string& _metadata)
    : data(folly::IOBuf::copyBuffer(_data)) {
  if (!_metadata.empty()) {
    metadata = folly::IOBuf::copyBuffer(_metadata);
  }
}

void Payload::checkFlags(FrameFlags flags) const {
  DCHECK(!!(flags & FrameFlags::METADATA) == bool(metadata));
}

std::ostream& operator<<(std::ostream& os, const Payload& payload) {
  return os << "[metadata: "
            << (payload.metadata
                    ? folly::to<std::string>(
                          payload.metadata->computeChainDataLength())
                    : "<null>")
            << " data: " << (payload.data
                                 ? folly::to<std::string>(
                                       payload.data->computeChainDataLength())
                                 : "<null>")
            << "]";
}

std::string Payload::moveDataToString() {
  if (!data) {
    return "";
  }
  return data->moveToFbString().toStdString();
}

void Payload::clear() {
  data.reset();
  metadata.reset();
}

FrameFlags Payload::getFlags() const {
  return (metadata != nullptr ? FrameFlags::METADATA : FrameFlags::EMPTY);
}

} // reactivesocket
