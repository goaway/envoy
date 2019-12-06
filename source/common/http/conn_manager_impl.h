#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "envoy/access_log/access_log.h"
#include "envoy/common/scope_tracker.h"
#include "envoy/event/deferred_deletable.h"
#include "envoy/http/codec.h"
#include "envoy/http/codes.h"
#include "envoy/http/context.h"
#include "envoy/http/filter.h"
#include "envoy/network/connection.h"
#include "envoy/network/drain_decision.h"
#include "envoy/network/filter.h"
#include "envoy/router/rds.h"
#include "envoy/router/scopes.h"
#include "envoy/runtime/runtime.h"
#include "envoy/server/overload_manager.h"
#include "envoy/ssl/connection.h"
#include "envoy/stats/scope.h"
#include "envoy/stats/stats_macros.h"
#include "envoy/upstream/upstream.h"

#include "common/buffer/watermark_buffer.h"
#include "common/common/dump_state_utils.h"
#include "common/common/linked_object.h"
#include "common/grpc/common.h"
#include "common/http/conn_manager_config.h"
#include "common/http/user_agent.h"
#include "common/http/utility.h"
#include "common/stream_info/stream_info_impl.h"
#include "common/tracing/http_tracer_impl.h"

namespace Envoy {
namespace Http {

namespace ConnectionManager {
struct ActiveStream;
} // namespace ConnectionManager

using ActiveStream = ConnectionManager::ActiveStream;
using ActiveStreamPtr = std::unique_ptr<ConnectionManager::ActiveStream>;
/**
 * Implementation of both ConnectionManager and ServerConnectionCallbacks. This is a
 * Network::Filter that can be installed on a connection that will perform HTTP protocol agnostic
 * handling of a connection and all requests/pushes that occur on a connection.
 */
class ConnectionManagerImpl : Logger::Loggable<Logger::Id::http>,
                              public Network::ReadFilter,
                              public ServerConnectionCallbacks,
                              public Network::ConnectionCallbacks {
public:
  ConnectionManagerImpl(ConnectionManagerConfig& config, const Network::DrainDecision& drain_close,
                        Runtime::RandomGenerator& random_generator, Http::Context& http_context,
                        Runtime::Loader& runtime, const LocalInfo::LocalInfo& local_info,
                        Upstream::ClusterManager& cluster_manager,
                        Server::OverloadManager* overload_manager, TimeSource& time_system);
  ~ConnectionManagerImpl() override;

  static ConnectionManagerStats generateStats(const std::string& prefix, Stats::Scope& scope);
  static ConnectionManagerTracingStats generateTracingStats(const std::string& prefix,
                                                            Stats::Scope& scope);
  static void chargeTracingStats(const Tracing::Reason& tracing_reason,
                                 ConnectionManagerTracingStats& tracing_stats);
  static ConnectionManagerListenerStats generateListenerStats(const std::string& prefix,
                                                              Stats::Scope& scope);
  static const HeaderMapImpl& continueHeader();

  // Network::ReadFilter
  Network::FilterStatus onData(Buffer::Instance& data, bool end_stream) override;
  Network::FilterStatus onNewConnection() override;
  void initializeReadFilterCallbacks(Network::ReadFilterCallbacks& callbacks) override;

  // Http::ConnectionCallbacks
  void onGoAway() override;

  // Http::ServerConnectionCallbacks
  StreamDecoder& newStream(StreamEncoder& response_encoder,
                           bool is_internally_created = false) override;

  // Network::ConnectionCallbacks
  void onEvent(Network::ConnectionEvent event) override;
  // Pass connection watermark events on to all the streams associated with that connection.
  void onAboveWriteBufferHighWatermark() override {
    codec_->onUnderlyingConnectionAboveWriteBufferHighWatermark();
  }
  void onBelowWriteBufferLowWatermark() override {
    codec_->onUnderlyingConnectionBelowWriteBufferLowWatermark();
  }

  TimeSource& timeSource() { return time_source_; }

  // NOTE: perhaps change this accessors to specialized function calls. And change visibility to
  // private.
  const Network::DrainDecision& drainClose() const { return drain_close_; }
  Runtime::RandomGenerator& randomGenerator() const { return random_generator_; }
  const LocalInfo::LocalInfo& localInfo() const { return local_info_; }
  Upstream::ClusterManager& clusterManager() const { return cluster_manager_; }

private:
  /**
   * Check to see if the connection can be closed after gracefully waiting to send pending codec
   * data.
   */
  void checkForDeferredClose();

  /**
   * Do a delayed destruction of a stream to allow for stack unwind. Also calls onDestroy() for
   * each filter.
   */
  void doDeferredStreamDestroy(ActiveStream& stream);

  /**
   * Process a stream that is ending due to upstream response or reset.
   */
  void doEndStream(ActiveStream& stream);

  void resetAllStreams(absl::optional<StreamInfo::ResponseFlag> response_flag);
  void onIdleTimeout();
  void onConnectionDurationTimeout();
  void onDrainTimeout();
  void startDrainSequence();
  Tracing::HttpTracer& tracer() { return http_context_.tracer(); }
  void handleCodecException(const char* error);

  enum class DrainState { NotDraining, Draining, Closing };

  ConnectionManagerConfig& config_;
  ConnectionManagerStats& stats_; // We store a reference here to avoid an extra stats() call on the
                                  // config in the hot path.
  ServerConnectionPtr codec_;
  std::list<ActiveStreamPtr> streams_;
  Stats::TimespanPtr conn_length_;
  const Network::DrainDecision& drain_close_;
  DrainState drain_state_{DrainState::NotDraining};
  UserAgent user_agent_;
  // An idle timer for the connection. This is only armed when there are no streams on the
  // connection. When there are active streams it is disarmed in favor of each stream's
  // stream_idle_timer_.
  Event::TimerPtr connection_idle_timer_;
  // A connection duration timer. Armed during handling new connection if enabled in config.
  Event::TimerPtr connection_duration_timer_;
  Event::TimerPtr drain_timer_;
  Runtime::RandomGenerator& random_generator_;
  Http::Context& http_context_;
  Runtime::Loader& runtime_;
  const LocalInfo::LocalInfo& local_info_;
  Upstream::ClusterManager& cluster_manager_;
  Network::ReadFilterCallbacks* read_callbacks_{};
  ConnectionManagerListenerStats& listener_stats_;
  // References into the overload manager thread local state map. Using these lets us avoid a map
  // lookup in the hot path of processing each request.
  const Server::OverloadActionState& overload_stop_accepting_requests_ref_;
  const Server::OverloadActionState& overload_disable_keepalive_ref_;
  TimeSource& time_source_;
};

} // namespace Http
} // namespace Envoy
