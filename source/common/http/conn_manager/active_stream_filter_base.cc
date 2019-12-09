#include "common/http/conn_manager/active_stream_filter_base.h"

#include "common/tracing/http_tracer_impl.h"

namespace Envoy {
namespace Http {
namespace ConnectionManager {

void ActiveStreamFilterBase::commonContinue() {
  // TODO(mattklein123): Raise an error if this is called during a callback.
  if (!canContinue()) {
    ENVOY_STREAM_LOG(trace, "cannot continue filter chain: filter={}", parent_,
                     static_cast<const void*>(this));
    return;
  }

  ENVOY_STREAM_LOG(trace, "continuing filter chain: filter={}", parent_,
                   static_cast<const void*>(this));
  ASSERT(!canIterate());
  // If iteration has stopped for all frame types, set iterate_from_current_filter_ to true so the
  // filter iteration starts with the current filter instead of the next one.
  if (stoppedAll()) {
    iterate_from_current_filter_ = true;
  }
  allowIteration();

  // Only resume with do100ContinueHeaders() if we've actually seen a 100-Continue.
  if (parent_.has_continue_headers_ && !continue_headers_continued_) {
    continue_headers_continued_ = true;
    do100ContinueHeaders();
    // If the response headers have not yet come in, don't continue on with
    // headers and body. doHeaders expects request headers to exist.
    if (!parent_.response_headers_.get()) {
      return;
    }
  }

  // Make sure that we handle the zero byte data frame case. We make no effort to optimize this
  // case in terms of merging it into a header only request/response. This could be done in the
  // future.
  if (!headers_continued_) {
    headers_continued_ = true;
    doHeaders(complete() && !bufferedData() && !trailers());
  }

  doMetadata();

  if (bufferedData()) {
    doData(complete() && !trailers());
  }

  if (trailers()) {
    doTrailers();
  }

  iterate_from_current_filter_ = false;
}

bool ActiveStreamFilterBase::commonHandleAfter100ContinueHeadersCallback(
    FilterHeadersStatus status) {
  ASSERT(parent_.has_continue_headers_);
  ASSERT(!continue_headers_continued_);
  ASSERT(canIterate());

  if (status == FilterHeadersStatus::StopIteration) {
    iteration_state_ = IterationState::StopSingleIteration;
    return false;
  } else {
    ASSERT(status == FilterHeadersStatus::Continue);
    continue_headers_continued_ = true;
    return true;
  }
}

bool ActiveStreamFilterBase::commonHandleAfterHeadersCallback(FilterHeadersStatus status,
                                                              bool& headers_only) {
  ASSERT(!headers_continued_);
  ASSERT(canIterate());

  if (status == FilterHeadersStatus::StopIteration) {
    iteration_state_ = IterationState::StopSingleIteration;
  } else if (status == FilterHeadersStatus::StopAllIterationAndBuffer) {
    iteration_state_ = IterationState::StopAllBuffer;
  } else if (status == FilterHeadersStatus::StopAllIterationAndWatermark) {
    iteration_state_ = IterationState::StopAllWatermark;
  } else if (status == FilterHeadersStatus::ContinueAndEndStream) {
    // Set headers_only to true so we know to end early if necessary,
    // but continue filter iteration so we actually write the headers/run the cleanup code.
    headers_only = true;
    ENVOY_STREAM_LOG(debug, "converting to headers only", parent_);
  } else {
    ASSERT(status == FilterHeadersStatus::Continue);
    headers_continued_ = true;
  }

  handleMetadataAfterHeadersCallback();

  if (stoppedAll() || status == FilterHeadersStatus::StopIteration) {
    return false;
  } else {
    return true;
  }
}

void ActiveStreamFilterBase::commonHandleBufferData(Buffer::Instance& provided_data) {

  // The way we do buffering is a little complicated which is why we have this common function
  // which is used for both encoding and decoding. When data first comes into our filter pipeline,
  // we send it through. Any filter can choose to stop iteration and buffer or not. If we then
  // continue iteration in the future, we use the buffered data. A future filter can stop and
  // buffer again. In this case, since we are already operating on buffered data, we don't
  // rebuffer, because we assume the filter has modified the buffer as it wishes in place.
  if (bufferedData().get() != &provided_data) {
    if (!bufferedData()) {
      bufferedData() = createBuffer();
    }
    bufferedData()->move(provided_data);
  }
}

bool ActiveStreamFilterBase::commonHandleAfterDataCallback(FilterDataStatus status,
                                                           Buffer::Instance& provided_data,
                                                           bool& buffer_was_streaming) {

  if (status == FilterDataStatus::Continue) {
    if (iteration_state_ == IterationState::StopSingleIteration) {
      commonHandleBufferData(provided_data);
      commonContinue();
      return false;
    } else {
      ASSERT(headers_continued_);
    }
  } else {
    iteration_state_ = IterationState::StopSingleIteration;
    if (status == FilterDataStatus::StopIterationAndBuffer ||
        status == FilterDataStatus::StopIterationAndWatermark) {
      buffer_was_streaming = status == FilterDataStatus::StopIterationAndWatermark;
      commonHandleBufferData(provided_data);
    } else if (complete() && !trailers() && !bufferedData()) {
      // If this filter is doing StopIterationNoBuffer and this stream is terminated with a zero
      // byte data frame, we need to create an empty buffer to make sure that when commonContinue
      // is called, the pipeline resumes with an empty data frame with end_stream = true
      ASSERT(end_stream_);
      bufferedData() = createBuffer();
    }

    return false;
  }

  return true;
}

bool ActiveStreamFilterBase::commonHandleAfterTrailersCallback(FilterTrailersStatus status) {

  if (status == FilterTrailersStatus::Continue) {
    if (iteration_state_ == IterationState::StopSingleIteration) {
      commonContinue();
      return false;
    } else {
      ASSERT(headers_continued_);
    }
  } else {
    return false;
  }

  return true;
}

const Network::Connection* ActiveStreamFilterBase::connection() { return parent_.connection(); }

Event::Dispatcher& ActiveStreamFilterBase::dispatcher() {
  return parent_.stream_manager_.connection().dispatcher();
}

StreamInfo::StreamInfo& ActiveStreamFilterBase::streamInfo() { return parent_.stream_info_; }

Tracing::Span& ActiveStreamFilterBase::activeSpan() {
  if (parent_.active_span_) {
    return *parent_.active_span_;
  } else {
    return Tracing::NullSpan::instance();
  }
}

Tracing::Config& ActiveStreamFilterBase::tracingConfig() { return parent_; }

Upstream::ClusterInfoConstSharedPtr ActiveStreamFilterBase::clusterInfo() {
  // NOTE: Refreshing route caches clusterInfo as well.
  if (!parent_.cached_route_.has_value()) {
    parent_.refreshCachedRoute();
  }

  return parent_.cached_cluster_info_.value();
}

Router::RouteConstSharedPtr ActiveStreamFilterBase::route() {
  if (!parent_.cached_route_.has_value()) {
    parent_.refreshCachedRoute();
  }

  return parent_.cached_route_.value();
}

void ActiveStreamFilterBase::clearRouteCache() {
  parent_.cached_route_ = absl::optional<Router::RouteConstSharedPtr>();
  parent_.cached_cluster_info_ = absl::optional<Upstream::ClusterInfoConstSharedPtr>();
  if (parent_.tracing_custom_tags_) {
    parent_.tracing_custom_tags_->clear();
  }
}

void ActiveStreamFilterBase::resetStream() {
  parent_.connection_manager_stats_.named_.downstream_rq_tx_reset_.inc();
  parent_.stream_manager_.doEndStream(this->parent_);
}

uint64_t ActiveStreamFilterBase::streamId() { return parent_.stream_id_; }

} // namespace ConnectionManager
} // namespace Http
} // namespace Envoy
