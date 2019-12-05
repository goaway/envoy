#pragma once

#include "common/http/conn_manager/active_stream_filter_base.h"

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
