// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

#include <condition_variable>
#include <mutex>
#include <vector>

#include <folly/ExceptionWrapper.h>

#include "src/Payload.h"
#include "src/ReactiveStreamsCompat.h"

namespace reactivesocket {
namespace tck {

class TestSubscriber : public reactivesocket::Subscriber<Payload> {
 public:
  explicit TestSubscriber(int initialRequestN = 0);

  void request(int n);
  void cancel();

  void awaitTerminalEvent();
  void awaitAtLeast(int numItems);
  void awaitNoEvents(int waitTime);
  void assertNoErrors();
  void assertError();
  void assertValues(
      const std::vector<std::pair<std::string, std::string>>& values);
  void assertValueCount(size_t valueCount);
  void assertReceivedAtLeast(size_t valueCount);
  void assertCompleted();
  void assertNotCompleted();
  void assertCanceled();

 protected:
  void onSubscribe(
      std::shared_ptr<Subscription> subscription) noexcept override;
  void onNext(Payload element) noexcept override;
  void onComplete() noexcept override;
  void onError(folly::exception_wrapper ex) noexcept override;

 private:
  void assertTerminated();

  std::shared_ptr<Subscription> subscription_;
  int initialRequestN_{0};

  std::atomic<bool> canceled_{false};

  ////////////////////////////////////////////////////////////////////////////
  std::mutex mutex_; // all variables below has to be protected with the mutex

  std::vector<std::pair<std::string, std::string>> values_;
  std::condition_variable valuesCV_;
  std::atomic<int> valuesCount_{0};

  std::vector<folly::exception_wrapper> errors_;

  std::condition_variable terminatedCV_;
  std::atomic<bool> completed_{false}; // by onComplete
  std::atomic<bool> errored_{false}; // by onError
  ////////////////////////////////////////////////////////////////////////////
};

} // tck
} // reactivesocket
