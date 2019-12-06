namespace Envoy {
namespace Http {
namespace ConnectionManager {

class StreamControlCallbacks {
public:
  virtual ~StreamControlCallbacks() = default;

  virtual Network::Connection& connection() PURE;
  virtual void doEndStream() PURE;
}

} // namespace ConnectionManager
} // namespace Http
} // namespace Envoy
