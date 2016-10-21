// Copyright 2004-present Facebook. All Rights Reserved.

#include "src/ReactiveSocket.h"

#include <cassert>
#include <functional>
#include <memory>

#include <folly/ExceptionWrapper.h>
#include <folly/Memory.h>
#include <folly/MoveWrapper.h>
#include <folly/futures/QueuedImmediateExecutor.h>

#include "src/CancellingSubscription.h"
#include "src/ConnectionAutomaton.h"
#include "src/ConnectionSetupPayload.h"
#include "src/DuplexConnection.h"
#include "src/Frame.h"
#include "src/Payload.h"
#include "src/ReactiveSocketSubscriberFactory.h"
#include "src/RequestHandler.h"
#include "src/automata/ChannelRequester.h"
#include "src/automata/ChannelResponder.h"
#include "src/automata/RequestResponseRequester.h"
#include "src/automata/RequestResponseResponder.h"
#include "src/automata/StreamRequester.h"
#include "src/automata/StreamResponder.h"
#include "src/automata/SubscriptionRequester.h"
#include "src/automata/SubscriptionResponder.h"

namespace reactivesocket {

ReactiveSocket::~ReactiveSocket() {
  // Force connection closure, this will trigger terminal signals to be
  // delivered to all stream automata.
  connection_->disconnect();
}

ReactiveSocket::ReactiveSocket(
    bool isServer,
    std::unique_ptr<DuplexConnection> connection,
    std::unique_ptr<RequestHandlerBase> handler,
    ResumeSocketListener resumeListener,
    Stats& stats,
    std::unique_ptr<KeepaliveTimer> keepaliveTimer)
    : connection_(new ConnectionAutomaton(
          std::move(connection),
          std::bind(
              &ReactiveSocket::createResponder,
              this,
              std::placeholders::_1,
              std::placeholders::_2),
          std::bind(
              &ReactiveSocket::resumeListener,
              this,
              std::placeholders::_1,
              std::placeholders::_2),
          stats,
          isServer)),
      handler_(std::move(handler)),
      nextStreamId_(isServer ? 1 : 2),
      keepaliveTimer_(std::move(keepaliveTimer)),
      resumeSocketListener_(resumeListener) {}

folly::Executor& ReactiveSocket::defaultExecutor() {
  static folly::QueuedImmediateExecutor immediateExecutor;
  return immediateExecutor;
}

std::unique_ptr<ReactiveSocket> ReactiveSocket::fromClientConnection(
    std::unique_ptr<DuplexConnection> connection,
    std::unique_ptr<RequestHandlerBase> handler,
    ConnectionSetupPayload setupPayload,
    Stats& stats,
    std::unique_ptr<KeepaliveTimer> keepaliveTimer,
    const ResumeIdentificationToken& token) {
  std::unique_ptr<ReactiveSocket> socket(new ReactiveSocket(
      false,
      std::move(connection),
      std::move(handler),
      [](ReactiveSocket&, const ResumeIdentificationToken&, ResumePosition) {
        return false;
      },
      stats,
      std::move(keepaliveTimer)));
  socket->connection_->connect();

  uint32_t keepaliveTime = socket->keepaliveTimer_
      ? socket->keepaliveTimer_->keepaliveTime().count()
      : std::numeric_limits<uint32_t>::max();

  // TODO set correct version
  Frame_SETUP frame(
      FrameFlags_EMPTY,
      0,
      keepaliveTime,
      std::numeric_limits<uint32_t>::max(),
      token,
      std::move(setupPayload.metadataMimeType),
      std::move(setupPayload.dataMimeType),
      std::move(setupPayload.payload));

  socket->connection_->outputFrameOrEnqueue(frame.serializeOut());

  if (socket->keepaliveTimer_) {
    socket->keepaliveTimer_->start(socket->connection_.get());
  }

  return socket;
}

std::unique_ptr<ReactiveSocket> ReactiveSocket::fromServerConnection(
    std::unique_ptr<DuplexConnection> connection,
    std::unique_ptr<RequestHandlerBase> handler,
    Stats& stats,
    ResumeSocketListener resumeListener) {
  std::unique_ptr<ReactiveSocket> socket(new ReactiveSocket(
      true,
      std::move(connection),
      std::move(handler),
      resumeListener,
      stats,
      std::unique_ptr<KeepaliveTimer>(nullptr)));
  socket->connection_->connect();
  return socket;
}

Subscriber<Payload>& ReactiveSocket::requestChannel(
    Subscriber<Payload>& responseSink,
    folly::Executor& executor) {
  // TODO(stupaq): handle any exceptions
  StreamId streamId = nextStreamId_;
  nextStreamId_ += 2;
  ChannelRequester::Parameters params = {{connection_, streamId}, executor};
  auto automaton = new ChannelRequester(params);
  connection_->addStream(streamId, *automaton);
  automaton->subscribe(responseSink);
  responseSink.onSubscribe(*automaton);
  automaton->start();
  return *automaton;
}

void ReactiveSocket::requestStream(
    Payload request,
    Subscriber<Payload>& responseSink,
    folly::Executor& executor) {
  // TODO(stupaq): handle any exceptions
  StreamId streamId = nextStreamId_;
  nextStreamId_ += 2;
  StreamRequester::Parameters params = {{connection_, streamId}, executor};
  auto automaton = new StreamRequester(params);
  connection_->addStream(streamId, *automaton);
  automaton->subscribe(responseSink);
  responseSink.onSubscribe(*automaton);
  automaton->onNext(std::move(request));
  automaton->start();
}

void ReactiveSocket::requestSubscription(
    Payload request,
    Subscriber<Payload>& responseSink,
    folly::Executor& executor) {
  // TODO(stupaq): handle any exceptions
  StreamId streamId = nextStreamId_;
  nextStreamId_ += 2;
  SubscriptionRequester::Parameters params = {{connection_, streamId},
                                              executor};
  auto automaton = new SubscriptionRequester(params);
  connection_->addStream(streamId, *automaton);
  automaton->subscribe(responseSink);
  responseSink.onSubscribe(*automaton);
  automaton->onNext(std::move(request));
  automaton->start();
}

void ReactiveSocket::requestFireAndForget(Payload request) {
  // TODO(stupaq): handle any exceptions
  StreamId streamId = nextStreamId_;
  nextStreamId_ += 2;
  Frame_REQUEST_FNF frame(
      streamId, FrameFlags_EMPTY, std::move(std::move(request)));
  connection_->outputFrameOrEnqueue(frame.serializeOut());
}

void ReactiveSocket::requestResponse(
    Payload payload,
    Subscriber<Payload>& responseSink,
    folly::Executor& executor) {
  // TODO(stupaq): handle any exceptions
  StreamId streamId = nextStreamId_;
  nextStreamId_ += 2;
  RequestResponseRequester::Parameters params = {{connection_, streamId},
                                                 executor};
  auto automaton = new RequestResponseRequester(params);
  connection_->addStream(streamId, *automaton);
  automaton->subscribe(responseSink);
  responseSink.onSubscribe(*automaton);
  automaton->onNext(std::move(payload));
  automaton->start();
}

void ReactiveSocket::metadataPush(std::unique_ptr<folly::IOBuf> metadata) {
  connection_->outputFrameOrEnqueue(
      Frame_METADATA_PUSH(std::move(metadata)).serializeOut());
}

bool ReactiveSocket::createResponder(
    StreamId streamId,
    std::unique_ptr<folly::IOBuf> serializedFrame) {
  auto type = FrameHeader::peekType(*serializedFrame);
  switch (type) {
    case FrameType::SETUP: {
      Frame_SETUP frame;
      if (frame.deserializeFrom(std::move(serializedFrame))) {
        if (frame.header_.flags_ & FrameFlags_LEASE) {
          // TODO(yschimke) We don't have the correct lease and wait logic above
          // yet
          LOG(WARNING) << "ignoring setup frame with lease";
          //          connectionOutput_.onNext(
          //              Frame_ERROR::badSetupFrame("leases not supported")
          //                  .serializeOut());
          //          disconnect();
        }

        handler_->handleSetupPayload(ConnectionSetupPayload(
            std::move(frame.metadataMimeType_),
            std::move(frame.dataMimeType_),
            std::move(frame.payload_)));
      } else {
        // TODO(yschimke) enable this later after clients upgraded
        LOG(WARNING) << "ignoring bad setup frame";
        //        connectionOutput_.onNext(
        //            Frame_ERROR::badSetupFrame("bad setup
        //            frame").serializeOut());
        //        disconnect();
      }

      break;
    }
    case FrameType::REQUEST_CHANNEL: {
      Frame_REQUEST_CHANNEL frame;
      if (!frame.deserializeFrom(std::move(serializedFrame))) {
        return false;
      }
      ChannelResponder* automaton = nullptr;
      ReactiveSocketSubscriberFactory subscriberFactory(
          [&](folly::Executor* executor) -> ChannelResponder& {
            ChannelResponder::Parameters params = {
                {connection_, streamId},
                executor ? *executor : defaultExecutor()};
            automaton = new ChannelResponder(params);
            connection_->addStream(streamId, *automaton);
            return *automaton;
          });
      auto& requestSink = handler_->onRequestChannel(
          std::move(frame.payload_), subscriberFactory);
      if (!automaton) {
        auto& subscriber = subscriberFactory.createSubscriber();
        subscriber.onSubscribe(
            createManagedInstance<CancellingSubscription>(subscriber));
      }
      automaton->subscribe(requestSink);
      // any calls from onSubscribe are queued until we start
      requestSink.onSubscribe(*automaton);
      // onNextFrame executes directly, it may cause to call request(n)
      // which may call back and it will be queued after the calls from
      // the onSubscribe method
      automaton->onNextFrame(std::move(frame));
      automaton->start();
      break;
    }
    case FrameType::REQUEST_STREAM: {
      Frame_REQUEST_STREAM frame;
      if (!frame.deserializeFrom(std::move(serializedFrame))) {
        return false;
      }
      StreamResponder* automaton = nullptr;
      ReactiveSocketSubscriberFactory subscriberFactory(
          [&](folly::Executor* executor) -> StreamResponder& {
            StreamResponder::Parameters params = {
                {connection_, streamId},
                executor ? *executor : defaultExecutor()};
            automaton = new StreamResponder(params);
            connection_->addStream(streamId, *automaton);
            return *automaton;
          });
      handler_->onRequestStream(
          std::move(frame.payload_), subscriberFactory);
      if (!automaton) {
        auto& subscriber = subscriberFactory.createSubscriber();
        subscriber.onSubscribe(
            createManagedInstance<CancellingSubscription>(subscriber));
      }
      automaton->onNextFrame(std::move(frame));
      automaton->start();
      break;
    }
    case FrameType::REQUEST_SUB: {
      Frame_REQUEST_SUB frame;
      if (!frame.deserializeFrom(std::move(serializedFrame))) {
        return false;
      }
      SubscriptionResponder* automaton = nullptr;
      ReactiveSocketSubscriberFactory subscriberFactory(
          [&](folly::Executor* executor) -> SubscriptionResponder& {
            SubscriptionResponder::Parameters params = {
                {connection_, streamId},
                executor ? *executor : defaultExecutor()};
            automaton = new SubscriptionResponder(params);
            connection_->addStream(streamId, *automaton);
            return *automaton;
          });
      handler_->onRequestSubscription(
          std::move(frame.payload_), subscriberFactory);
      if (!automaton) {
        auto& subscriber = subscriberFactory.createSubscriber();
        subscriber.onSubscribe(
            createManagedInstance<CancellingSubscription>(subscriber));
      }
      automaton->onNextFrame(std::move(frame));
      automaton->start();
      break;
    }
    case FrameType::REQUEST_RESPONSE: {
      Frame_REQUEST_RESPONSE frame;
      if (!frame.deserializeFrom(std::move(serializedFrame))) {
        return false;
      }
      RequestResponseResponder* automaton = nullptr;
      ReactiveSocketSubscriberFactory subscriberFactory(
          [&](folly::Executor* executor) -> RequestResponseResponder& {
            RequestResponseResponder::Parameters params = {
                {connection_, streamId},
                executor ? *executor : defaultExecutor()};
            automaton = new RequestResponseResponder(params);
            connection_->addStream(streamId, *automaton);
            return *automaton;
          });
      handler_->onRequestResponse(
          std::move(frame.payload_), subscriberFactory);
      // we need to create a responder to at least close the stream
      if (!automaton) {
        auto& subscriber = subscriberFactory.createSubscriber();
        subscriber.onSubscribe(
            createManagedInstance<CancellingSubscription>(subscriber));
      }
      automaton->onNextFrame(std::move(frame));
      automaton->start();
      break;
    }
    case FrameType::REQUEST_FNF: {
      Frame_REQUEST_FNF frame;
      if (!frame.deserializeFrom(std::move(serializedFrame))) {
        return false;
      }
      // no stream tracking is necessary
      handler_->handleFireAndForgetRequest(std::move(frame.payload_));
      break;
    }
    case FrameType::METADATA_PUSH: {
      Frame_METADATA_PUSH frame;
      if (!frame.deserializeFrom(std::move(serializedFrame))) {
        return false;
      }
      handler_->handleMetadataPush(std::move(frame.metadata_));
      break;
    }
    // Other frames cannot start a stream.
    default:
      return false;
  }
  return true;
}

bool ReactiveSocket::resumeListener(
    const ResumeIdentificationToken& token,
    ResumePosition position) {
  return resumeSocketListener_(*this, token, position);
}

void ReactiveSocket::close() {
  connection_->disconnect();
}

void ReactiveSocket::onClose(CloseListener listener) {
  connection_->onClose([listener, this]() { listener(*this); });
}

void ReactiveSocket::resumeFromSocket(ReactiveSocket& socket) {
  connection_->resumeFromAutomaton(*socket.connection_);
}

void ReactiveSocket::tryClientResume(
    std::unique_ptr<DuplexConnection> newConnection,
    const ResumeIdentificationToken& token) {
  connection_->reconnect(std::move(newConnection));
  connection_->sendResume(token);
}

bool ReactiveSocket::isPositionAvailable(ResumePosition position) {
  return connection_->isPositionAvailable(position);
}

ResumePosition ReactiveSocket::positionDifference(ResumePosition position) {
  return connection_->positionDifference(position);
}
}
