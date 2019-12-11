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
#include "envoy/tracing/http_tracer.h"
#include "envoy/upstream/upstream.h"

#include "common/buffer/watermark_buffer.h"
#include "common/common/dump_state_utils.h"
#include "common/common/linked_object.h"
#include "common/grpc/common.h"
#include "common/http/conn_manager/active_stream_filter_base.h"
#include "common/http/conn_manager_config.h"
#include "common/http/user_agent.h"
#include "common/http/utility.h"
#include "common/stream_info/stream_info_impl.h"

namespace Envoy {
namespace Http {
namespace ConnectionManager {

/**
 * Wrapper for a stream decoder filter.
 */
struct ActiveStreamDecoderFilter : public ActiveStreamFilterBase,
                                   public StreamDecoderFilterCallbacks,
                                   LinkedObject<ActiveStreamDecoderFilter> {
  ActiveStreamDecoderFilter(ActiveStream& parent, StreamDecoderFilterSharedPtr filter,
                            bool dual_filter)
      : ActiveStreamFilterBase(parent, dual_filter), handle_(filter) {}

  // ActiveStreamFilterBase
  bool canContinue() override {
    // It is possible for the connection manager to respond directly to a request even while
    // a filter is trying to continue. If a response has already happened, we should not
    // continue to further filters. A concrete example of this is a filter buffering data, the
    // last data frame comes in and the filter continues, but the final buffering takes the stream
    // over the high watermark such that a 413 is returned.
    return !parent_.state_.local_complete_;
  }
  Buffer::WatermarkBufferPtr createBuffer() override;
  Buffer::WatermarkBufferPtr& bufferedData() override { return parent_.buffered_request_data_; }
  bool complete() override { return parent_.state_.remote_complete_; }
  void do100ContinueHeaders() override { NOT_REACHED_GCOVR_EXCL_LINE; }
  void doHeaders(bool end_stream) override {
    parent_.decodeHeaders(this, *parent_.request_headers_, end_stream);
  }
  void doData(bool end_stream) override {
    parent_.decodeData(this, *parent_.buffered_request_data_, end_stream,
                       ActiveStream::FilterIterationStartState::CanStartFromCurrent);
  }
  void doMetadata() override {
    if (saved_request_metadata_ != nullptr) {
      drainSavedRequestMetadata();
    }
  }
  void doTrailers() override { parent_.decodeTrailers(this, *parent_.request_trailers_); }
  const HeaderMapPtr& trailers() override { return parent_.request_trailers_; }

  void drainSavedRequestMetadata() {
    ASSERT(saved_request_metadata_ != nullptr);
    for (auto& metadata_map : *getSavedRequestMetadata()) {
      parent_.decodeMetadata(this, *metadata_map);
    }
    getSavedRequestMetadata()->clear();
  }
  // This function is called after the filter calls decodeHeaders() to drain accumulated metadata.
  void handleMetadataAfterHeadersCallback() override;

  // Http::StreamDecoderFilterCallbacks
  void addDecodedData(Buffer::Instance& data, bool streaming) override;
  void injectDecodedDataToFilterChain(Buffer::Instance& data, bool end_stream) override;
  HeaderMap& addDecodedTrailers() override;
  MetadataMapVector& addDecodedMetadata() override;
  void continueDecoding() override;
  const Buffer::Instance* decodingBuffer() override { return parent_.buffered_request_data_.get(); }

  void modifyDecodingBuffer(std::function<void(Buffer::Instance&)> callback) override {
    // TODO: should this be a RELEASE_ASSERT, since it's the only guard on this modification?
    ASSERT(parent_.state_.latest_data_decoding_filter_ == this);
    callback(*parent_.buffered_request_data_.get());
  }

  void sendLocalReply(Code code, absl::string_view body,
                      std::function<void(HeaderMap& headers)> modify_headers,
                      const absl::optional<Grpc::Status::GrpcStatus> grpc_status,
                      absl::string_view details) override {
    parent_.stream_info_.setResponseCodeDetails(details);
    parent_.sendLocalReply(is_grpc_request_, code, body, modify_headers, parent_.is_head_request_,
                           grpc_status, details);
  }
  void encode100ContinueHeaders(HeaderMapPtr&& headers) override;
  void encodeHeaders(HeaderMapPtr&& headers, bool end_stream) override;
  void encodeData(Buffer::Instance& data, bool end_stream) override;
  void encodeTrailers(HeaderMapPtr&& trailers) override;
  void encodeMetadata(MetadataMapPtr&& metadata_map_ptr) override;
  void onDecoderFilterAboveWriteBufferHighWatermark() override;
  void onDecoderFilterBelowWriteBufferLowWatermark() override;
  void addDownstreamWatermarkCallbacks(DownstreamWatermarkCallbacks& watermark_callbacks) override;
  void
  removeDownstreamWatermarkCallbacks(DownstreamWatermarkCallbacks& watermark_callbacks) override;
  void setDecoderBufferLimit(uint32_t limit) override { parent_.setBufferLimit(limit); }
  uint32_t decoderBufferLimit() override { return parent_.buffer_limit_; }
  bool recreateStream() override;

  void addUpstreamSocketOptions(const Network::Socket::OptionsSharedPtr& options) override {
    Network::Socket::appendOptions(parent_.upstream_options_, options);
  }

  Network::Socket::OptionsSharedPtr getUpstreamSocketOptions() const override {
    return parent_.upstream_options_;
  }

  // Each decoder filter instance checks if the request passed to the filter is gRPC
  // so that we can issue gRPC local responses to gRPC requests. Filter's decodeHeaders()
  // called here may change the content type, so we must check it before the call.
  FilterHeadersStatus decodeHeaders(HeaderMap& headers, bool end_stream) {
    is_grpc_request_ = Grpc::Common::hasGrpcContentType(headers);
    FilterHeadersStatus status = handle_->decodeHeaders(headers, end_stream);
    if (end_stream) {
      handle_->decodeComplete();
    }
    return status;
  }

  void requestDataTooLarge();
  void requestDataDrained();

  StreamDecoderFilterSharedPtr handle_;
  bool is_grpc_request_{};
};

using ActiveStreamDecoderFilterPtr = std::unique_ptr<ActiveStreamDecoderFilter>;

} // namespace ConnectionManager
} // namespace Http
} // namespace Envoy
