#pragma once

#include "envoy/event/dispatcher.h"
#include "envoy/http/protocol.h"
#include "envoy/local_info/local_info.h"
#include "envoy/network/connection.h"
#include "envoy/runtime/runtime.h"
#include "envoy/tracing/http_tracer.h"
#include "envoy/upstream/cluster_manager.h"

namespace Envoy {
namespace Http {

/**
 * Wrapper for static state owned by the ConnectionManager which an ActiveStream (currently) needs to be able to access.
 */
struct ConnectionManagerInfo {
public:
  Tracing::HttpTracer& tracer_;
  TimeSource& time_source_;
  Runtime::Loader& runtime_;
  const LocalInfo::LocalInfo& local_info_;
  Event::Dispatcher& dispatcher_;
  Upstream::ClusterManager& cluster_manager_;
  Runtime::RandomGenerator& random_generator_;

  // FIXME Discuss:
  // Need to expose this because ActiveStreamBaseFilter needs to implement StreamFilterCallbacks.
  Network::Connection& connection_;
};

} // namespace Http
} // namespace Envoy
