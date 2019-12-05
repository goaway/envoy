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
#include "envoy/tracing/http_tracer.h"

#include "common/buffer/watermark_buffer.h"
#include "common/common/dump_state_utils.h"
#include "common/common/linked_object.h"
#include "common/grpc/common.h"
#include "common/http/conn_manager/active_stream.h"
#include "common/http/conn_manager_config.h"
#include "common/http/user_agent.h"
#include "common/http/utility.h"
#include "common/stream_info/stream_info_impl.h"

namespace Envoy {
namespace Http {
namespace ConnectionManager {

/**
 * Base class wrapper for both stream encoder and decoder filters.
 */
struct ActiveStreamFilterBase : public virtual StreamFilterCallbacks {
  ActiveStreamFilterBase(ActiveStream& parent, bool dual_filter)
      : parent_(parent), iteration_state_(IterationState::Continue),
        iterate_from_current_filter_(false), headers_continued_(false),
        continue_headers_continued_(false), end_stream_(false), dual_filter_(dual_filter),
        decode_headers_called_(false), encode_headers_called_(false) {}

  // Functions in the following block are called after the filter finishes processing
  // corresponding data. Those functions handle state updates and data storage (if needed)
  // according to the status returned by filter's callback functions.
  bool commonHandleAfter100ContinueHeadersCallback(FilterHeadersStatus status);
  bool commonHandleAfterHeadersCallback(FilterHeadersStatus status, bool& headers_only);
  bool commonHandleAfterDataCallback(FilterDataStatus status, Buffer::Instance& provided_data,
                                     bool& buffer_was_streaming);
  bool commonHandleAfterTrailersCallback(FilterTrailersStatus status);

  // Buffers provided_data.
  void commonHandleBufferData(Buffer::Instance& provided_data);

  // If iteration has stopped for all frame types, calls this function to buffer the data before
  // the filter processes data. The function also updates streaming state.
  void commonBufferDataIfStopAll(Buffer::Instance& provided_data, bool& buffer_was_streaming);

  void commonContinue();
  virtual bool canContinue() PURE;
  virtual Buffer::WatermarkBufferPtr createBuffer() PURE;
  virtual Buffer::WatermarkBufferPtr& bufferedData() PURE;
  virtual bool complete() PURE;
  virtual void do100ContinueHeaders() PURE;
  virtual void doHeaders(bool end_stream) PURE;
  virtual void doData(bool end_stream) PURE;
  virtual void doTrailers() PURE;
  virtual const HeaderMapPtr& trailers() PURE;
  virtual void doMetadata() PURE;
  // TODO(soya3129): make this pure when adding impl to encoder filter.
  virtual void handleMetadataAfterHeadersCallback() PURE;

  // Http::StreamFilterCallbacks
  const Network::Connection* connection() override;
  Event::Dispatcher& dispatcher() override;
  void resetStream() override;
  Router::RouteConstSharedPtr route() override;
  Upstream::ClusterInfoConstSharedPtr clusterInfo() override;
  void clearRouteCache() override;
  uint64_t streamId() override;
  StreamInfo::StreamInfo& streamInfo() override;
  Tracing::Span& activeSpan() override;
  Tracing::Config& tracingConfig() override;
  const ScopeTrackedObject& scope() override { return parent_; }

  // Functions to set or get iteration state.
  bool canIterate() { return iteration_state_ == IterationState::Continue; }
  bool stoppedAll() {
    return iteration_state_ == IterationState::StopAllBuffer ||
           iteration_state_ == IterationState::StopAllWatermark;
  }
  void allowIteration() {
    ASSERT(iteration_state_ != IterationState::Continue);
    iteration_state_ = IterationState::Continue;
  }
  MetadataMapVector* getSavedRequestMetadata() {
    if (saved_request_metadata_ == nullptr) {
      saved_request_metadata_ = std::make_unique<MetadataMapVector>();
    }
    return saved_request_metadata_.get();
  }
  MetadataMapVector* getSavedResponseMetadata() {
    if (saved_response_metadata_ == nullptr) {
      saved_response_metadata_ = std::make_unique<MetadataMapVector>();
    }
    return saved_response_metadata_.get();
  }

  // A vector to save metadata when the current filter's [de|en]codeMetadata() can not be called,
  // either because [de|en]codeHeaders() of the current filter returns StopAllIteration or because
  // [de|en]codeHeaders() adds new metadata to [de|en]code, but we don't know
  // [de|en]codeHeaders()'s return value yet. The storage is created on demand.
  std::unique_ptr<MetadataMapVector> saved_request_metadata_{nullptr};
  std::unique_ptr<MetadataMapVector> saved_response_metadata_{nullptr};
  // The state of iteration.
  enum class IterationState {
    Continue,            // Iteration has not stopped for any frame type.
    StopSingleIteration, // Iteration has stopped for headers, 100-continue, or data.
    StopAllBuffer,       // Iteration has stopped for all frame types, and following data should
                         // be buffered.
    StopAllWatermark,    // Iteration has stopped for all frame types, and following data should
                         // be buffered until high watermark is reached.
  };
  ActiveStream& parent_;
  IterationState iteration_state_;
  // If the filter resumes iteration from a StopAllBuffer/Watermark state, the current filter
  // hasn't parsed data and trailers. As a result, the filter iteration should start with the
  // current filter instead of the next one. If true, filter iteration starts with the current
  // filter. Otherwise, starts with the next filter in the chain.
  bool iterate_from_current_filter_ : 1;
  bool headers_continued_ : 1;
  bool continue_headers_continued_ : 1;
  // If true, end_stream is called for this filter.
  bool end_stream_ : 1;
  const bool dual_filter_ : 1;
  bool decode_headers_called_ : 1;
  bool encode_headers_called_ : 1;
};

} // namespace ConnectionManager
} // namespace Http
} // namespace Envoy
