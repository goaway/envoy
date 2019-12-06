#include "envoy/network/connection.h"

namespace Envoy {
namespace Http {
namespace ConnectionManager {

class StreamControlCallbacks {
public:
  virtual ~StreamControlCallbacks() = default;

  virtual Network::Connection& connection() PURE;
    /**
   * Process a stream that is ending due to upstream response or reset.
   */
  virtual void doEndStream(ActiveStream&) PURE;
    /**
   * Do a delayed destruction of a stream to allow for stack unwind. Also calls onDestroy() for
   * each filter.
   */
  virtual void doDeferredStreamDestroy(ActiveStream&) PURE;
  virtual Protocol protocol() PURE;
}

} // namespace ConnectionManager
} // namespace Http
} // namespace Envoy
