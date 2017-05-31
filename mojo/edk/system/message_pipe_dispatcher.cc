// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mojo/edk/system/message_pipe_dispatcher.h"

#include <limits>
#include <memory>

#include "base/logging.h"
#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "mojo/edk/embedder/embedder_internal.h"
#include "mojo/edk/system/core.h"
#include "mojo/edk/system/node_controller.h"
#include "mojo/edk/system/ports/event.h"
#include "mojo/edk/system/ports/message_filter.h"
#include "mojo/edk/system/request_context.h"
#include "mojo/edk/system/user_message_impl.h"

namespace mojo {
namespace edk {

namespace {

#pragma pack(push, 1)

struct SerializedState {
  uint64_t pipe_id;
  int8_t endpoint;
  char padding[7];
};

static_assert(sizeof(SerializedState) % 8 == 0,
              "Invalid SerializedState size.");

#pragma pack(pop)

}  // namespace

// A PortObserver which forwards to a MessagePipeDispatcher. This owns a
// reference to the MPD to ensure it lives as long as the observed port.
class MessagePipeDispatcher::PortObserverThunk
    : public NodeController::PortObserver {
 public:
  explicit PortObserverThunk(scoped_refptr<MessagePipeDispatcher> dispatcher)
      : dispatcher_(dispatcher) {}

 private:
  ~PortObserverThunk() override {}

  // NodeController::PortObserver:
  void OnPortStatusChanged() override { dispatcher_->OnPortStatusChanged(); }

  scoped_refptr<MessagePipeDispatcher> dispatcher_;

  DISALLOW_COPY_AND_ASSIGN(PortObserverThunk);
};

#if DCHECK_IS_ON()

// A MessageFilter which never matches a message. Used to peek at the size of
// the next available message on a port, for debug logging only.
class PeekSizeMessageFilter : public ports::MessageFilter {
 public:
  PeekSizeMessageFilter() {}
  ~PeekSizeMessageFilter() override {}

  // ports::MessageFilter:
  bool Match(const ports::UserMessageEvent& message) override {
    message_size_ = message.GetMessage<UserMessageImpl>()->user_payload_size();
    return false;
  }

  size_t message_size() const { return message_size_; }

 private:
  size_t message_size_ = 0;

  DISALLOW_COPY_AND_ASSIGN(PeekSizeMessageFilter);
};

#endif  // DCHECK_IS_ON()

MessagePipeDispatcher::MessagePipeDispatcher(NodeController* node_controller,
                                             const ports::PortRef& port,
                                             uint64_t pipe_id,
                                             int endpoint)
    : node_controller_(node_controller),
      port_(port),
      pipe_id_(pipe_id),
      endpoint_(endpoint),
      watchers_(this) {
  DVLOG(2) << "Creating new MessagePipeDispatcher for port " << port.name()
           << " [pipe_id=" << pipe_id << "; endpoint=" << endpoint << "]";

  node_controller_->SetPortObserver(
      port_,
      make_scoped_refptr(new PortObserverThunk(this)));
}

bool MessagePipeDispatcher::Fuse(MessagePipeDispatcher* other) {
  node_controller_->SetPortObserver(port_, nullptr);
  node_controller_->SetPortObserver(other->port_, nullptr);

  ports::PortRef port0;
  {
    base::AutoLock lock(signal_lock_);
    port0 = port_;
    port_closed_.Set(true);
    watchers_.NotifyClosed();
  }

  ports::PortRef port1;
  {
    base::AutoLock lock(other->signal_lock_);
    port1 = other->port_;
    other->port_closed_.Set(true);
    other->watchers_.NotifyClosed();
  }

  // Both ports are always closed by this call.
  int rv = node_controller_->MergeLocalPorts(port0, port1);
  return rv == ports::OK;
}

Dispatcher::Type MessagePipeDispatcher::GetType() const {
  return Type::MESSAGE_PIPE;
}

MojoResult MessagePipeDispatcher::Close() {
  base::AutoLock lock(signal_lock_);
  DVLOG(2) << "Closing message pipe " << pipe_id_ << " endpoint " << endpoint_
           << " [port=" << port_.name() << "]";
  return CloseNoLock();
}

MojoResult MessagePipeDispatcher::WriteMessage(
    std::unique_ptr<ports::UserMessageEvent> message,
    MojoWriteMessageFlags flags) {
  if (port_closed_ || in_transit_)
    return MOJO_RESULT_INVALID_ARGUMENT;

  const size_t num_bytes =
      message->GetMessage<UserMessageImpl>()->user_payload_size();
  int rv = node_controller_->SendUserMessage(port_, std::move(message));

  DVLOG(4) << "Sent message on pipe " << pipe_id_ << " endpoint " << endpoint_
           << " [port=" << port_.name() << "; rv=" << rv
           << "; num_bytes=" << num_bytes << "]";

  if (rv != ports::OK) {
    if (rv == ports::ERROR_PORT_UNKNOWN ||
        rv == ports::ERROR_PORT_STATE_UNEXPECTED ||
        rv == ports::ERROR_PORT_CANNOT_SEND_PEER) {
      return MOJO_RESULT_INVALID_ARGUMENT;
    } else if (rv == ports::ERROR_PORT_PEER_CLOSED) {
      return MOJO_RESULT_FAILED_PRECONDITION;
    }

    NOTREACHED();
    return MOJO_RESULT_UNKNOWN;
  }

  return MOJO_RESULT_OK;
}

MojoResult MessagePipeDispatcher::ReadMessage(
    std::unique_ptr<ports::UserMessageEvent>* message,
    uint32_t* num_bytes,
    MojoHandle* handles,
    uint32_t* num_handles,
    MojoReadMessageFlags flags,
    bool read_any_size) {
  // We can't read from a port that's closed or in transit!
  if (port_closed_ || in_transit_)
    return MOJO_RESULT_INVALID_ARGUMENT;

  const bool may_discard = flags & MOJO_READ_MESSAGE_FLAG_MAY_DISCARD;

  // Grab a message if the provided handles buffer is large enough. If the input
  // |num_bytes| is provided and |read_any_size| is false, we also ensure
  // that it specifies a size at least as large as the next available payload.
  //
  // If |read_any_size| is true, the input value of |*num_bytes| is ignored.
  // This flag exists to support both new and old API behavior.
  MojoResult read_result = UserMessageImpl::ReadMessageEventFromPort(
      node_controller_, port_, read_any_size, may_discard, num_bytes, handles,
      num_handles, message);

  // We may need to update anyone watching our signals in case we just read the
  // last available message.
  base::AutoLock lock(signal_lock_);
  watchers_.NotifyState(GetHandleSignalsStateNoLock());

  return read_result;
}

HandleSignalsState
MessagePipeDispatcher::GetHandleSignalsState() const {
  base::AutoLock lock(signal_lock_);
  return GetHandleSignalsStateNoLock();
}

MojoResult MessagePipeDispatcher::AddWatcherRef(
    const scoped_refptr<WatcherDispatcher>& watcher,
    uintptr_t context) {
  base::AutoLock lock(signal_lock_);
  if (port_closed_ || in_transit_)
    return MOJO_RESULT_INVALID_ARGUMENT;
  return watchers_.Add(watcher, context, GetHandleSignalsStateNoLock());
}

MojoResult MessagePipeDispatcher::RemoveWatcherRef(WatcherDispatcher* watcher,
                                                   uintptr_t context) {
  base::AutoLock lock(signal_lock_);
  if (port_closed_ || in_transit_)
    return MOJO_RESULT_INVALID_ARGUMENT;
  return watchers_.Remove(watcher, context);
}

void MessagePipeDispatcher::StartSerialize(uint32_t* num_bytes,
                                           uint32_t* num_ports,
                                           uint32_t* num_handles) {
  *num_bytes = static_cast<uint32_t>(sizeof(SerializedState));
  *num_ports = 1;
  *num_handles = 0;
}

bool MessagePipeDispatcher::EndSerialize(void* destination,
                                         ports::PortName* ports,
                                         PlatformHandle* handles) {
  SerializedState* state = static_cast<SerializedState*>(destination);
  state->pipe_id = pipe_id_;
  state->endpoint = static_cast<int8_t>(endpoint_);
  memset(state->padding, 0, sizeof(state->padding));
  ports[0] = port_.name();
  return true;
}

bool MessagePipeDispatcher::BeginTransit() {
  base::AutoLock lock(signal_lock_);
  if (in_transit_ || port_closed_)
    return false;
  in_transit_.Set(true);
  return in_transit_;
}

void MessagePipeDispatcher::CompleteTransitAndClose() {
  node_controller_->SetPortObserver(port_, nullptr);

  base::AutoLock lock(signal_lock_);
  port_transferred_ = true;
  in_transit_.Set(false);
  CloseNoLock();
}

void MessagePipeDispatcher::CancelTransit() {
  base::AutoLock lock(signal_lock_);
  in_transit_.Set(false);

  // Something may have happened while we were waiting for potential transit.
  watchers_.NotifyState(GetHandleSignalsStateNoLock());
}

// static
scoped_refptr<Dispatcher> MessagePipeDispatcher::Deserialize(
    const void* data,
    size_t num_bytes,
    const ports::PortName* ports,
    size_t num_ports,
    PlatformHandle* handles,
    size_t num_handles) {
  if (num_ports != 1 || num_handles || num_bytes != sizeof(SerializedState))
    return nullptr;

  const SerializedState* state = static_cast<const SerializedState*>(data);

  ports::PortRef port;
  CHECK_EQ(
      ports::OK,
      internal::g_core->GetNodeController()->node()->GetPort(ports[0], &port));

  return new MessagePipeDispatcher(internal::g_core->GetNodeController(), port,
                                   state->pipe_id, state->endpoint);
}

MessagePipeDispatcher::~MessagePipeDispatcher() {
  DCHECK(port_closed_ && !in_transit_);
}

MojoResult MessagePipeDispatcher::CloseNoLock() {
  signal_lock_.AssertAcquired();
  if (port_closed_ || in_transit_)
    return MOJO_RESULT_INVALID_ARGUMENT;

  port_closed_.Set(true);
  watchers_.NotifyClosed();

  if (!port_transferred_) {
    base::AutoUnlock unlock(signal_lock_);
    node_controller_->ClosePort(port_);
  }

  return MOJO_RESULT_OK;
}

HandleSignalsState MessagePipeDispatcher::GetHandleSignalsStateNoLock() const {
  HandleSignalsState rv;

  ports::PortStatus port_status;
  if (node_controller_->node()->GetStatus(port_, &port_status) != ports::OK) {
    CHECK(in_transit_ || port_transferred_ || port_closed_);
    return HandleSignalsState();
  }

  if (port_status.has_messages) {
    rv.satisfied_signals |= MOJO_HANDLE_SIGNAL_READABLE;
    rv.satisfiable_signals |= MOJO_HANDLE_SIGNAL_READABLE;
  }
  if (port_status.receiving_messages)
    rv.satisfiable_signals |= MOJO_HANDLE_SIGNAL_READABLE;
  if (!port_status.peer_closed) {
    rv.satisfied_signals |= MOJO_HANDLE_SIGNAL_WRITABLE;
    rv.satisfiable_signals |= MOJO_HANDLE_SIGNAL_WRITABLE;
    rv.satisfiable_signals |= MOJO_HANDLE_SIGNAL_READABLE;
  } else {
    rv.satisfied_signals |= MOJO_HANDLE_SIGNAL_PEER_CLOSED;
  }
  rv.satisfiable_signals |= MOJO_HANDLE_SIGNAL_PEER_CLOSED;
  return rv;
}

void MessagePipeDispatcher::OnPortStatusChanged() {
  DCHECK(RequestContext::current());

  base::AutoLock lock(signal_lock_);

  // We stop observing our port as soon as it's transferred, but this can race
  // with events which are raised right before that happens. This is fine to
  // ignore.
  if (port_transferred_)
    return;

#if DCHECK_IS_ON()
  ports::PortStatus port_status;
  if (node_controller_->node()->GetStatus(port_, &port_status) == ports::OK) {
    if (port_status.has_messages) {
      std::unique_ptr<ports::UserMessageEvent> unused;
      PeekSizeMessageFilter filter;
      node_controller_->node()->GetMessage(port_, &unused, &filter);
      DVLOG(4) << "New message detected on message pipe " << pipe_id_
               << " endpoint " << endpoint_ << " [port=" << port_.name()
               << "; size=" << filter.message_size() << "]";
    }
    if (port_status.peer_closed) {
      DVLOG(2) << "Peer closure detected on message pipe " << pipe_id_
               << " endpoint " << endpoint_ << " [port=" << port_.name() << "]";
    }
  }
#endif

  watchers_.NotifyState(GetHandleSignalsStateNoLock());
}

}  // namespace edk
}  // namespace mojo
