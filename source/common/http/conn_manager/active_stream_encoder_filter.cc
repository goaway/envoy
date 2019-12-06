#include "common/http/conn_manager/active_stream_encoder_filter.h"

#include "common/http/codes.h"

namespace Envoy {
namespace Http {
namespace ConnectionManager {

Buffer::WatermarkBufferPtr ActiveStreamEncoderFilter::createBuffer() {
  auto buffer = new Buffer::WatermarkBuffer([this]() -> void { this->responseDataDrained(); },
                                            [this]() -> void { this->responseDataTooLarge(); });
  buffer->setWatermarks(parent_.buffer_limit_);
  return Buffer::WatermarkBufferPtr{buffer};
}

void ActiveStreamEncoderFilter::handleMetadataAfterHeadersCallback() {
  // If we drain accumulated metadata, the iteration must start with the current filter.
  const bool saved_state = iterate_from_current_filter_;
  iterate_from_current_filter_ = true;
  // If encodeHeaders() returns StopAllIteration, we should skip draining metadata, and wait
  // for doMetadata() to drain the metadata after iteration continues.
  if (!stoppedAll() && saved_response_metadata_ != nullptr &&
      !getSavedResponseMetadata()->empty()) {
    drainSavedResponseMetadata();
  }
  // Restores the original value of iterate_from_current_filter_.
  iterate_from_current_filter_ = saved_state;
}
void ActiveStreamEncoderFilter::addEncodedData(Buffer::Instance& data, bool streaming) {
  return parent_.addEncodedData(*this, data, streaming);
}

void ActiveStreamEncoderFilter::injectEncodedDataToFilterChain(Buffer::Instance& data,
                                                               bool end_stream) {
  parent_.encodeData(this, data, end_stream,
                     ActiveStream::FilterIterationStartState::CanStartFromCurrent);
}

HeaderMap& ActiveStreamEncoderFilter::addEncodedTrailers() { return parent_.addEncodedTrailers(); }

void ActiveStreamEncoderFilter::addEncodedMetadata(MetadataMapPtr&& metadata_map_ptr) {
  return parent_.encodeMetadata(this, std::move(metadata_map_ptr));
}

void ActiveStreamEncoderFilter::onEncoderFilterAboveWriteBufferHighWatermark() {
  ENVOY_STREAM_LOG(debug, "Disabling upstream stream due to filter callbacks.", parent_);
  parent_.callHighWatermarkCallbacks();
}

void ActiveStreamEncoderFilter::onEncoderFilterBelowWriteBufferLowWatermark() {
  ENVOY_STREAM_LOG(debug, "Enabling upstream stream due to filter callbacks.", parent_);
  parent_.callLowWatermarkCallbacks();
}

void ActiveStreamEncoderFilter::continueEncoding() { commonContinue(); }

void ActiveStreamEncoderFilter::responseDataTooLarge() {
  if (parent_.state_.encoder_filters_streaming_) {
    onEncoderFilterAboveWriteBufferHighWatermark();
  } else {
    parent_.connection_manager_stats_.named_.rs_too_large_.inc();

    // If headers have not been sent to the user, send a 500.
    if (!headers_continued_) {
      // Make sure we won't end up with nested watermark calls from the body buffer.
      parent_.state_.encoder_filters_streaming_ = true;
      allowIteration();

      parent_.stream_info_.setResponseCodeDetails(
          StreamInfo::ResponseCodeDetails::get().RequestHeadersTooLarge);
      Http::Utility::sendLocalReply(
          Grpc::Common::hasGrpcContentType(*parent_.request_headers_),
          [&](HeaderMapPtr&& response_headers, bool end_stream) -> void {
            parent_.chargeStats(*response_headers);
            parent_.response_headers_ = std::move(response_headers);
            parent_.response_encoder_->encodeHeaders(*parent_.response_headers_, end_stream);
            parent_.state_.local_complete_ = end_stream;
          },
          [&](Buffer::Instance& data, bool end_stream) -> void {
            parent_.response_encoder_->encodeData(data, end_stream);
            parent_.state_.local_complete_ = end_stream;
          },
          parent_.state_.destroyed_, Http::Code::InternalServerError,
          CodeUtility::toString(Http::Code::InternalServerError), absl::nullopt,
          parent_.is_head_request_);
      parent_.maybeEndEncode(parent_.state_.local_complete_);
    } else {
      ENVOY_STREAM_LOG(
          debug, "Resetting stream. Response data too large and headers have already been sent",
          *this);
      resetStream();
    }
  }
}

void ActiveStreamEncoderFilter::responseDataDrained() {
  onEncoderFilterBelowWriteBufferLowWatermark();
}

} // namespace ConnectionManager
} // namespace Http
} // namespace Envoy
