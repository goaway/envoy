#include "common/http/conn_manager/active_stream_decoder_filter.h"

namespace Envoy {
namespace Http {
namespace ConnectionManager {

Buffer::WatermarkBufferPtr ActiveStreamDecoderFilter::createBuffer() {
  auto buffer =
      std::make_unique<Buffer::WatermarkBuffer>([this]() -> void { this->requestDataDrained(); },
                                                [this]() -> void { this->requestDataTooLarge(); });
  buffer->setWatermarks(parent_.buffer_limit_);
  return buffer;
}

void ActiveStreamDecoderFilter::handleMetadataAfterHeadersCallback() {
  // If we drain accumulated metadata, the iteration must start with the current filter.
  const bool saved_state = iterate_from_current_filter_;
  iterate_from_current_filter_ = true;
  // If decodeHeaders() returns StopAllIteration, we should skip draining metadata, and wait
  // for doMetadata() to drain the metadata after iteration continues.
  if (!stoppedAll() && saved_request_metadata_ != nullptr && !getSavedRequestMetadata()->empty()) {
    drainSavedRequestMetadata();
  }
  // Restores the original value of iterate_from_current_filter_.
  iterate_from_current_filter_ = saved_state;
}

HeaderMap& ActiveStreamDecoderFilter::addDecodedTrailers() { return parent_.addDecodedTrailers(); }

void ActiveStreamDecoderFilter::addDecodedData(Buffer::Instance& data, bool streaming) {
  parent_.addDecodedData(*this, data, streaming);
}

MetadataMapVector& ActiveStreamDecoderFilter::addDecodedMetadata() {
  return parent_.addDecodedMetadata();
}

void ActiveStreamDecoderFilter::injectDecodedDataToFilterChain(Buffer::Instance& data,
                                                               bool end_stream) {
  parent_.decodeData(this, data, end_stream,
                     ActiveStream::FilterIterationStartState::CanStartFromCurrent);
}

void ActiveStreamDecoderFilter::continueDecoding() { commonContinue(); }

void ActiveStreamDecoderFilter::encode100ContinueHeaders(HeaderMapPtr&& headers) {
  // If Envoy is not configured to proxy 100-Continue responses, swallow the 100 Continue
  // here. This avoids the potential situation where Envoy strips Expect: 100-Continue and sends a
  // 100-Continue, then proxies a duplicate 100 Continue from upstream.
  if (parent_.connection_manager_config_.proxy100Continue()) {
    parent_.continue_headers_ = std::move(headers);
    parent_.encode100ContinueHeaders(nullptr, *parent_.continue_headers_);
  }
}

void ActiveStreamDecoderFilter::encodeHeaders(HeaderMapPtr&& headers, bool end_stream) {
  parent_.response_headers_ = std::move(headers);
  parent_.encodeHeaders(nullptr, *parent_.response_headers_, end_stream);
}

void ActiveStreamDecoderFilter::encodeData(Buffer::Instance& data, bool end_stream) {
  parent_.encodeData(nullptr, data, end_stream,
                     ActiveStream::FilterIterationStartState::CanStartFromCurrent);
}

void ActiveStreamDecoderFilter::encodeTrailers(HeaderMapPtr&& trailers) {
  parent_.response_trailers_ = std::move(trailers);
  parent_.encodeTrailers(nullptr, *parent_.response_trailers_);
}

void ActiveStreamDecoderFilter::encodeMetadata(MetadataMapPtr&& metadata_map_ptr) {
  parent_.encodeMetadata(nullptr, std::move(metadata_map_ptr));
}

void ActiveStreamDecoderFilter::onDecoderFilterAboveWriteBufferHighWatermark() {
  ENVOY_STREAM_LOG(debug, "Read-disabling downstream stream due to filter callbacks.", parent_);
  parent_.response_encoder_->getStream().readDisable(true);
  parent_.connection_manager_stats_.named_.downstream_flow_control_paused_reading_total_.inc();
}

void ActiveStreamDecoderFilter::requestDataTooLarge() {
  ENVOY_STREAM_LOG(debug, "request data too large watermark exceeded", parent_);
  if (parent_.state_.decoder_filters_streaming_) {
    onDecoderFilterAboveWriteBufferHighWatermark();
  } else {
    parent_.connection_manager_stats_.named_.downstream_rq_too_large_.inc();
    sendLocalReply(Code::PayloadTooLarge, CodeUtility::toString(Code::PayloadTooLarge), nullptr,
                   absl::nullopt, StreamInfo::ResponseCodeDetails::get().RequestPayloadTooLarge);
  }
}

void ActiveStreamDecoderFilter::requestDataDrained() {
  // If this is called it means the call to requestDataTooLarge() was a
  // streaming call, or a 413 would have been sent.
  onDecoderFilterBelowWriteBufferLowWatermark();
}

void ActiveStreamDecoderFilter::onDecoderFilterBelowWriteBufferLowWatermark() {
  ENVOY_STREAM_LOG(debug, "Read-enabling downstream stream due to filter callbacks.", parent_);
  parent_.response_encoder_->getStream().readDisable(false);
  parent_.connection_manager_stats_.named_.downstream_flow_control_resumed_reading_total_.inc();
}

void ActiveStreamDecoderFilter::addDownstreamWatermarkCallbacks(
    DownstreamWatermarkCallbacks& watermark_callbacks) {
  // This is called exactly once per upstream-stream, by the router filter. Therefore, we
  // expect the same callbacks to not be registered twice.
  ASSERT(std::find(parent_.watermark_callbacks_.begin(), parent_.watermark_callbacks_.end(),
                   &watermark_callbacks) == parent_.watermark_callbacks_.end());
  parent_.watermark_callbacks_.emplace(parent_.watermark_callbacks_.end(), &watermark_callbacks);
  for (uint32_t i = 0; i < parent_.high_watermark_count_; ++i) {
    watermark_callbacks.onAboveWriteBufferHighWatermark();
  }
}
void ActiveStreamDecoderFilter::removeDownstreamWatermarkCallbacks(
    DownstreamWatermarkCallbacks& watermark_callbacks) {
  ASSERT(std::find(parent_.watermark_callbacks_.begin(), parent_.watermark_callbacks_.end(),
                   &watermark_callbacks) != parent_.watermark_callbacks_.end());
  parent_.watermark_callbacks_.remove(&watermark_callbacks);
}

bool ActiveStreamDecoderFilter::recreateStream() {
  // Because the filter's and the HCM view of if the stream has a body and if
  // the stream is complete may differ, re-check bytesReceived() to make sure
  // there was no body from the HCM's point of view.
  if (!complete() || parent_.stream_info_->bytesReceived() != 0) {
    return false;
  }
  // n.b. we do not currently change the codecs to point at the new stream
  // decoder because the decoder callbacks are complete. It would be good to
  // null out that pointer but should not be necessary.
  HeaderMapPtr request_headers(std::move(parent_.request_headers_));
  StreamEncoder* response_encoder = parent_.response_encoder_;
  parent_.response_encoder_ = nullptr;
  response_encoder->getStream().removeCallbacks(parent_);
  // This functionally deletes the stream (via deferred delete) so do not
  // reference anything beyond this point.
  parent_.stream_manager_.doEndStream(this->parent_);

  StreamDecoder& new_stream = parent_.stream_manager_.newStream(*response_encoder, true);
  new_stream.decodeHeaders(std::move(request_headers), true);
  return true;
}

} // namespace ConnectionManager
} // namespace Http
} // namespace Envoy
