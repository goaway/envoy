#pragma once

#include "envoy/http/codec.h"
#include "envoy/http/header_map.h"
#include "envoy/http/protocol.h"
#include "envoy/local_info/local_info.h"
#include "envoy/network/connection.h"
#include "envoy/runtime/runtime.h"
#include "envoy/tracing/http_tracer.h"

namespace Envoy {
namespace Http {
namespace ConnectionManager {

struct ActiveStream;

class StreamManager {
public:
  virtual ~StreamManager() = default;

  virtual Network::Connection& connection() PURE; // can be replaced by dispatcher + stream_info

  /**
   * Process a stream that is ending due to upstream response or reset.
   */
  virtual void doEndStream(ActiveStream&) PURE;

  /**
   * Do a delayed destruction of a stream to allow for stack unwind. Also calls onDestroy() for
   * each filter.
   */
  virtual void doDeferredStreamDestroy(ActiveStream&) PURE;
  virtual Protocol protocol() PURE; // stream_info_

  // NEW
  virtual Tracing::HttpTracer& tracer() PURE; // tracing cleanup?
  virtual Runtime::Loader& runtime() PURE; // tracing cleanup?
  virtual const LocalInfo::LocalInfo& localInfo() PURE; // stream_info_

  virtual bool updateDrainState(ActiveStream&) PURE;
  virtual bool isOverloaded() PURE; // acceptStream()
  virtual void initializeUserAgentFromHeaders(HeaderMap& headers) PURE; // preprocessHeaders()
  virtual StreamDecoder& newStream(StreamEncoder& response_encoder,
                                   bool is_internally_created) PURE;
};

} // namespace ConnectionManager
} // namespace Http
} // namespace Envoy
