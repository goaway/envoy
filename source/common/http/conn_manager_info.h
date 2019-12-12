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

class ConnectionManagerInfo {
public:
  virtual ~ConnectionManagerInfo() = default;

  virtual Tracing::HttpTracer& tracer() PURE;
  virtual TimeSource& timeSource() PURE;
  virtual Runtime::Loader& runtime() PURE;
  virtual const LocalInfo::LocalInfo& localInfo() PURE;
  virtual Event::Dispatcher& dispatcher() PURE;
  virtual Upstream::ClusterManager& clusterManager() PURE;
  virtual Runtime::RandomGenerator& randomGenerator() PURE;

  // FIXME Discuss:
  // Need to expose this because ActiveStreamBaseFilter needs to implement StreamFilterCallbacks.
  virtual Network::Connection& connection() PURE;
  // Needed because apparently the codec protocol can shift.
  // Take a look at ActiveStream::decodeHeaders.
  virtual Protocol protocol() PURE;
};

} // namespace Http
} // namespace Envoy