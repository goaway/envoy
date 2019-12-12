#pragma once

#include "envoy/http/codec.h"
#include "envoy/http/header_map.h"

namespace Envoy {
namespace Http {
namespace ConnectionManager {

struct ActiveStream;

class StreamManager {
public:
  virtual ~StreamManager() = default;
  /**
   * Process a stream that is ending due to upstream response or reset.
   */
  virtual void doEndStream(ActiveStream&) PURE;

  /**
   * Do a delayed destruction of a stream to allow for stack unwind. Also calls onDestroy() for
   * each filter.
   */
  virtual void doDeferredStreamDestroy(ActiveStream&) PURE;
  virtual bool updateDrainState(ActiveStream&) PURE;
  virtual bool isOverloaded() PURE;                                     // acceptStream()
  virtual void initializeUserAgentFromHeaders(HeaderMap& headers) PURE; // preprocessHeaders()
  virtual StreamDecoder& newStream(StreamEncoder& response_encoder,
                                   bool is_internally_created) PURE;
};

} // namespace ConnectionManager
} // namespace Http
} // namespace Envoy
