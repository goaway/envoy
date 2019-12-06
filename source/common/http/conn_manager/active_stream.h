#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "envoy/access_log/access_log.h"
#include "envoy/buffer/buffer.h"
#include "envoy/common/scope_tracker.h"
#include "envoy/event/deferred_deletable.h"
#include "envoy/http/codec.h"
#include "envoy/http/filter.h"
#include "envoy/http/header_map.h"
#include "envoy/network/connection.h"
#include "envoy/router/router.h"
#include "envoy/router/scopes.h"
#include "envoy/stats/scope.h"
#include "envoy/stats/stats_macros.h"
#include "envoy/stats/timespan.h"
#include "envoy/upstream/upstream.h"
#include "envoy/tracing/http_tracer.h"

#include "common/buffer/watermark_buffer.h"
#include "common/common/dump_state_utils.h"
#include "common/common/linked_object.h"
#include "common/grpc/common.h"
#include "common/http/conn_manager/stream_control_callbacks.h"
#include "common/http/conn_manager_config.h"
#include "common/http/utility.h"
#include "common/stream_info/stream_info_impl.h"

namespace Envoy {
namespace Http {
namespace ConnectionManager {

struct ActiveStreamFilterBase;
struct ActiveStreamDecoderFilter;
struct ActiveStreamEncoderFilter;

using ActiveStreamEncoderFilterPtr = std::unique_ptr<ActiveStreamEncoderFilter>;
using ActiveStreamDecoderFilterPtr = std::unique_ptr<ActiveStreamDecoderFilter>;

/**
 * Wraps a single active stream on the connection. These are either full request/response pairs
 * or pushes.
 */
struct ActiveStream : LinkedObject<ActiveStream>,
                      public Event::DeferredDeletable,
                      public StreamCallbacks,
                      public StreamDecoder,
                      public FilterChainFactoryCallbacks,
                      public Tracing::Config,
                      public ScopeTrackedObject {
  ActiveStream(StreamControlCallbacks& stream_control_callbacks, ConnectionManagerStats& connection_manager_stats,
               ConnectionManagerListenerStats& listener_stats,
               ConnectionManagerConfig& connection_manager_config, Runtime::RandomGenerator& random_generator, TimeSource& time_source);
  ~ActiveStream() override;

  // Indicates which filter to start the iteration with.
  enum class FilterIterationStartState { AlwaysStartFromNext, CanStartFromCurrent };

  void addStreamDecoderFilterWorker(StreamDecoderFilterSharedPtr filter, bool dual_filter);
  void addStreamEncoderFilterWorker(StreamEncoderFilterSharedPtr filter, bool dual_filter);
  void chargeStats(const HeaderMap& headers);
  // Returns the encoder filter to start iteration with.
  std::list<ActiveStreamEncoderFilterPtr>::iterator
  commonEncodePrefix(ActiveStreamEncoderFilter* filter, bool end_stream,
                     FilterIterationStartState filter_iteration_start_state);
  // Returns the decoder filter to start iteration with.
  std::list<ActiveStreamDecoderFilterPtr>::iterator
  commonDecodePrefix(ActiveStreamDecoderFilter* filter,
                     FilterIterationStartState filter_iteration_start_state);
  const Network::Connection* connection();
  void addDecodedData(ActiveStreamDecoderFilter& filter, Buffer::Instance& data, bool streaming);
  HeaderMap& addDecodedTrailers();
  MetadataMapVector& addDecodedMetadata();
  void decodeHeaders(ActiveStreamDecoderFilter* filter, HeaderMap& headers, bool end_stream);
  // Sends data through decoding filter chains. filter_iteration_start_state indicates which
  // filter to start the iteration with.
  void decodeData(ActiveStreamDecoderFilter* filter, Buffer::Instance& data, bool end_stream,
                  FilterIterationStartState filter_iteration_start_state);
  void decodeTrailers(ActiveStreamDecoderFilter* filter, HeaderMap& trailers);
  void decodeMetadata(ActiveStreamDecoderFilter* filter, MetadataMap& metadata_map);
  void disarmRequestTimeout();
  void maybeEndDecode(bool end_stream);
  void addEncodedData(ActiveStreamEncoderFilter& filter, Buffer::Instance& data, bool streaming);
  HeaderMap& addEncodedTrailers();
  void sendLocalReply(bool is_grpc_request, Code code, absl::string_view body,
                      const std::function<void(HeaderMap& headers)>& modify_headers,
                      bool is_head_request,
                      const absl::optional<Grpc::Status::GrpcStatus> grpc_status,
                      absl::string_view details);
  void encode100ContinueHeaders(ActiveStreamEncoderFilter* filter, HeaderMap& headers);
  void encodeHeaders(ActiveStreamEncoderFilter* filter, HeaderMap& headers, bool end_stream);
  // Sends data through encoding filter chains. filter_iteration_start_state indicates which
  // filter to start the iteration with.
  void encodeData(ActiveStreamEncoderFilter* filter, Buffer::Instance& data, bool end_stream,
                  FilterIterationStartState filter_iteration_start_state);
  void encodeTrailers(ActiveStreamEncoderFilter* filter, HeaderMap& trailers);
  void encodeMetadata(ActiveStreamEncoderFilter* filter, MetadataMapPtr&& metadata_map_ptr);
  void maybeEndEncode(bool end_stream);
  // Returns true if new metadata is decoded. Otherwise, returns false.
  bool processNewlyAddedMetadata();
  uint64_t streamId() { return stream_id_; }
  // Returns true if filter has stopped iteration for all frame types. Otherwise, returns false.
  // filter_streaming is the variable to indicate if stream is streaming, and its value may be
  // changed by the function.
  bool handleDataIfStopAll(ActiveStreamFilterBase& filter, Buffer::Instance& data,
                           bool& filter_streaming);

  // Http::StreamCallbacks
  void onResetStream(StreamResetReason reason, absl::string_view transport_failure_reason) override;
  void onAboveWriteBufferHighWatermark() override;
  void onBelowWriteBufferLowWatermark() override;

  // Http::StreamDecoder
  void decode100ContinueHeaders(HeaderMapPtr&&) override { NOT_REACHED_GCOVR_EXCL_LINE; }
  void decodeHeaders(HeaderMapPtr&& headers, bool end_stream) override;
  void decodeData(Buffer::Instance& data, bool end_stream) override;
  void decodeTrailers(HeaderMapPtr&& trailers) override;
  void decodeMetadata(MetadataMapPtr&&) override;

  // Http::FilterChainFactoryCallbacks
  void addStreamDecoderFilter(StreamDecoderFilterSharedPtr filter) override {
    addStreamDecoderFilterWorker(filter, false);
  }
  void addStreamEncoderFilter(StreamEncoderFilterSharedPtr filter) override {
    addStreamEncoderFilterWorker(filter, false);
  }
  void addStreamFilter(StreamFilterSharedPtr filter) override {
    addStreamDecoderFilterWorker(filter, true);
    addStreamEncoderFilterWorker(filter, true);
  }
  void addAccessLogHandler(AccessLog::InstanceSharedPtr handler) override;

  // Tracing::TracingConfig
  Tracing::OperationName operationName() const override;
  const Tracing::CustomTagMap* customTags() const override;
  bool verbose() const override;
  uint32_t maxPathTagLength() const override;

  // ScopeTrackedObject
  void dumpState(std::ostream& os, int indent_level = 0) const override {
    const char* spaces = spacesForLevel(indent_level);
    os << spaces << "ActiveStream " << this << DUMP_MEMBER(stream_id_)
       << DUMP_MEMBER(has_continue_headers_) << DUMP_MEMBER(is_head_request_)
       << DUMP_MEMBER(decoding_headers_only_) << DUMP_MEMBER(encoding_headers_only_) << "\n";

    DUMP_DETAILS(request_headers_);
    DUMP_DETAILS(request_trailers_);
    DUMP_DETAILS(response_headers_);
    DUMP_DETAILS(response_trailers_);
    DUMP_DETAILS(&stream_info_);
  }

  void traceRequest();

  // Updates the snapped_route_config_ (by reselecting scoped route configuration), if a scope is
  // not found, snapped_route_config_ is set to Router::NullConfigImpl.
  void snapScopedRouteConfig();

  void refreshCachedRoute();

  void refreshCachedTracingCustomTags();

  // Pass on watermark callbacks to watermark subscribers. This boils down to passing watermark
  // events for this stream and the downstream connection to the router filter.
  void callHighWatermarkCallbacks();
  void callLowWatermarkCallbacks();

  /**
   * Flags that keep track of which filter calls are currently in progress.
   */
  // clang-format off
  struct FilterCallState {
    static constexpr uint32_t DecodeHeaders   = 0x01;
    static constexpr uint32_t DecodeData      = 0x02;
    static constexpr uint32_t DecodeTrailers  = 0x04;
    static constexpr uint32_t EncodeHeaders   = 0x08;
    static constexpr uint32_t EncodeData      = 0x10;
    static constexpr uint32_t EncodeTrailers  = 0x20;
    // Encode100ContinueHeaders is a bit of a special state as 100 continue
    // headers may be sent during request processing. This state is only used
    // to verify we do not encode100Continue headers more than once per
    // filter.
    static constexpr uint32_t Encode100ContinueHeaders  = 0x40;
    // Used to indicate that we're processing the final [En|De]codeData frame,
    // i.e. end_stream = true
    static constexpr uint32_t LastDataFrame = 0x80;
  };
  // clang-format on

  // All state for the stream. Put here for readability.
  struct State {
    State()
        : remote_complete_(false), local_complete_(false), codec_saw_local_complete_(false),
          saw_connection_close_(false), successful_upgrade_(false), created_filter_chain_(false),
          is_internally_created_(false) {}

    uint32_t filter_call_state_{0};
    // The following 3 members are booleans rather than part of the space-saving bitfield as they
    // are passed as arguments to functions expecting bools. Extend State using the bitfield
    // where possible.
    bool encoder_filters_streaming_{true};
    bool decoder_filters_streaming_{true};
    bool destroyed_{false};
    bool remote_complete_ : 1;
    bool local_complete_ : 1; // This indicates that local is complete prior to filter processing.
                              // A filter can still stop the stream from being complete as seen
                              // by the codec.
    bool codec_saw_local_complete_ : 1; // This indicates that local is complete as written all
                                        // the way through to the codec.
    bool saw_connection_close_ : 1;
    bool successful_upgrade_ : 1;
    bool created_filter_chain_ : 1;

    // True if this stream is internally created. Currently only used for
    // internal redirects or other streams created via recreateStream().
    bool is_internally_created_ : 1;

    // Used to track which filter is the latest filter that has received data.
    ActiveStreamEncoderFilter* latest_data_encoding_filter_{};
    ActiveStreamDecoderFilter* latest_data_decoding_filter_{};
  };

  // Possibly increases buffer_limit_ to the value of limit.
  void setBufferLimit(uint32_t limit);
  // Set up the Encoder/Decoder filter chain.
  bool createFilterChain();
  // Per-stream idle timeout callback.
  void onIdleTimeout();
  // Reset per-stream idle timer.
  void resetIdleTimer();
  // Per-stream request timeout callback
  void onRequestTimeout();

  bool hasCachedRoute() { return cached_route_.has_value() && cached_route_.value(); }

  friend std::ostream& operator<<(std::ostream& os, const ActiveStream& s) {
    s.dumpState(os);
    return os;
  }

  MetadataMapVector* getRequestMetadataMapVector() {
    if (request_metadata_map_vector_ == nullptr) {
      request_metadata_map_vector_ = std::make_unique<MetadataMapVector>();
    }
    return request_metadata_map_vector_.get();
  }

  Tracing::CustomTagMap& getOrMakeTracingCustomTagMap() {
    if (tracing_custom_tags_ == nullptr) {
      tracing_custom_tags_ = std::make_unique<Tracing::CustomTagMap>();
    }
    return *tracing_custom_tags_;
  }

  StreamControlCallbacks& stream_control_callbacks_;
  ConnectionManagerStats& connection_manager_stats_;
  ConnectionManagerListenerStats& listener_stats_;
  ConnectionManagerConfig& connection_manager_config_;
  Runtime::RandomGenerator& random_generator_;
  Router::ConfigConstSharedPtr snapped_route_config_;
  Router::ScopedConfigConstSharedPtr snapped_scoped_routes_config_;
  Tracing::SpanPtr active_span_;
  const uint64_t stream_id_;
  StreamEncoder* response_encoder_{};
  HeaderMapPtr continue_headers_;
  HeaderMapPtr response_headers_;
  Buffer::WatermarkBufferPtr buffered_response_data_;
  HeaderMapPtr response_trailers_{};
  HeaderMapPtr request_headers_;
  Buffer::WatermarkBufferPtr buffered_request_data_;
  HeaderMapPtr request_trailers_;
  std::list<ActiveStreamDecoderFilterPtr> decoder_filters_;
  std::list<ActiveStreamEncoderFilterPtr> encoder_filters_;
  std::list<AccessLog::InstanceSharedPtr> access_log_handlers_;
  Stats::TimespanPtr request_response_timespan_;
  // Per-stream idle timeout.
  Event::TimerPtr stream_idle_timer_;
  // Per-stream request timeout.
  Event::TimerPtr request_timer_;
  std::chrono::milliseconds idle_timeout_ms_{};
  State state_;
  StreamInfo::StreamInfoImpl stream_info_;
  absl::optional<Router::RouteConstSharedPtr> cached_route_;
  absl::optional<Upstream::ClusterInfoConstSharedPtr> cached_cluster_info_;
  std::list<DownstreamWatermarkCallbacks*> watermark_callbacks_{};
  // Stores metadata added in the decoding filter that is being processed. Will be cleared before
  // processing the next filter. The storage is created on demand. We need to store metadata
  // temporarily in the filter in case the filter has stopped all while processing headers.
  std::unique_ptr<MetadataMapVector> request_metadata_map_vector_{nullptr};
  uint32_t buffer_limit_{0};
  uint32_t high_watermark_count_{0};
  const std::string* decorated_operation_{nullptr};
  // By default, we will assume there are no 100-Continue headers. If encode100ContinueHeaders
  // is ever called, this is set to true so commonContinue resumes processing the 100-Continue.
  bool has_continue_headers_{};
  bool is_head_request_{};
  // Whether a filter has indicated that the request should be treated as a headers only request.
  bool decoding_headers_only_{};
  // Whether a filter has indicated that the response should be treated as a headers only
  // response.
  bool encoding_headers_only_{};
  Network::Socket::OptionsSharedPtr upstream_options_;
  std::unique_ptr<Tracing::CustomTagMap> tracing_custom_tags_{nullptr};
};

using ActiveStreamPtr = std::unique_ptr<ActiveStream>;

} // namespace ConnectionManager
} // namespace Http
} // namespace Envoy
