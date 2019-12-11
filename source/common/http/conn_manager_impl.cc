#include "common/http/conn_manager_impl.h"

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "envoy/buffer/buffer.h"
#include "envoy/common/time.h"
#include "envoy/event/dispatcher.h"
#include "envoy/network/drain_decision.h"
#include "envoy/router/router.h"
#include "envoy/ssl/connection.h"
#include "envoy/stats/scope.h"
#include "envoy/tracing/http_tracer.h"

#include "common/buffer/buffer_impl.h"
#include "common/common/assert.h"
#include "common/common/empty_string.h"
#include "common/common/enum_to_int.h"
#include "common/common/fmt.h"
#include "common/common/scope_tracker.h"
#include "common/common/utility.h"
#include "common/http/codes.h"
#include "common/http/conn_manager/active_stream.h"
#include "common/http/conn_manager/active_stream_decoder_filter.h"
#include "common/http/conn_manager/active_stream_encoder_filter.h"
#include "common/http/conn_manager_utility.h"
#include "common/http/exception.h"
#include "common/http/header_map_impl.h"
#include "common/http/headers.h"
#include "common/http/http1/codec_impl.h"
#include "common/http/http2/codec_impl.h"
#include "common/http/path_utility.h"
#include "common/http/utility.h"
#include "common/network/utility.h"
#include "common/router/config_impl.h"
#include "common/runtime/runtime_impl.h"
#include "common/stats/timespan_impl.h"

#include "absl/strings/escaping.h"
#include "absl/strings/match.h"

namespace Envoy {
namespace Http {

ConnectionManagerStats ConnectionManagerImpl::generateStats(const std::string& prefix,
                                                            Stats::Scope& scope) {
  return {
      {ALL_HTTP_CONN_MAN_STATS(POOL_COUNTER_PREFIX(scope, prefix), POOL_GAUGE_PREFIX(scope, prefix),
                               POOL_HISTOGRAM_PREFIX(scope, prefix))},
      prefix,
      scope};
}

ConnectionManagerTracingStats ConnectionManagerImpl::generateTracingStats(const std::string& prefix,
                                                                          Stats::Scope& scope) {
  return {CONN_MAN_TRACING_STATS(POOL_COUNTER_PREFIX(scope, prefix + "tracing."))};
}

ConnectionManagerListenerStats
ConnectionManagerImpl::generateListenerStats(const std::string& prefix, Stats::Scope& scope) {
  return {CONN_MAN_LISTENER_STATS(POOL_COUNTER_PREFIX(scope, prefix))};
}

ConnectionManagerImpl::ConnectionManagerImpl(ConnectionManagerConfig& config,
                                             const Network::DrainDecision& drain_close,
                                             Runtime::RandomGenerator& random_generator,
                                             Http::Context& http_context, Runtime::Loader& runtime,
                                             const LocalInfo::LocalInfo& local_info,
                                             Upstream::ClusterManager& cluster_manager,
                                             Server::OverloadManager* overload_manager,
                                             TimeSource& time_source)
    : config_(config), stats_(config_.stats()),
      conn_length_(new Stats::HistogramCompletableTimespanImpl(
          stats_.named_.downstream_cx_length_ms_, time_source)),
      drain_close_(drain_close), random_generator_(random_generator), http_context_(http_context),
      runtime_(runtime), local_info_(local_info), cluster_manager_(cluster_manager),
      listener_stats_(config_.listenerStats()),
      overload_stop_accepting_requests_ref_(
          overload_manager ? overload_manager->getThreadLocalOverloadState().getState(
                                 Server::OverloadActionNames::get().StopAcceptingRequests)
                           : Server::OverloadManager::getInactiveState()),
      overload_disable_keepalive_ref_(
          overload_manager ? overload_manager->getThreadLocalOverloadState().getState(
                                 Server::OverloadActionNames::get().DisableHttpKeepAlive)
                           : Server::OverloadManager::getInactiveState()),
      time_source_(time_source) {}

void ConnectionManagerImpl::initializeReadFilterCallbacks(Network::ReadFilterCallbacks& callbacks) {
  read_callbacks_ = &callbacks;
  stats_.named_.downstream_cx_total_.inc();
  stats_.named_.downstream_cx_active_.inc();
  if (read_callbacks_->connection().ssl()) {
    stats_.named_.downstream_cx_ssl_total_.inc();
    stats_.named_.downstream_cx_ssl_active_.inc();
  }

  read_callbacks_->connection().addConnectionCallbacks(*this);

  if (config_.idleTimeout()) {
    connection_idle_timer_ = read_callbacks_->connection().dispatcher().createTimer(
        [this]() -> void { onIdleTimeout(); });
    connection_idle_timer_->enableTimer(config_.idleTimeout().value());
  }

  if (config_.maxConnectionDuration()) {
    connection_duration_timer_ = read_callbacks_->connection().dispatcher().createTimer(
        [this]() -> void { onConnectionDurationTimeout(); });
    connection_duration_timer_->enableTimer(config_.maxConnectionDuration().value());
  }

  read_callbacks_->connection().setDelayedCloseTimeout(config_.delayedCloseTimeout());

  read_callbacks_->connection().setConnectionStats(
      {stats_.named_.downstream_cx_rx_bytes_total_, stats_.named_.downstream_cx_rx_bytes_buffered_,
       stats_.named_.downstream_cx_tx_bytes_total_, stats_.named_.downstream_cx_tx_bytes_buffered_,
       nullptr, &stats_.named_.downstream_cx_delayed_close_timeout_});
}

ConnectionManagerImpl::~ConnectionManagerImpl() {
  stats_.named_.downstream_cx_destroy_.inc();

  stats_.named_.downstream_cx_active_.dec();
  if (read_callbacks_->connection().ssl()) {
    stats_.named_.downstream_cx_ssl_active_.dec();
  }

  if (codec_) {
    if (codec_->protocol() == Protocol::Http2) {
      stats_.named_.downstream_cx_http2_active_.dec();
    } else if (codec_->protocol() == Protocol::Http3) {
      stats_.named_.downstream_cx_http3_active_.dec();
    } else {
      stats_.named_.downstream_cx_http1_active_.dec();
    }
  }

  conn_length_->complete();
  user_agent_.completeConnectionLength(*conn_length_);
}

void ConnectionManagerImpl::checkForDeferredClose() {
  if (drain_state_ == DrainState::Closing && streams_.empty() && !codec_->wantsToWrite()) {
    read_callbacks_->connection().close(Network::ConnectionCloseType::FlushWriteAndDelay);
  }
}

void ConnectionManagerImpl::doEndStream(ActiveStream& stream) {
  // The order of what happens in this routine is important and a little complicated. We first see
  // if the stream needs to be reset. If it needs to be, this will end up invoking reset callbacks
  // and then moving the stream to the deferred destruction list. If the stream has not been reset,
  // we move it to the deferred deletion list here. Then, we potentially close the connection. This
  // must be done after deleting the stream since the stream refers to the connection and must be
  // deleted first.
  bool reset_stream = false;
  // If the response encoder is still associated with the stream, reset the stream. The exception
  // here is when Envoy "ends" the stream by calling recreateStream at which point recreateStream
  // explicitly nulls out response_encoder to avoid the downstream being notified of the
  // Envoy-internal stream instance being ended.
  if (stream.response_encoder_ != nullptr &&
      (!stream.state_.remote_complete_ || !stream.state_.codec_saw_local_complete_)) {
    // Indicate local is complete at this point so that if we reset during a continuation, we don't
    // raise further data or trailers.
    ENVOY_STREAM_LOG(debug, "doEndStream() resetting stream", stream);
    stream.state_.local_complete_ = true;
    stream.state_.codec_saw_local_complete_ = true;
    stream.response_encoder_->getStream().resetStream(StreamResetReason::LocalReset);
    reset_stream = true;
  }

  if (!reset_stream) {
    doDeferredStreamDestroy(stream);
  }

  if (reset_stream && codec_->protocol() < Protocol::Http2) {
    drain_state_ = DrainState::Closing;
  }

  checkForDeferredClose();

  // Reading may have been disabled for the non-multiplexing case, so enable it again.
  // Also be sure to unwind any read-disable done by the prior downstream
  // connection.
  if (drain_state_ != DrainState::Closing && codec_->protocol() < Protocol::Http2) {
    while (!read_callbacks_->connection().readEnabled()) {
      read_callbacks_->connection().readDisable(false);
    }
  }
}

Network::Connection& ConnectionManagerImpl::connection() { return read_callbacks_->connection(); }

void ConnectionManagerImpl::doDeferredStreamDestroy(ActiveStream& stream) {
  if (stream.stream_idle_timer_ != nullptr) {
    stream.stream_idle_timer_->disableTimer();
    stream.stream_idle_timer_ = nullptr;
  }
  stream.disarmRequestTimeout();

  stream.state_.destroyed_ = true;
  for (auto& filter : stream.decoder_filters_) {
    filter->handle_->onDestroy();
  }

  for (auto& filter : stream.encoder_filters_) {
    // Do not call on destroy twice for dual registered filters.
    if (!filter->dual_filter_) {
      filter->handle_->onDestroy();
    }
  }

  read_callbacks_->connection().dispatcher().deferredDelete(stream.removeFromList(streams_));

  if (connection_idle_timer_ && streams_.empty()) {
    connection_idle_timer_->enableTimer(config_.idleTimeout().value());
  }
}

Protocol ConnectionManagerImpl::protocol() { return codec_->protocol(); }

StreamDecoder& ConnectionManagerImpl::newStream(StreamEncoder& response_encoder,
                                                bool is_internally_created) {
  if (connection_idle_timer_) {
    connection_idle_timer_->disableTimer();
  }

  ENVOY_CONN_LOG(debug, "new stream", read_callbacks_->connection());
  ActiveStreamPtr new_stream(
      new ActiveStream(*this, cluster_manager_, stats_, listener_stats_, config_, random_generator_, time_source_));
  new_stream->state_.is_internally_created_ = is_internally_created;
  new_stream->response_encoder_ = &response_encoder;
  new_stream->response_encoder_->getStream().addCallbacks(*new_stream);
  new_stream->buffer_limit_ = new_stream->response_encoder_->getStream().bufferLimit();
  // If the network connection is backed up, the stream should be made aware of it on creation.
  // Both HTTP/1.x and HTTP/2 codecs handle this in StreamCallbackHelper::addCallbacks_.
  ASSERT(read_callbacks_->connection().aboveHighWatermark() == false ||
         new_stream->high_watermark_count_ > 0);
  new_stream->moveIntoList(std::move(new_stream), streams_);
  return **streams_.begin();
}

void ConnectionManagerImpl::handleCodecException(const char* error) {
  ENVOY_CONN_LOG(debug, "dispatch error: {}", read_callbacks_->connection(), error);

  // In the protocol error case, we need to reset all streams now. The connection might stick around
  // long enough for a pending stream to come back and try to encode.
  resetAllStreams(StreamInfo::ResponseFlag::DownstreamProtocolError);

  // HTTP/1.1 codec has already sent a 400 response if possible. HTTP/2 codec has already sent
  // GOAWAY.
  read_callbacks_->connection().close(Network::ConnectionCloseType::FlushWriteAndDelay);
}

Network::FilterStatus ConnectionManagerImpl::onData(Buffer::Instance& data, bool) {
  if (!codec_) {
    // Http3 codec should have been instantiated by now.
    codec_ = config_.createCodec(read_callbacks_->connection(), data, *this);
    if (codec_->protocol() == Protocol::Http2) {
      stats_.named_.downstream_cx_http2_total_.inc();
      stats_.named_.downstream_cx_http2_active_.inc();
    } else {
      ASSERT(codec_->protocol() != Protocol::Http3);
      stats_.named_.downstream_cx_http1_total_.inc();
      stats_.named_.downstream_cx_http1_active_.inc();
    }
  }

  try {
    codec_->dispatch(data);
  } catch (const FrameFloodException& e) {
    // TODO(mattklein123): This is an emergency substitute for the lack of connection level
    // logging in the HCM. In a public follow up change we will add full support for connection
    // level logging in the HCM, similar to what we have in tcp_proxy. This will allow abuse
    // indicators to be stored in the connection level stream info, and then matched, sampled,
    // etc. when logged.
    const envoy::type::FractionalPercent default_value; // 0
    if (runtime_.snapshot().featureEnabled("http.connection_manager.log_flood_exception",
                                           default_value)) {
      ENVOY_CONN_LOG(warn, "downstream HTTP flood from IP '{}': {}", read_callbacks_->connection(),
                     read_callbacks_->connection().remoteAddress()->asString(), e.what());
    }

    handleCodecException(e.what());
    return Network::FilterStatus::StopIteration;
  } catch (const CodecProtocolException& e) {
    stats_.named_.downstream_cx_protocol_error_.inc();
    handleCodecException(e.what());
    return Network::FilterStatus::StopIteration;
  }

  // Processing incoming data may release outbound data so check for closure here as well.
  checkForDeferredClose();

  // The HTTP/1 codec will pause parsing after a single message is complete. If we have a single
  // complete non-WebSocket stream but have not responded yet we will pause socket reads
  // to apply back pressure.
  if (codec_->protocol() < Protocol::Http2) {
    if (!streams_.empty() && streams_.front()->state_.remote_complete_) {
      read_callbacks_->connection().readDisable(true);
    }
  }

  return Network::FilterStatus::StopIteration;
}

Network::FilterStatus ConnectionManagerImpl::onNewConnection() {
  if (!read_callbacks_->connection().streamInfo().protocol()) {
    // For Non-QUIC traffic, continue passing data to filters.
    return Network::FilterStatus::Continue;
  }
  // Only QUIC connection's stream_info_ specifies protocol.
  Buffer::OwnedImpl dummy;
  codec_ = config_.createCodec(read_callbacks_->connection(), dummy, *this);
  ASSERT(codec_->protocol() == Protocol::Http3);
  stats_.named_.downstream_cx_http3_total_.inc();
  stats_.named_.downstream_cx_http3_active_.inc();
  // Stop iterating through each filters for QUIC. Currently a QUIC connection
  // only supports one filter, HCM, and bypasses the onData() interface. Because
  // QUICHE already handles de-multiplexing.
  return Network::FilterStatus::StopIteration;
}

void ConnectionManagerImpl::resetAllStreams(
    absl::optional<StreamInfo::ResponseFlag> response_flag) {
  while (!streams_.empty()) {
    // Mimic a downstream reset in this case. We must also remove callbacks here. Though we are
    // about to close the connection and will disable further reads, it is possible that flushing
    // data out can cause stream callbacks to fire (e.g., low watermark callbacks).
    //
    // TODO(mattklein123): I tried to actually reset through the codec here, but ran into issues
    // with nghttp2 state and being unhappy about sending reset frames after the connection had
    // been terminated via GOAWAY. It might be possible to do something better here inside the h2
    // codec but there are no easy answers and this seems simpler.
    auto& stream = *streams_.front();
    stream.response_encoder_->getStream().removeCallbacks(stream);
    stream.onResetStream(StreamResetReason::ConnectionTermination, absl::string_view());
    if (response_flag.has_value()) {
      stream.stream_info_.setResponseFlag(response_flag.value());
    }
  }
}

void ConnectionManagerImpl::onEvent(Network::ConnectionEvent event) {
  if (event == Network::ConnectionEvent::LocalClose) {
    stats_.named_.downstream_cx_destroy_local_.inc();
  }

  if (event == Network::ConnectionEvent::RemoteClose) {
    stats_.named_.downstream_cx_destroy_remote_.inc();
  }

  if (event == Network::ConnectionEvent::RemoteClose ||
      event == Network::ConnectionEvent::LocalClose) {
    if (connection_idle_timer_) {
      connection_idle_timer_->disableTimer();
      connection_idle_timer_.reset();
    }

    if (connection_duration_timer_) {
      connection_duration_timer_->disableTimer();
      connection_duration_timer_.reset();
    }

    if (drain_timer_) {
      drain_timer_->disableTimer();
      drain_timer_.reset();
    }
  }

  if (!streams_.empty()) {
    if (event == Network::ConnectionEvent::LocalClose) {
      stats_.named_.downstream_cx_destroy_local_active_rq_.inc();
    }
    if (event == Network::ConnectionEvent::RemoteClose) {
      stats_.named_.downstream_cx_destroy_remote_active_rq_.inc();
    }

    stats_.named_.downstream_cx_destroy_active_rq_.inc();
    user_agent_.onConnectionDestroy(event, true);
    resetAllStreams(absl::nullopt);
  }
}

void ConnectionManagerImpl::onGoAway() {
  // Currently we do nothing with remote go away frames. In the future we can decide to no longer
  // push resources if applicable.
}

void ConnectionManagerImpl::onIdleTimeout() {
  ENVOY_CONN_LOG(debug, "idle timeout", read_callbacks_->connection());
  stats_.named_.downstream_cx_idle_timeout_.inc();
  if (!codec_) {
    // No need to delay close after flushing since an idle timeout has already fired. Attempt to
    // write out buffered data one last time and issue a local close if successful.
    read_callbacks_->connection().close(Network::ConnectionCloseType::FlushWrite);
  } else if (drain_state_ == DrainState::NotDraining) {
    startDrainSequence();
  }
}

void ConnectionManagerImpl::onConnectionDurationTimeout() {
  ENVOY_CONN_LOG(debug, "max connection duration reached", read_callbacks_->connection());
  stats_.named_.downstream_cx_max_duration_reached_.inc();
  if (!codec_) {
    // Attempt to write out buffered data one last time and issue a local close if successful.
    read_callbacks_->connection().close(Network::ConnectionCloseType::FlushWrite);
  } else if (drain_state_ == DrainState::NotDraining) {
    startDrainSequence();
  }
}

void ConnectionManagerImpl::onDrainTimeout() {
  ASSERT(drain_state_ != DrainState::NotDraining);
  codec_->goAway();
  drain_state_ = DrainState::Closing;
  checkForDeferredClose();
}

void ConnectionManagerImpl::startDrainSequence() {
  ASSERT(drain_state_ == DrainState::NotDraining);
  drain_state_ = DrainState::Draining;
  codec_->shutdownNotice();
  drain_timer_ = read_callbacks_->connection().dispatcher().createTimer(
      [this]() -> void { onDrainTimeout(); });
  drain_timer_->enableTimer(config_.drainTimeout());
}

bool ConnectionManagerImpl::updateDrainState(ActiveStream& stream) {
  // See if we want to drain/close the connection. Send the go away frame prior to encoding the
  // header block.
  if (drain_state_ == DrainState::NotDraining && drain_close_.drainClose()) {

    // This doesn't really do anything for HTTP/1.1 other then give the connection another boost
    // of time to race with incoming requests. It mainly just keeps the logic the same between
    // HTTP/1.1 and HTTP/2.
    startDrainSequence();
    stats_.named_.downstream_cx_drain_close_.inc();
    ENVOY_STREAM_LOG(debug, "drain closing connection", stream);
  }

  if (drain_state_ == DrainState::NotDraining && stream.state_.saw_connection_close_) {
    ENVOY_STREAM_LOG(debug, "closing connection due to connection close header", stream);
    drain_state_ = DrainState::Closing;
  }

  if (drain_state_ == DrainState::NotDraining &&
      overload_disable_keepalive_ref_ == Server::OverloadActionState::Active) {
    ENVOY_STREAM_LOG(debug, "disabling keepalive due to envoy overload", stream);
    drain_state_ = DrainState::Closing;
    stats_.named_.downstream_cx_overload_disable_keepalive_.inc();
  }

  // If we are destroying a stream before remote is complete and the connection does not support
  // multiplexing, we should disconnect since we don't want to wait around for the request to
  // finish.
  if (!stream.state_.remote_complete_) {
    if (protocol() < Protocol::Http2) {
      drain_state_ = DrainState::Closing;
    }

    stats_.named_.downstream_rq_response_before_rq_complete_.inc();
  }

  return drain_state_ != DrainState::NotDraining;
}

} // namespace Http
} // namespace Envoy
