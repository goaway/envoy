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
#include "common/http/conn_manager/active_stream_filter_base.h"
#include "common/http/conn_manager_config.h"
#include "common/http/user_agent.h"
#include "common/http/utility.h"
#include "common/stream_info/stream_info_impl.h"

namespace Envoy {
namespace Http {
namespace ConnectionManager {

/**
 * Wrapper for a stream encoder filter.
 */
struct ActiveStreamEncoderFilter : public ActiveStreamFilterBase,
                                   public StreamEncoderFilterCallbacks,
                                   LinkedObject<ActiveStreamEncoderFilter> {
  ActiveStreamEncoderFilter(ActiveStream& parent, StreamEncoderFilterSharedPtr filter,
                            bool dual_filter)
      : ActiveStreamFilterBase(parent, dual_filter), handle_(filter) {}

  // ActiveStreamFilterBase
  bool canContinue() override { return true; }
  Buffer::WatermarkBufferPtr createBuffer() override;
  Buffer::WatermarkBufferPtr& bufferedData() override { return parent_.buffered_response_data_; }
  bool complete() override { return parent_.state_.local_complete_; }
  void do100ContinueHeaders() override {
    parent_.encode100ContinueHeaders(this, *parent_.continue_headers_);
  }
  void doHeaders(bool end_stream) override {
    parent_.encodeHeaders(this, *parent_.response_headers_, end_stream);
  }
  void doData(bool end_stream) override {
    parent_.encodeData(this, *parent_.buffered_response_data_, end_stream,
                       ActiveStream::FilterIterationStartState::CanStartFromCurrent);
  }
  void drainSavedResponseMetadata() {
    ASSERT(saved_response_metadata_ != nullptr);
    for (auto& metadata_map : *getSavedResponseMetadata()) {
      parent_.encodeMetadata(this, std::move(metadata_map));
    }
    getSavedResponseMetadata()->clear();
  }
  void handleMetadataAfterHeadersCallback() override;

  void doMetadata() override {
    if (saved_response_metadata_ != nullptr) {
      drainSavedResponseMetadata();
    }
  }
  void doTrailers() override { parent_.encodeTrailers(this, *parent_.response_trailers_); }
  const HeaderMapPtr& trailers() override { return parent_.response_trailers_; }

  // Http::StreamEncoderFilterCallbacks
  void addEncodedData(Buffer::Instance& data, bool streaming) override;
  void injectEncodedDataToFilterChain(Buffer::Instance& data, bool end_stream) override;
  HeaderMap& addEncodedTrailers() override;
  void addEncodedMetadata(MetadataMapPtr&& metadata_map) override;
  void onEncoderFilterAboveWriteBufferHighWatermark() override;
  void onEncoderFilterBelowWriteBufferLowWatermark() override;
  void setEncoderBufferLimit(uint32_t limit) override { parent_.setBufferLimit(limit); }
  uint32_t encoderBufferLimit() override { return parent_.buffer_limit_; }
  void continueEncoding() override;
  const Buffer::Instance* encodingBuffer() override {
    return parent_.buffered_response_data_.get();
  }
  void modifyEncodingBuffer(std::function<void(Buffer::Instance&)> callback) override {
    ASSERT(parent_.state_.latest_data_encoding_filter_ == this);
    callback(*parent_.buffered_response_data_.get());
  }

  void responseDataTooLarge();
  void responseDataDrained();

  StreamEncoderFilterSharedPtr handle_;
};

using ActiveStreamEncoderFilterPtr = std::unique_ptr<ActiveStreamEncoderFilter>;

} // namespace ConnectionManager
} // namespace Http
} // namespace Envoy
