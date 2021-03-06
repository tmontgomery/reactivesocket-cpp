// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

#include <iosfwd>
#include "src/automata/StreamSubscriptionRequesterBase.h"

namespace folly {
class exception_wrapper;
}

namespace reactivesocket {

enum class StreamCompletionSignal;

/// Implementation of stream automaton that represents a Stream requester
class StreamRequester : public StreamSubscriptionRequesterBase {
  using Base = StreamSubscriptionRequesterBase;

 public:
  explicit StreamRequester(const Base::Parameters& params, Payload payload)
      : ExecutorBase(params.executor), Base(params, std::move(payload)) {}

 private:
  void sendRequestFrame(size_t, Payload&&) override;
};
} // reactivesocket
