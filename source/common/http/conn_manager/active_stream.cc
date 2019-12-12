#include "common/http/conn_manager/active_stream.h"

#include "envoy/common/time.h"
#include "envoy/event/dispatcher.h"
#include "envoy/network/drain_decision.h"
#include "envoy/router/router.h"
#include "envoy/ssl/connection.h"
#include "envoy/stats/scope.h"
#include "envoy/tracing/http_tracer.h"

#include "common/buffer/buffer_impl.h"
#include "common/common/assert.h"
#include "common/common/empty_string.h"
#include "common/common/scope_tracker.h"
#include "common/common/utility.h"
#include "common/http/codes.h"
#include "common/http/conn_manager/active_stream_decoder_filter.h"
#include "common/http/conn_manager/active_stream_encoder_filter.h"
#include "common/http/conn_manager_utility.h"
#include "common/http/exception.h"
#include "common/http/header_map_impl.h"
#include "common/http/headers.h"
#include "common/http/http1/codec_impl.h"
#include "common/http/http2/codec_impl.h"
#include "common/http/path_utility.h"
#include "common/http/utility.h"
#include "common/network/utility.h"
#include "common/router/config_impl.h"
#include "common/runtime/runtime_impl.h"
#include "common/stats/timespan_impl.h"
#include "common/tracing/http_tracer_impl.h"

#include "absl/strings/escaping.h"
#include "absl/strings/match.h"

namespace Envoy {
namespace Http {
namespace ConnectionManager {

namespace {

template <class T> using FilterList = std::list<std::unique_ptr<T>>;

// Shared helper for recording the latest filter used.
template <class T>
void recordLatestDataFilter(const typename FilterList<T>::iterator current_filter,
                            T*& latest_filter, const FilterList<T>& filters) {
  // If this is the first time we're calling onData, just record the current filter.
  if (latest_filter == nullptr) {
    latest_filter = current_filter->get();
    return;
  }

  // We want to keep this pointing at the latest filter in the filter list that has received the
  // onData callback. To do so, we compare the current latest with the *previous* filter. If they
  // match, then we must be processing a new filter for the first time. We omit this check if we're
  // the first filter, since the above check handles that case.
  //
  // We compare against the previous filter to avoid multiple filter iterations from resetting the
  // pointer: If we just set latest to current, then the first onData filter iteration would
  // correctly iterate over the filters and set latest, but on subsequent onData iterations
  // we'd start from the beginning again, potentially allowing filter N to modify the buffer even
  // though filter M > N was the filter that inserted data into the buffer.
  if (current_filter != filters.begin() && latest_filter == std::prev(current_filter)->get()) {
    latest_filter = current_filter->get();
  }
}

} // namespace

ActiveStream::ActiveStream(StreamInfo::StreamInfoImplPtr&& stream_info,
                           StreamManager& stream_manager,
                           ConnectionManagerInfo& connection_manager_info,
                           ConnectionManagerStats& connection_manager_stats,
                           ConnectionManagerListenerStats& listener_stats,
                           ConnectionManagerConfig& connection_manager_config)
    : stream_manager_(stream_manager), connection_manager_info_(connection_manager_info),
      connection_manager_stats_(connection_manager_stats), listener_stats_(listener_stats),
      connection_manager_config_(connection_manager_config),
      stream_id_(connection_manager_info_.randomGenerator().random()),
      request_response_timespan_(new Stats::HistogramCompletableTimespanImpl(
          connection_manager_stats_.named_.downstream_rq_time_,
          connection_manager_info_.timeSource())),
      stream_info_(std::move(stream_info)),
      upstream_options_(std::make_shared<Network::Socket::Options>()) {
  ASSERT(!connection_manager_config_.isRoutable() ||
             ((connection_manager_config_.routeConfigProvider() == nullptr &&
               connection_manager_config_.scopedRouteConfigProvider() != nullptr) ||
              (connection_manager_config_.routeConfigProvider() != nullptr &&
               connection_manager_config_.scopedRouteConfigProvider() == nullptr)),
         "Either routeConfigProvider or scopedRouteConfigProvider should be set in "
         "ConnectionManagerImpl.");

  ScopeTrackerScopeState scope(this, connection_manager_info_.dispatcher());

  connection_manager_stats_.named_.downstream_rq_total_.inc();
  connection_manager_stats_.named_.downstream_rq_active_.inc();
  if (connection_manager_info_.protocol() == Protocol::Http2) {
    connection_manager_stats_.named_.downstream_rq_http2_total_.inc();
  } else if (connection_manager_info_.protocol() == Protocol::Http3) {
    connection_manager_stats_.named_.downstream_rq_http3_total_.inc();
  } else {
    connection_manager_stats_.named_.downstream_rq_http1_total_.inc();
  }

  if (connection_manager_config_.streamIdleTimeout().count()) {
    idle_timeout_ms_ = connection_manager_config_.streamIdleTimeout();
    stream_idle_timer_ =
        connection_manager_info_.dispatcher().createTimer([this]() -> void { onIdleTimeout(); });
    resetIdleTimer();
  }

  if (connection_manager_config_.requestTimeout().count()) {
    std::chrono::milliseconds request_timeout_ms_ = connection_manager_config_.requestTimeout();
    request_timer_ =
        connection_manager_info_.dispatcher().createTimer([this]() -> void { onRequestTimeout(); });
    request_timer_->enableTimer(request_timeout_ms_, this);
  }
}

ActiveStream::~ActiveStream() {
  stream_info_->onRequestComplete();

  // A downstream disconnect can be identified for HTTP requests when the upstream returns with a 0
  // response code and when no other response flags are set.
  if (!stream_info_->hasAnyResponseFlag() && !stream_info_->responseCode()) {
    stream_info_->setResponseFlag(StreamInfo::ResponseFlag::DownstreamConnectionTermination);
  }

  connection_manager_stats_.named_.downstream_rq_active_.dec();
  // Refresh byte sizes of the HeaderMaps before logging.
  // TODO(asraa): Remove this when entries in HeaderMap can no longer be modified by reference and
  // HeaderMap holds an accurate internal byte size count.
  if (request_headers_ != nullptr) {
    request_headers_->refreshByteSize();
  }
  if (response_headers_ != nullptr) {
    response_headers_->refreshByteSize();
  }
  if (response_trailers_ != nullptr) {
    response_trailers_->refreshByteSize();
  }
  for (const AccessLog::InstanceSharedPtr& access_log : connection_manager_config_.accessLogs()) {
    access_log->log(request_headers_.get(), response_headers_.get(), response_trailers_.get(),
                    *stream_info_);
  }
  for (const auto& log_handler : access_log_handlers_) {
    log_handler->log(request_headers_.get(), response_headers_.get(), response_trailers_.get(),
                     *stream_info_);
  }

  if (stream_info_->healthCheck()) {
    connection_manager_config_.tracingStats().health_check_.inc();
  }

  if (active_span_) {
    Tracing::HttpTracerUtility::finalizeDownstreamSpan(
        *active_span_, request_headers_.get(), response_headers_.get(), response_trailers_.get(),
        *stream_info_, *this);
  }
  if (state_.successful_upgrade_) {
    connection_manager_stats_.named_.downstream_cx_upgrades_active_.dec();
  }

  ASSERT(state_.filter_call_state_ == 0);
}

void ActiveStream::resetIdleTimer() {
  if (stream_idle_timer_ != nullptr) {
    // TODO(htuch): If this shows up in performance profiles, optimize by only
    // updating a timestamp here and doing periodic checks for idle timeouts
    // instead, or reducing the accuracy of timers.
    stream_idle_timer_->enableTimer(idle_timeout_ms_);
  }
}

void ActiveStream::onIdleTimeout() {
  connection_manager_stats_.named_.downstream_rq_idle_timeout_.inc();
  // If headers have not been sent to the user, send a 408.
  if (response_headers_ != nullptr) {
    // TODO(htuch): We could send trailers here with an x-envoy timeout header
    // or gRPC status code, and/or set H2 RST_STREAM error.
    stream_manager_.doEndStream(*this);
  } else {
    stream_info_->setResponseFlag(StreamInfo::ResponseFlag::StreamIdleTimeout);
    sendLocalReply(request_headers_ != nullptr &&
                       Grpc::Common::hasGrpcContentType(*request_headers_),
                   Http::Code::RequestTimeout, "stream timeout", nullptr, is_head_request_,
                   absl::nullopt, StreamInfo::ResponseCodeDetails::get().StreamIdleTimeout);
  }
}

void ActiveStream::onRequestTimeout() {
  connection_manager_stats_.named_.downstream_rq_timeout_.inc();
  sendLocalReply(request_headers_ != nullptr && Grpc::Common::hasGrpcContentType(*request_headers_),
                 Http::Code::RequestTimeout, "request timeout", nullptr, is_head_request_,
                 absl::nullopt, StreamInfo::ResponseCodeDetails::get().RequestOverallTimeout);
}

void ActiveStream::addStreamDecoderFilterWorker(StreamDecoderFilterSharedPtr filter,
                                                bool dual_filter) {
  ActiveStreamDecoderFilterPtr wrapper(new ActiveStreamDecoderFilter(*this, filter, dual_filter));
  filter->setDecoderFilterCallbacks(*wrapper);
  wrapper->moveIntoListBack(std::move(wrapper), decoder_filters_);
}

void ActiveStream::addStreamEncoderFilterWorker(StreamEncoderFilterSharedPtr filter,
                                                bool dual_filter) {
  ActiveStreamEncoderFilterPtr wrapper(new ActiveStreamEncoderFilter(*this, filter, dual_filter));
  filter->setEncoderFilterCallbacks(*wrapper);
  wrapper->moveIntoList(std::move(wrapper), encoder_filters_);
}

void ActiveStream::addAccessLogHandler(AccessLog::InstanceSharedPtr handler) {
  access_log_handlers_.push_back(handler);
}

void ActiveStream::chargeStats(const HeaderMap& headers) {
  uint64_t response_code = Utility::getResponseStatus(headers);
  stream_info_->response_code_ = response_code;

  if (stream_info_->health_check_request_) {
    return;
  }

  connection_manager_stats_.named_.downstream_rq_completed_.inc();
  listener_stats_.downstream_rq_completed_.inc();
  if (CodeUtility::is1xx(response_code)) {
    connection_manager_stats_.named_.downstream_rq_1xx_.inc();
    listener_stats_.downstream_rq_1xx_.inc();
  } else if (CodeUtility::is2xx(response_code)) {
    connection_manager_stats_.named_.downstream_rq_2xx_.inc();
    listener_stats_.downstream_rq_2xx_.inc();
  } else if (CodeUtility::is3xx(response_code)) {
    connection_manager_stats_.named_.downstream_rq_3xx_.inc();
    listener_stats_.downstream_rq_3xx_.inc();
  } else if (CodeUtility::is4xx(response_code)) {
    connection_manager_stats_.named_.downstream_rq_4xx_.inc();
    listener_stats_.downstream_rq_4xx_.inc();
  } else if (CodeUtility::is5xx(response_code)) {
    connection_manager_stats_.named_.downstream_rq_5xx_.inc();
    listener_stats_.downstream_rq_5xx_.inc();
  }
}

const Network::Connection* ActiveStream::connection() {
  return &connection_manager_info_.connection();
}

// Ordering in this function is complicated, but important.
//
// We want to do minimal work before selecting route and creating a filter
// chain to maximize the number of requests which get custom filter behavior,
// e.g. registering access logging.
//
// This must be balanced by doing sanity checking for invalid requests (one
// can't route select properly without full headers), checking state required to
// serve error responses (connection close, head requests, etc), and
// modifications which may themselves affect route selection.
//
// TODO(alyssawilk) all the calls here should be audited for order priority,
// e.g. many early returns do not currently handle connection: close properly.
void ActiveStream::decodeHeaders(HeaderMapPtr&& headers, bool end_stream) {
  ScopeTrackerScopeState scope(this, connection_manager_info_.dispatcher());
  request_headers_ = std::move(headers);

  // We need to snap snapped_route_config_ here as it's used in mutateRequestHeaders later.
  if (connection_manager_config_.isRoutable()) {
    if (connection_manager_config_.routeConfigProvider() != nullptr) {
      snapped_route_config_ = connection_manager_config_.routeConfigProvider()->config();
    } else if (connection_manager_config_.scopedRouteConfigProvider() != nullptr) {
      snapped_scoped_routes_config_ =
          connection_manager_config_.scopedRouteConfigProvider()->config<Router::ScopedConfig>();
      snapScopedRouteConfig();
    }
  } else {
    snapped_route_config_ = connection_manager_config_.routeConfigProvider()->config();
  }

  if (Http::Headers::get().MethodValues.Head ==
      request_headers_->Method()->value().getStringView()) {
    is_head_request_ = true;
  }
  ENVOY_STREAM_LOG(debug, "request headers complete (end_stream={}):\n{}", *this, end_stream,
                   *request_headers_);

  // We end the decode here only if the request is header only. If we convert the request to a
  // header only, the stream will be marked as done once a subsequent decodeData/decodeTrailers is
  // called with end_stream=true.
  maybeEndDecode(end_stream);

  // Drop new requests when overloaded as soon as we have decoded the headers.
  if (stream_manager_.isOverloaded()) {
    // In this one special case, do not create the filter chain. If there is a risk of memory
    // overload it is more important to avoid unnecessary allocation than to create the filters.
    state_.created_filter_chain_ = true;
    connection_manager_stats_.named_.downstream_rq_overload_close_.inc();
    sendLocalReply(Grpc::Common::hasGrpcContentType(*request_headers_),
                   Http::Code::ServiceUnavailable, "envoy overloaded", nullptr, is_head_request_,
                   absl::nullopt, StreamInfo::ResponseCodeDetails::get().Overload);
    return;
  }

  if (!connection_manager_config_.proxy100Continue() && request_headers_->Expect() &&
      request_headers_->Expect()->value() == Headers::get().ExpectValues._100Continue.c_str()) {
    // Note in the case Envoy is handling 100-Continue complexity, it skips the filter chain
    // and sends the 100-Continue directly to the encoder.
    chargeStats(ConnectionManagerUtility::continueHeader());
    response_encoder_->encode100ContinueHeaders(ConnectionManagerUtility::continueHeader());
    // Remove the Expect header so it won't be handled again upstream.
    request_headers_->removeExpect();
  }

  stream_manager_.initializeUserAgentFromHeaders(*request_headers_);

  // Make sure we are getting a codec version we support.
  Protocol protocol = connection_manager_info_.protocol();
  if (protocol == Protocol::Http10) {
    // Assume this is HTTP/1.0. This is fine for HTTP/0.9 but this code will also affect any
    // requests with non-standard version numbers (0.9, 1.3), basically anything which is not
    // HTTP/1.1.
    //
    // The protocol may have shifted in the HTTP/1.0 case so reset it.
    stream_info_->protocol(protocol);
    if (!connection_manager_config_.http1Settings().accept_http_10_) {
      // Send "Upgrade Required" if HTTP/1.0 support is not explicitly configured on.
      sendLocalReply(false, Code::UpgradeRequired, "", nullptr, is_head_request_, absl::nullopt,
                     StreamInfo::ResponseCodeDetails::get().LowVersion);
      return;
    } else {
      // HTTP/1.0 defaults to single-use connections. Make sure the connection
      // will be closed unless Keep-Alive is present.
      state_.saw_connection_close_ = true;
      if (request_headers_->Connection() &&
          absl::EqualsIgnoreCase(request_headers_->Connection()->value().getStringView(),
                                 Http::Headers::get().ConnectionValues.KeepAlive)) {
        state_.saw_connection_close_ = false;
      }
    }
  }

  if (!request_headers_->Host()) {
    if ((protocol == Protocol::Http10) &&
        !connection_manager_config_.http1Settings().default_host_for_http_10_.empty()) {
      // Add a default host if configured to do so.
      request_headers_->setHost(
          connection_manager_config_.http1Settings().default_host_for_http_10_);
    } else {
      // Require host header. For HTTP/1.1 Host has already been translated to :authority.
      sendLocalReply(Grpc::Common::hasGrpcContentType(*request_headers_), Code::BadRequest, "",
                     nullptr, is_head_request_, absl::nullopt,
                     StreamInfo::ResponseCodeDetails::get().MissingHost);
      return;
    }
  }

  // Currently we only support relative paths at the application layer. We expect the codec to have
  // broken the path into pieces if applicable. NOTE: Currently the HTTP/1.1 codec only does this
  // when the allow_absolute_url flag is enabled on the HCM.
  // https://tools.ietf.org/html/rfc7230#section-5.3 We also need to check for the existence of
  // :path because CONNECT does not have a path, and we don't support that currently.
  if (!request_headers_->Path() || request_headers_->Path()->value().getStringView().empty() ||
      request_headers_->Path()->value().getStringView()[0] != '/') {
    const bool has_path =
        request_headers_->Path() && !request_headers_->Path()->value().getStringView().empty();
    connection_manager_stats_.named_.downstream_rq_non_relative_path_.inc();
    sendLocalReply(Grpc::Common::hasGrpcContentType(*request_headers_), Code::NotFound, "", nullptr,
                   is_head_request_, absl::nullopt,
                   has_path ? StreamInfo::ResponseCodeDetails::get().AbsolutePath
                            : StreamInfo::ResponseCodeDetails::get().MissingPath);
    return;
  }

  // Path sanitization should happen before any path access other than the above sanity check.
  if (!ConnectionManagerUtility::maybeNormalizePath(*request_headers_,
                                                    connection_manager_config_)) {
    sendLocalReply(Grpc::Common::hasGrpcContentType(*request_headers_), Code::BadRequest, "",
                   nullptr, is_head_request_, absl::nullopt,
                   StreamInfo::ResponseCodeDetails::get().PathNormalizationFailed);
    return;
  }

  if (protocol == Protocol::Http11 && request_headers_->Connection() &&
      absl::EqualsIgnoreCase(request_headers_->Connection()->value().getStringView(),
                             Http::Headers::get().ConnectionValues.Close)) {
    state_.saw_connection_close_ = true;
  }
  // Note: Proxy-Connection is not a standard header, but is supported here
  // since it is supported by http-parser the underlying parser for http
  // requests.
  if (protocol < Protocol::Http2 && !state_.saw_connection_close_ &&
      request_headers_->ProxyConnection() &&
      absl::EqualsIgnoreCase(request_headers_->ProxyConnection()->value().getStringView(),
                             Http::Headers::get().ConnectionValues.Close)) {
    state_.saw_connection_close_ = true;
  }

  if (!state_.is_internally_created_) { // Only sanitize headers on first pass.
    // Modify the downstream remote address depending on configuration and headers.
    stream_info_->setDownstreamRemoteAddress(ConnectionManagerUtility::mutateRequestHeaders(
        *request_headers_, connection_manager_info_.connection(), connection_manager_config_,
        *snapped_route_config_, connection_manager_info_.randomGenerator(),
        connection_manager_info_.localInfo()));
  }
  ASSERT(stream_info_->downstreamRemoteAddress() != nullptr);

  ASSERT(!cached_route_);
  refreshCachedRoute();

  if (!state_.is_internally_created_) { // Only mutate tracing headers on first pass.
    ConnectionManagerUtility::mutateTracingRequestHeader(
        *request_headers_, connection_manager_info_.runtime(), connection_manager_config_,
        cached_route_.value().get());
  }

  stream_info_->setRequestHeaders(*request_headers_);

  const bool upgrade_rejected = createFilterChain() == false;

  // TODO if there are no filters when starting a filter iteration, the connection manager
  // should return 404. The current returns no response if there is no router filter.
  if (protocol == Protocol::Http11 && hasCachedRoute()) {
    if (upgrade_rejected) {
      // Do not allow upgrades if the route does not support it.
      connection_manager_stats_.named_.downstream_rq_ws_on_non_ws_route_.inc();
      sendLocalReply(Grpc::Common::hasGrpcContentType(*request_headers_), Code::Forbidden, "",
                     nullptr, is_head_request_, absl::nullopt,
                     StreamInfo::ResponseCodeDetails::get().UpgradeFailed);
      return;
    }
    // Allow non websocket requests to go through websocket enabled routes.
  }

  if (hasCachedRoute()) {
    const Router::RouteEntry* route_entry = cached_route_.value()->routeEntry();
    if (route_entry != nullptr && route_entry->idleTimeout()) {
      idle_timeout_ms_ = route_entry->idleTimeout().value();
      if (idle_timeout_ms_.count()) {
        // If we have a route-level idle timeout but no global stream idle timeout, create a timer.
        if (stream_idle_timer_ == nullptr) {
          stream_idle_timer_ = connection_manager_info_.dispatcher().createTimer(
              [this]() -> void { onIdleTimeout(); });
        }
      } else if (stream_idle_timer_ != nullptr) {
        // If we had a global stream idle timeout but the route-level idle timeout is set to zero
        // (to override), we disable the idle timer.
        stream_idle_timer_->disableTimer();
        stream_idle_timer_ = nullptr;
      }
    }
  }

  // Check if tracing is enabled at all.
  if (connection_manager_config_.tracingConfig()) {
    traceRequest();
  }

  decodeHeaders(nullptr, *request_headers_, end_stream);

  // Reset it here for both global and overridden cases.
  resetIdleTimer();
}

void ActiveStream::traceRequest() {
  Tracing::Decision tracing_decision =
      Tracing::HttpTracerUtility::isTracing(*stream_info_, *request_headers_);
  ConnectionManagerUtility::chargeTracingStats(tracing_decision.reason,
                                               connection_manager_config_.tracingStats());

  active_span_ = connection_manager_info_.tracer().startSpan(*this, *request_headers_,
                                                             *stream_info_, tracing_decision);

  if (!active_span_) {
    return;
  }

  // TODO: Need to investigate the following code based on the cached route, as may
  // be broken in the case a filter changes the route.

  // If a decorator has been defined, apply it to the active span.
  if (hasCachedRoute() && cached_route_.value()->decorator()) {
    cached_route_.value()->decorator()->apply(*active_span_);

    // Cache decorated operation.
    if (!cached_route_.value()->decorator()->getOperation().empty()) {
      decorated_operation_ = &cached_route_.value()->decorator()->getOperation();
    }
  }

  if (connection_manager_config_.tracingConfig()->operation_name_ ==
      Tracing::OperationName::Egress) {
    // For egress (outbound) requests, pass the decorator's operation name (if defined)
    // as a request header to enable the receiving service to use it in its server span.
    if (decorated_operation_) {
      request_headers_->setEnvoyDecoratorOperation(*decorated_operation_);
    }
  } else {
    const HeaderEntry* req_operation_override = request_headers_->EnvoyDecoratorOperation();

    // For ingress (inbound) requests, if a decorator operation name has been provided, it
    // should be used to override the active span's operation.
    if (req_operation_override) {
      if (!req_operation_override->value().empty()) {
        active_span_->setOperation(req_operation_override->value().getStringView());

        // Clear the decorated operation so won't be used in the response header, as
        // it has been overridden by the inbound decorator operation request header.
        decorated_operation_ = nullptr;
      }
      // Remove header so not propagated to service
      request_headers_->removeEnvoyDecoratorOperation();
    }
  }
}

void ActiveStream::decodeHeaders(ActiveStreamDecoderFilter* filter, HeaderMap& headers,
                                 bool end_stream) {
  // Headers filter iteration should always start with the next filter if available.
  std::list<ActiveStreamDecoderFilterPtr>::iterator entry =
      commonDecodePrefix(filter, FilterIterationStartState::AlwaysStartFromNext);
  std::list<ActiveStreamDecoderFilterPtr>::iterator continue_data_entry = decoder_filters_.end();

  for (; entry != decoder_filters_.end(); entry++) {
    ASSERT(!(state_.filter_call_state_ & FilterCallState::DecodeHeaders));
    state_.filter_call_state_ |= FilterCallState::DecodeHeaders;
    (*entry)->end_stream_ =
        decoding_headers_only_ || (end_stream && continue_data_entry == decoder_filters_.end());
    FilterHeadersStatus status = (*entry)->decodeHeaders(headers, (*entry)->end_stream_);

    ASSERT(!(status == FilterHeadersStatus::ContinueAndEndStream && (*entry)->end_stream_));
    state_.filter_call_state_ &= ~FilterCallState::DecodeHeaders;
    ENVOY_STREAM_LOG(trace, "decode headers called: filter={} status={}", *this,
                     static_cast<const void*>((*entry).get()), static_cast<uint64_t>(status));

    const bool new_metadata_added = processNewlyAddedMetadata();
    // If end_stream is set in headers, and a filter adds new metadata, we need to delay end_stream
    // in headers by inserting an empty data frame with end_stream set. The empty data frame is sent
    // after the new metadata.
    if ((*entry)->end_stream_ && new_metadata_added && !buffered_request_data_) {
      Buffer::OwnedImpl empty_data("");
      ENVOY_STREAM_LOG(
          trace, "inserting an empty data frame for end_stream due metadata being added.", *this);
      // Metadata frame doesn't carry end of stream bit. We need an empty data frame to end the
      // stream.
      addDecodedData(*((*entry).get()), empty_data, true);
    }

    (*entry)->decode_headers_called_ = true;
    if (!(*entry)->commonHandleAfterHeadersCallback(status, decoding_headers_only_) &&
        std::next(entry) != decoder_filters_.end()) {
      // Stop iteration IFF this is not the last filter. If it is the last filter, continue with
      // processing since we need to handle the case where a terminal filter wants to buffer, but
      // a previous filter has added body.
      return;
    }

    // Here we handle the case where we have a header only request, but a filter adds a body
    // to it. We need to not raise end_stream = true to further filters during inline iteration.
    if (end_stream && buffered_request_data_ && continue_data_entry == decoder_filters_.end()) {
      continue_data_entry = entry;
    }
  }

  if (continue_data_entry != decoder_filters_.end()) {
    // We use the continueDecoding() code since it will correctly handle not calling
    // decodeHeaders() again. Fake setting StopSingleIteration since the continueDecoding() code
    // expects it.
    ASSERT(buffered_request_data_);
    (*continue_data_entry)->iteration_state_ =
        ActiveStreamFilterBase::IterationState::StopSingleIteration;
    (*continue_data_entry)->continueDecoding();
  }

  if (end_stream) {
    disarmRequestTimeout();
  }
}

void ActiveStream::decodeData(Buffer::Instance& data, bool end_stream) {
  ScopeTrackerScopeState scope(this, connection_manager_info_.dispatcher());
  maybeEndDecode(end_stream);
  stream_info_->addBytesReceived(data.length());

  decodeData(nullptr, data, end_stream, FilterIterationStartState::CanStartFromCurrent);
}

void ActiveStream::decodeData(ActiveStreamDecoderFilter* filter, Buffer::Instance& data,
                              bool end_stream,
                              FilterIterationStartState filter_iteration_start_state) {
  ScopeTrackerScopeState scope(this, connection_manager_info_.dispatcher());
  resetIdleTimer();

  // If we previously decided to decode only the headers, do nothing here.
  if (decoding_headers_only_) {
    return;
  }

  // If a response is complete or a reset has been sent, filters do not care about further body
  // data. Just drop it.
  if (state_.local_complete_) {
    return;
  }

  auto trailers_added_entry = decoder_filters_.end();
  const bool trailers_exists_at_start = request_trailers_ != nullptr;
  // Filter iteration may start at the current filter.
  std::list<ActiveStreamDecoderFilterPtr>::iterator entry =
      commonDecodePrefix(filter, filter_iteration_start_state);

  for (; entry != decoder_filters_.end(); entry++) {
    // If the filter pointed by entry has stopped for all frame types, return now.
    if (handleDataIfStopAll(**entry, data, state_.decoder_filters_streaming_)) {
      return;
    }
    // If end_stream_ is marked for a filter, the data is not for this filter and filters after.
    //
    // In following case, ActiveStreamFilterBase::commonContinue() could be called recursively and
    // its doData() is called with wrong data.
    //
    //  There are 3 decode filters and "wrapper" refers to ActiveStreamFilter object.
    //
    //  filter0->decodeHeaders(_, true)
    //    return STOP
    //  filter0->continueDecoding()
    //    wrapper0->commonContinue()
    //      wrapper0->decodeHeaders(_, _, true)
    //        filter1->decodeHeaders(_, true)
    //          filter1->addDecodeData()
    //          return CONTINUE
    //        filter2->decodeHeaders(_, false)
    //          return CONTINUE
    //        wrapper1->commonContinue() // Detects data is added.
    //          wrapper1->doData()
    //            wrapper1->decodeData()
    //              filter2->decodeData(_, true)
    //                 return CONTINUE
    //      wrapper0->doData() // This should not be called
    //        wrapper0->decodeData()
    //          filter1->decodeData(_, true)  // It will cause assertions.
    //
    // One way to solve this problem is to mark end_stream_ for each filter.
    // If a filter is already marked as end_stream_ when decodeData() is called, bails out the
    // whole function. If just skip the filter, the codes after the loop will be called with
    // wrong data. For encodeData, the response_encoder->encode() will be called.
    if ((*entry)->end_stream_) {
      return;
    }
    ASSERT(!(state_.filter_call_state_ & FilterCallState::DecodeData));

    // We check the request_trailers_ pointer here in case addDecodedTrailers
    // is called in decodeData during a previous filter invocation, at which point we communicate to
    // the current and future filters that the stream has not yet ended.
    if (end_stream) {
      state_.filter_call_state_ |= FilterCallState::LastDataFrame;
    }

    recordLatestDataFilter(entry, state_.latest_data_decoding_filter_, decoder_filters_);

    state_.filter_call_state_ |= FilterCallState::DecodeData;
    (*entry)->end_stream_ = end_stream && !request_trailers_;
    FilterDataStatus status = (*entry)->handle_->decodeData(data, (*entry)->end_stream_);
    if ((*entry)->end_stream_) {
      (*entry)->handle_->decodeComplete();
    }
    state_.filter_call_state_ &= ~FilterCallState::DecodeData;
    if (end_stream) {
      state_.filter_call_state_ &= ~FilterCallState::LastDataFrame;
    }
    ENVOY_STREAM_LOG(trace, "decode data called: filter={} status={}", *this,
                     static_cast<const void*>((*entry).get()), static_cast<uint64_t>(status));

    processNewlyAddedMetadata();

    if (!trailers_exists_at_start && request_trailers_ &&
        trailers_added_entry == decoder_filters_.end()) {
      trailers_added_entry = entry;
    }

    if (!(*entry)->commonHandleAfterDataCallback(status, data, state_.decoder_filters_streaming_) &&
        std::next(entry) != decoder_filters_.end()) {
      // Stop iteration IFF this is not the last filter. If it is the last filter, continue with
      // processing since we need to handle the case where a terminal filter wants to buffer, but
      // a previous filter has added trailers.
      return;
    }
  }

  // If trailers were adding during decodeData we need to trigger decodeTrailers in order
  // to allow filters to process the trailers.
  if (trailers_added_entry != decoder_filters_.end()) {
    decodeTrailers(trailers_added_entry->get(), *request_trailers_);
  }

  if (end_stream) {
    disarmRequestTimeout();
  }
}

HeaderMap& ActiveStream::addDecodedTrailers() {
  // Trailers can only be added during the last data frame (i.e. end_stream = true).
  ASSERT(state_.filter_call_state_ & FilterCallState::LastDataFrame);

  // Trailers can only be added once.
  ASSERT(!request_trailers_);

  request_trailers_ = std::make_unique<HeaderMapImpl>();
  return *request_trailers_;
}

void ActiveStream::addDecodedData(ActiveStreamDecoderFilter& filter, Buffer::Instance& data,
                                  bool streaming) {
  if (state_.filter_call_state_ == 0 ||
      (state_.filter_call_state_ & FilterCallState::DecodeHeaders) ||
      (state_.filter_call_state_ & FilterCallState::DecodeData) ||
      ((state_.filter_call_state_ & FilterCallState::DecodeTrailers) && !filter.canIterate())) {
    // Make sure if this triggers watermarks, the correct action is taken.
    state_.decoder_filters_streaming_ = streaming;
    // If no call is happening or we are in the decode headers/data callback, buffer the data.
    // Inline processing happens in the decodeHeaders() callback if necessary.
    filter.commonHandleBufferData(data);
  } else if (state_.filter_call_state_ & FilterCallState::DecodeTrailers) {
    // In this case we need to inline dispatch the data to further filters. If those filters
    // choose to buffer/stop iteration that's fine.
    decodeData(&filter, data, false, FilterIterationStartState::AlwaysStartFromNext);
  } else {
    // TODO(mattklein123): Formalize error handling for filters and add tests. Should probably
    // throw an exception here.
    NOT_IMPLEMENTED_GCOVR_EXCL_LINE;
  }
}

MetadataMapVector& ActiveStream::addDecodedMetadata() { return *getRequestMetadataMapVector(); }

void ActiveStream::decodeTrailers(HeaderMapPtr&& trailers) {
  ScopeTrackerScopeState scope(this, connection_manager_info_.dispatcher());
  resetIdleTimer();
  maybeEndDecode(true);
  request_trailers_ = std::move(trailers);
  decodeTrailers(nullptr, *request_trailers_);
}

void ActiveStream::decodeTrailers(ActiveStreamDecoderFilter* filter, HeaderMap& trailers) {
  // If we previously decided to decode only the headers, do nothing here.
  if (decoding_headers_only_) {
    return;
  }

  // See decodeData() above for why we check local_complete_ here.
  if (state_.local_complete_) {
    return;
  }

  // Filter iteration may start at the current filter.
  std::list<ActiveStreamDecoderFilterPtr>::iterator entry =
      commonDecodePrefix(filter, FilterIterationStartState::CanStartFromCurrent);

  for (; entry != decoder_filters_.end(); entry++) {
    // If the filter pointed by entry has stopped for all frame type, return now.
    if ((*entry)->stoppedAll()) {
      return;
    }

    ASSERT(!(state_.filter_call_state_ & FilterCallState::DecodeTrailers));
    state_.filter_call_state_ |= FilterCallState::DecodeTrailers;
    FilterTrailersStatus status = (*entry)->handle_->decodeTrailers(trailers);
    (*entry)->handle_->decodeComplete();
    (*entry)->end_stream_ = true;
    state_.filter_call_state_ &= ~FilterCallState::DecodeTrailers;
    ENVOY_STREAM_LOG(trace, "decode trailers called: filter={} status={}", *this,
                     static_cast<const void*>((*entry).get()), static_cast<uint64_t>(status));

    processNewlyAddedMetadata();

    if (!(*entry)->commonHandleAfterTrailersCallback(status)) {
      return;
    }
  }
  disarmRequestTimeout();
}

void ActiveStream::decodeMetadata(MetadataMapPtr&& metadata_map) {
  resetIdleTimer();
  // After going through filters, the ownership of metadata_map will be passed to terminal filter.
  // The terminal filter may encode metadata_map to the next hop immediately or store metadata_map
  // and encode later when connection pool is ready.
  decodeMetadata(nullptr, *metadata_map);
}

void ActiveStream::decodeMetadata(ActiveStreamDecoderFilter* filter, MetadataMap& metadata_map) {
  // Filter iteration may start at the current filter.
  std::list<ActiveStreamDecoderFilterPtr>::iterator entry =
      commonDecodePrefix(filter, FilterIterationStartState::CanStartFromCurrent);

  for (; entry != decoder_filters_.end(); entry++) {
    // If the filter pointed by entry has stopped for all frame type, stores metadata and returns.
    // If the filter pointed by entry hasn't returned from decodeHeaders, stores newly added
    // metadata in case decodeHeaders returns StopAllIteration. The latter can happen when headers
    // callbacks generate new metadata.
    if (!(*entry)->decode_headers_called_ || (*entry)->stoppedAll()) {
      Http::MetadataMapPtr metadata_map_ptr = std::make_unique<Http::MetadataMap>(metadata_map);
      (*entry)->getSavedRequestMetadata()->emplace_back(std::move(metadata_map_ptr));
      return;
    }

    FilterMetadataStatus status = (*entry)->handle_->decodeMetadata(metadata_map);
    ENVOY_STREAM_LOG(trace, "decode metadata called: filter={} status={}, metadata: {}", *this,
                     static_cast<const void*>((*entry).get()), static_cast<uint64_t>(status),
                     metadata_map);
  }
}

void ActiveStream::maybeEndDecode(bool end_stream) {
  ASSERT(!state_.remote_complete_);
  state_.remote_complete_ = end_stream;
  if (end_stream) {
    stream_info_->onLastDownstreamRxByteReceived();
    ENVOY_STREAM_LOG(debug, "request end stream", *this);
  }
}

void ActiveStream::disarmRequestTimeout() {
  if (request_timer_) {
    request_timer_->disableTimer();
  }
}

std::list<ActiveStreamEncoderFilterPtr>::iterator
ActiveStream::commonEncodePrefix(ActiveStreamEncoderFilter* filter, bool end_stream,
                                 FilterIterationStartState filter_iteration_start_state) {
  // Only do base state setting on the initial call. Subsequent calls for filtering do not touch
  // the base state.
  if (filter == nullptr) {
    ASSERT(!state_.local_complete_);
    state_.local_complete_ = end_stream;
    return encoder_filters_.begin();
  }

  if (filter_iteration_start_state == FilterIterationStartState::CanStartFromCurrent &&
      (*(filter->entry()))->iterate_from_current_filter_) {
    // The filter iteration has been stopped for all frame types, and now the iteration continues.
    // The current filter's encoding callback has not be called. Call it now.
    return filter->entry();
  }
  return std::next(filter->entry());
}

std::list<ActiveStreamDecoderFilterPtr>::iterator
ActiveStream::commonDecodePrefix(ActiveStreamDecoderFilter* filter,
                                 FilterIterationStartState filter_iteration_start_state) {
  if (!filter) {
    return decoder_filters_.begin();
  }
  if (filter_iteration_start_state == FilterIterationStartState::CanStartFromCurrent &&
      (*(filter->entry()))->iterate_from_current_filter_) {
    // The filter iteration has been stopped for all frame types, and now the iteration continues.
    // The current filter's callback function has not been called. Call it now.
    return filter->entry();
  }
  return std::next(filter->entry());
}

void ActiveStream::snapScopedRouteConfig() {
  ASSERT(request_headers_ != nullptr,
         "Try to snap scoped route config when there is no request headers.");

  // NOTE: if a RDS subscription hasn't got a RouteConfiguration back, a Router::NullConfigImpl is
  // returned, in that case we let it pass.
  snapped_route_config_ = snapped_scoped_routes_config_->getRouteConfig(*request_headers_);
  if (snapped_route_config_ == nullptr) {
    ENVOY_STREAM_LOG(trace, "can't find SRDS scope.", *this);
    // TODO(stevenzzzz): Consider to pass an error message to router filter, so that it can
    // send back 404 with some more details.
    snapped_route_config_ = std::make_shared<Router::NullConfigImpl>();
  }
}

void ActiveStream::refreshCachedRoute() {
  Router::RouteConstSharedPtr route;
  if (request_headers_ != nullptr) {
    if (connection_manager_config_.isRoutable() &&
        connection_manager_config_.scopedRouteConfigProvider() != nullptr) {
      // NOTE: re-select scope as well in case the scope key header has been changed by a filter.
      snapScopedRouteConfig();
    }
    if (snapped_route_config_ != nullptr) {
      route = snapped_route_config_->route(*request_headers_, *stream_info_, stream_id_);
    }
  }
  stream_info_->route_entry_ = route ? route->routeEntry() : nullptr;
  cached_route_ = std::move(route);
  if (nullptr == stream_info_->route_entry_) {
    cached_cluster_info_ = nullptr;
  } else {
    Upstream::ThreadLocalCluster* local_cluster =
        connection_manager_info_.clusterManager().get(stream_info_->route_entry_->clusterName());
    cached_cluster_info_ = (nullptr == local_cluster) ? nullptr : local_cluster->info();
  }

  refreshCachedTracingCustomTags();
}

void ActiveStream::refreshCachedTracingCustomTags() {
  if (!connection_manager_config_.tracingConfig()) {
    return;
  }
  const Tracing::CustomTagMap& conn_manager_tags =
      connection_manager_config_.tracingConfig()->custom_tags_;
  const Tracing::CustomTagMap* route_tags = nullptr;
  if (hasCachedRoute() && cached_route_.value()->tracingConfig()) {
    route_tags = &cached_route_.value()->tracingConfig()->getCustomTags();
  }
  const bool configured_in_conn = !conn_manager_tags.empty();
  const bool configured_in_route = route_tags && !route_tags->empty();
  if (!configured_in_conn && !configured_in_route) {
    return;
  }
  Tracing::CustomTagMap& custom_tag_map = getOrMakeTracingCustomTagMap();
  if (configured_in_route) {
    custom_tag_map.insert(route_tags->begin(), route_tags->end());
  }
  if (configured_in_conn) {
    custom_tag_map.insert(conn_manager_tags.begin(), conn_manager_tags.end());
  }
}

void ActiveStream::sendLocalReply(bool is_grpc_request, Code code, absl::string_view body,
                                  const std::function<void(HeaderMap& headers)>& modify_headers,
                                  bool is_head_request,
                                  const absl::optional<Grpc::Status::GrpcStatus> grpc_status,
                                  absl::string_view details) {
  ENVOY_STREAM_LOG(debug, "Sending local reply with details {}", *this, details);
  ASSERT(response_headers_ == nullptr);
  // For early error handling, do a best-effort attempt to create a filter chain
  // to ensure access logging.
  if (!state_.created_filter_chain_) {
    createFilterChain();
  }
  stream_info_->setResponseCodeDetails(details);
  Utility::sendLocalReply(
      is_grpc_request,
      [this, modify_headers](HeaderMapPtr&& headers, bool end_stream) -> void {
        if (modify_headers != nullptr) {
          modify_headers(*headers);
        }
        response_headers_ = std::move(headers);
        // TODO: Start encoding from the last decoder filter that saw the
        // request instead.
        encodeHeaders(nullptr, *response_headers_, end_stream);
      },
      [this](Buffer::Instance& data, bool end_stream) -> void {
        // TODO: Start encoding from the last decoder filter that saw the
        // request instead.
        encodeData(nullptr, data, end_stream, FilterIterationStartState::CanStartFromCurrent);
      },
      state_.destroyed_, code, body, grpc_status, is_head_request);
}

void ActiveStream::encode100ContinueHeaders(ActiveStreamEncoderFilter* filter, HeaderMap& headers) {
  resetIdleTimer();
  ASSERT(connection_manager_config_.proxy100Continue());
  // Make sure commonContinue continues encode100ContinueHeaders.
  has_continue_headers_ = true;

  // Similar to the block in encodeHeaders, run encode100ContinueHeaders on each
  // filter. This is simpler than that case because 100 continue implies no
  // end-stream, and because there are normal headers coming there's no need for
  // complex continuation logic.
  // 100-continue filter iteration should always start with the next filter if available.
  std::list<ActiveStreamEncoderFilterPtr>::iterator entry =
      commonEncodePrefix(filter, false, FilterIterationStartState::AlwaysStartFromNext);
  for (; entry != encoder_filters_.end(); entry++) {
    ASSERT(!(state_.filter_call_state_ & FilterCallState::Encode100ContinueHeaders));
    state_.filter_call_state_ |= FilterCallState::Encode100ContinueHeaders;
    FilterHeadersStatus status = (*entry)->handle_->encode100ContinueHeaders(headers);
    state_.filter_call_state_ &= ~FilterCallState::Encode100ContinueHeaders;
    ENVOY_STREAM_LOG(trace, "encode 100 continue headers called: filter={} status={}", *this,
                     static_cast<const void*>((*entry).get()), static_cast<uint64_t>(status));
    if (!(*entry)->commonHandleAfter100ContinueHeadersCallback(status)) {
      return;
    }
  }

  // Strip the T-E headers etc. Defer other header additions as well as drain-close logic to the
  // continuation headers.
  ConnectionManagerUtility::mutateResponseHeaders(headers, request_headers_.get(), EMPTY_STRING);

  // Count both the 1xx and follow-up response code in stats.
  chargeStats(headers);

  ENVOY_STREAM_LOG(debug, "encoding 100 continue headers via codec:\n{}", *this, headers);

  // Now actually encode via the codec.
  response_encoder_->encode100ContinueHeaders(headers);
}

void ActiveStream::encodeHeaders(ActiveStreamEncoderFilter* filter, HeaderMap& headers,
                                 bool end_stream) {
  resetIdleTimer();
  disarmRequestTimeout();

  // Headers filter iteration should always start with the next filter if available.
  std::list<ActiveStreamEncoderFilterPtr>::iterator entry =
      commonEncodePrefix(filter, end_stream, FilterIterationStartState::AlwaysStartFromNext);
  std::list<ActiveStreamEncoderFilterPtr>::iterator continue_data_entry = encoder_filters_.end();

  for (; entry != encoder_filters_.end(); entry++) {
    ASSERT(!(state_.filter_call_state_ & FilterCallState::EncodeHeaders));
    state_.filter_call_state_ |= FilterCallState::EncodeHeaders;
    (*entry)->end_stream_ =
        encoding_headers_only_ || (end_stream && continue_data_entry == encoder_filters_.end());
    FilterHeadersStatus status = (*entry)->handle_->encodeHeaders(headers, (*entry)->end_stream_);
    if ((*entry)->end_stream_) {
      (*entry)->handle_->encodeComplete();
    }
    state_.filter_call_state_ &= ~FilterCallState::EncodeHeaders;
    ENVOY_STREAM_LOG(trace, "encode headers called: filter={} status={}", *this,
                     static_cast<const void*>((*entry).get()), static_cast<uint64_t>(status));

    (*entry)->encode_headers_called_ = true;
    const auto continue_iteration =
        (*entry)->commonHandleAfterHeadersCallback(status, encoding_headers_only_);

    // If we're encoding a headers only response, then mark the local as complete. This ensures
    // that we don't attempt to reset the downstream request in doEndStream.
    if (encoding_headers_only_) {
      state_.local_complete_ = true;
    }

    if (!continue_iteration) {
      return;
    }

    // Here we handle the case where we have a header only response, but a filter adds a body
    // to it. We need to not raise end_stream = true to further filters during inline iteration.
    if (end_stream && buffered_response_data_ && continue_data_entry == encoder_filters_.end()) {
      continue_data_entry = entry;
    }
  }

  // Base headers.
  connection_manager_config_.dateProvider().setDateHeader(headers);
  // Following setReference() is safe because serverName() is constant for the life of the listener.
  const auto transformation = connection_manager_config_.serverHeaderTransformation();
  if (transformation == ConnectionManagerConfig::HttpConnectionManagerProto::OVERWRITE ||
      (transformation == ConnectionManagerConfig::HttpConnectionManagerProto::APPEND_IF_ABSENT &&
       headers.Server() == nullptr)) {
    headers.setReferenceServer(connection_manager_config_.serverName());
  }
  ConnectionManagerUtility::mutateResponseHeaders(headers, request_headers_.get(),
                                                  connection_manager_config_.via());

  // NOTE: BIG CHUNK REMOVED
  if (stream_manager_.updateDrainState(*this)) {
    // If the connection manager is draining send "Connection: Close" on HTTP/1.1 connections.
    // Do not do this for H2 (which drains via GOAWAY) or Upgrade (as the upgrade
    // payload is no longer HTTP/1.1)
    if (connection_manager_info_.protocol() < Protocol::Http2 && !Utility::isUpgrade(headers)) {
      headers.setReferenceConnection(Headers::get().ConnectionValues.Close);
    }
  }

  if (connection_manager_config_.tracingConfig()) {
    if (connection_manager_config_.tracingConfig()->operation_name_ ==
        Tracing::OperationName::Ingress) {
      // For ingress (inbound) responses, if the request headers do not include a
      // decorator operation (override), then pass the decorator's operation name (if defined)
      // as a response header to enable the client service to use it in its client span.
      if (decorated_operation_) {
        headers.setEnvoyDecoratorOperation(*decorated_operation_);
      }
    } else if (connection_manager_config_.tracingConfig()->operation_name_ ==
               Tracing::OperationName::Egress) {
      const HeaderEntry* resp_operation_override = headers.EnvoyDecoratorOperation();

      // For Egress (outbound) response, if a decorator operation name has been provided, it
      // should be used to override the active span's operation.
      if (resp_operation_override) {
        if (!resp_operation_override->value().empty() && active_span_) {
          active_span_->setOperation(resp_operation_override->value().getStringView());
        }
        // Remove header so not propagated to service.
        headers.removeEnvoyDecoratorOperation();
      }
    }
  }

  chargeStats(headers);

  ENVOY_STREAM_LOG(debug, "encoding headers via codec (end_stream={}):\n{}", *this,
                   encoding_headers_only_ ||
                       (end_stream && continue_data_entry == encoder_filters_.end()),
                   headers);

  // Now actually encode via the codec.
  stream_info_->onFirstDownstreamTxByteSent();
  response_encoder_->encodeHeaders(
      headers,
      encoding_headers_only_ || (end_stream && continue_data_entry == encoder_filters_.end()));
  if (continue_data_entry != encoder_filters_.end()) {
    // We use the continueEncoding() code since it will correctly handle not calling
    // encodeHeaders() again. Fake setting StopSingleIteration since the continueEncoding() code
    // expects it.
    ASSERT(buffered_response_data_);
    (*continue_data_entry)->iteration_state_ =
        ActiveStreamFilterBase::IterationState::StopSingleIteration;
    (*continue_data_entry)->continueEncoding();
  } else {
    // End encoding if this is a header only response, either due to a filter converting it to one
    // or due to the upstream returning headers only.
    maybeEndEncode(encoding_headers_only_ || end_stream);
  }
}

void ActiveStream::encodeMetadata(ActiveStreamEncoderFilter* filter,
                                  MetadataMapPtr&& metadata_map_ptr) {
  resetIdleTimer();

  std::list<ActiveStreamEncoderFilterPtr>::iterator entry =
      commonEncodePrefix(filter, false, FilterIterationStartState::CanStartFromCurrent);

  for (; entry != encoder_filters_.end(); entry++) {
    // If the filter pointed by entry has stopped for all frame type, stores metadata and returns.
    // If the filter pointed by entry hasn't returned from encodeHeaders, stores newly added
    // metadata in case encodeHeaders returns StopAllIteration. The latter can happen when headers
    // callbacks generate new metadata.
    if (!(*entry)->encode_headers_called_ || (*entry)->stoppedAll()) {
      (*entry)->getSavedResponseMetadata()->emplace_back(std::move(metadata_map_ptr));
      return;
    }

    FilterMetadataStatus status = (*entry)->handle_->encodeMetadata(*metadata_map_ptr);
    ENVOY_STREAM_LOG(trace, "encode metadata called: filter={} status={}", *this,
                     static_cast<const void*>((*entry).get()), static_cast<uint64_t>(status));
  }
  // TODO(soya3129): update stats with metadata.

  // Now encode metadata via the codec.
  if (!metadata_map_ptr->empty()) {
    ENVOY_STREAM_LOG(debug, "encoding metadata via codec:\n{}", *this, *metadata_map_ptr);
    MetadataMapVector metadata_map_vector;
    metadata_map_vector.emplace_back(std::move(metadata_map_ptr));
    response_encoder_->encodeMetadata(metadata_map_vector);
  }
}

HeaderMap& ActiveStream::addEncodedTrailers() {
  // Trailers can only be added during the last data frame (i.e. end_stream = true).
  ASSERT(state_.filter_call_state_ & FilterCallState::LastDataFrame);

  // Trailers can only be added once.
  ASSERT(!response_trailers_);

  response_trailers_ = std::make_unique<HeaderMapImpl>();
  return *response_trailers_;
}

void ActiveStream::addEncodedData(ActiveStreamEncoderFilter& filter, Buffer::Instance& data,
                                  bool streaming) {
  if (state_.filter_call_state_ == 0 ||
      (state_.filter_call_state_ & FilterCallState::EncodeHeaders) ||
      (state_.filter_call_state_ & FilterCallState::EncodeData) ||
      ((state_.filter_call_state_ & FilterCallState::EncodeTrailers) && !filter.canIterate())) {
    // Make sure if this triggers watermarks, the correct action is taken.
    state_.encoder_filters_streaming_ = streaming;
    // If no call is happening or we are in the decode headers/data callback, buffer the data.
    // Inline processing happens in the decodeHeaders() callback if necessary.
    filter.commonHandleBufferData(data);
  } else if (state_.filter_call_state_ & FilterCallState::EncodeTrailers) {
    // In this case we need to inline dispatch the data to further filters. If those filters
    // choose to buffer/stop iteration that's fine.
    encodeData(&filter, data, false, FilterIterationStartState::AlwaysStartFromNext);
  } else {
    // TODO(mattklein123): Formalize error handling for filters and add tests. Should probably
    // throw an exception here.
    NOT_IMPLEMENTED_GCOVR_EXCL_LINE;
  }
}

void ActiveStream::encodeData(ActiveStreamEncoderFilter* filter, Buffer::Instance& data,
                              bool end_stream,
                              FilterIterationStartState filter_iteration_start_state) {
  resetIdleTimer();

  // If we previously decided to encode only the headers, do nothing here.
  if (encoding_headers_only_) {
    return;
  }

  // Filter iteration may start at the current filter.
  std::list<ActiveStreamEncoderFilterPtr>::iterator entry =
      commonEncodePrefix(filter, end_stream, filter_iteration_start_state);
  auto trailers_added_entry = encoder_filters_.end();

  const bool trailers_exists_at_start = response_trailers_ != nullptr;
  for (; entry != encoder_filters_.end(); entry++) {
    // If the filter pointed by entry has stopped for all frame type, return now.
    if (handleDataIfStopAll(**entry, data, state_.encoder_filters_streaming_)) {
      return;
    }
    // If end_stream_ is marked for a filter, the data is not for this filter and filters after.
    // For details, please see the comment in the ActiveStream::decodeData() function.
    if ((*entry)->end_stream_) {
      return;
    }
    ASSERT(!(state_.filter_call_state_ & FilterCallState::EncodeData));

    // We check the response_trailers_ pointer here in case addEncodedTrailers
    // is called in encodeData during a previous filter invocation, at which point we communicate to
    // the current and future filters that the stream has not yet ended.
    state_.filter_call_state_ |= FilterCallState::EncodeData;
    if (end_stream) {
      state_.filter_call_state_ |= FilterCallState::LastDataFrame;
    }

    recordLatestDataFilter(entry, state_.latest_data_encoding_filter_, encoder_filters_);

    (*entry)->end_stream_ = end_stream && !response_trailers_;
    FilterDataStatus status = (*entry)->handle_->encodeData(data, (*entry)->end_stream_);
    if ((*entry)->end_stream_) {
      (*entry)->handle_->encodeComplete();
    }
    state_.filter_call_state_ &= ~FilterCallState::EncodeData;
    if (end_stream) {
      state_.filter_call_state_ &= ~FilterCallState::LastDataFrame;
    }
    ENVOY_STREAM_LOG(trace, "encode data called: filter={} status={}", *this,
                     static_cast<const void*>((*entry).get()), static_cast<uint64_t>(status));

    if (!trailers_exists_at_start && response_trailers_ &&
        trailers_added_entry == encoder_filters_.end()) {
      trailers_added_entry = entry;
    }

    if (!(*entry)->commonHandleAfterDataCallback(status, data, state_.encoder_filters_streaming_)) {
      return;
    }
  }

  ENVOY_STREAM_LOG(trace, "encoding data via codec (size={} end_stream={})", *this, data.length(),
                   end_stream);

  stream_info_->addBytesSent(data.length());

  // If trailers were adding during encodeData we need to trigger decodeTrailers in order
  // to allow filters to process the trailers.
  if (trailers_added_entry != encoder_filters_.end()) {
    response_encoder_->encodeData(data, false);
    encodeTrailers(trailers_added_entry->get(), *response_trailers_);
  } else {
    response_encoder_->encodeData(data, end_stream);
    maybeEndEncode(end_stream);
  }
}

void ActiveStream::encodeTrailers(ActiveStreamEncoderFilter* filter, HeaderMap& trailers) {
  resetIdleTimer();

  // If we previously decided to encode only the headers, do nothing here.
  if (encoding_headers_only_) {
    return;
  }

  // Filter iteration may start at the current filter.
  std::list<ActiveStreamEncoderFilterPtr>::iterator entry =
      commonEncodePrefix(filter, true, FilterIterationStartState::CanStartFromCurrent);
  for (; entry != encoder_filters_.end(); entry++) {
    // If the filter pointed by entry has stopped for all frame type, return now.
    if ((*entry)->stoppedAll()) {
      return;
    }
    ASSERT(!(state_.filter_call_state_ & FilterCallState::EncodeTrailers));
    state_.filter_call_state_ |= FilterCallState::EncodeTrailers;
    FilterTrailersStatus status = (*entry)->handle_->encodeTrailers(trailers);
    (*entry)->handle_->encodeComplete();
    (*entry)->end_stream_ = true;
    state_.filter_call_state_ &= ~FilterCallState::EncodeTrailers;
    ENVOY_STREAM_LOG(trace, "encode trailers called: filter={} status={}", *this,
                     static_cast<const void*>((*entry).get()), static_cast<uint64_t>(status));
    if (!(*entry)->commonHandleAfterTrailersCallback(status)) {
      return;
    }
  }

  ENVOY_STREAM_LOG(debug, "encoding trailers via codec:\n{}", *this, trailers);

  response_encoder_->encodeTrailers(trailers);
  maybeEndEncode(true);
}

void ActiveStream::maybeEndEncode(bool end_stream) {
  if (end_stream) {
    ASSERT(!state_.codec_saw_local_complete_);
    state_.codec_saw_local_complete_ = true;
    stream_info_->onLastDownstreamTxByteSent();
    request_response_timespan_->complete();
    stream_manager_.doEndStream(*this);
  }
}

bool ActiveStream::processNewlyAddedMetadata() {
  if (request_metadata_map_vector_ == nullptr) {
    return false;
  }
  for (const auto& metadata_map : *getRequestMetadataMapVector()) {
    decodeMetadata(nullptr, *metadata_map);
  }
  getRequestMetadataMapVector()->clear();
  return true;
}

bool ActiveStream::handleDataIfStopAll(ActiveStreamFilterBase& filter, Buffer::Instance& data,
                                       bool& filter_streaming) {
  if (filter.stoppedAll()) {
    ASSERT(!filter.canIterate());
    filter_streaming =
        filter.iteration_state_ == ActiveStreamFilterBase::IterationState::StopAllWatermark;
    filter.commonHandleBufferData(data);
    return true;
  }
  return false;
}

void ActiveStream::onResetStream(StreamResetReason, absl::string_view) {
  // NOTE: This function gets called in all of the following cases:
  //       1) We TX an app level reset
  //       2) The codec TX a codec level reset
  //       3) The codec RX a reset
  //       If we need to differentiate we need to do it inside the codec. Can start with this.
  ENVOY_STREAM_LOG(debug, "stream reset", *this);
  connection_manager_stats_.named_.downstream_rq_rx_reset_.inc();
  stream_manager_.doDeferredStreamDestroy(*this);
}

void ActiveStream::onAboveWriteBufferHighWatermark() {
  ENVOY_STREAM_LOG(debug, "Disabling upstream stream due to downstream stream watermark.", *this);
  callHighWatermarkCallbacks();
}

void ActiveStream::onBelowWriteBufferLowWatermark() {
  ENVOY_STREAM_LOG(debug, "Enabling upstream stream due to downstream stream watermark.", *this);
  callLowWatermarkCallbacks();
}

Tracing::OperationName ActiveStream::operationName() const {
  return connection_manager_config_.tracingConfig()->operation_name_;
}

const Tracing::CustomTagMap* ActiveStream::customTags() const { return tracing_custom_tags_.get(); }

bool ActiveStream::verbose() const { return connection_manager_config_.tracingConfig()->verbose_; }

uint32_t ActiveStream::maxPathTagLength() const {
  return connection_manager_config_.tracingConfig()->max_path_tag_length_;
}

void ActiveStream::callHighWatermarkCallbacks() {
  ++high_watermark_count_;
  for (auto watermark_callbacks : watermark_callbacks_) {
    watermark_callbacks->onAboveWriteBufferHighWatermark();
  }
}

void ActiveStream::callLowWatermarkCallbacks() {
  ASSERT(high_watermark_count_ > 0);
  --high_watermark_count_;
  for (auto watermark_callbacks : watermark_callbacks_) {
    watermark_callbacks->onBelowWriteBufferLowWatermark();
  }
}

void ActiveStream::setBufferLimit(uint32_t new_limit) {
  ENVOY_STREAM_LOG(debug, "setting buffer limit to {}", *this, new_limit);
  buffer_limit_ = new_limit;
  if (buffered_request_data_) {
    buffered_request_data_->setWatermarks(buffer_limit_);
  }
  if (buffered_response_data_) {
    buffered_response_data_->setWatermarks(buffer_limit_);
  }
}

bool ActiveStream::createFilterChain() {
  if (state_.created_filter_chain_) {
    return false;
  }
  bool upgrade_rejected = false;
  auto upgrade = request_headers_ ? request_headers_->Upgrade() : nullptr;
  state_.created_filter_chain_ = true;
  if (upgrade != nullptr) {
    const Router::RouteEntry::UpgradeMap* upgrade_map = nullptr;

    // We must check if the 'cached_route_' optional is populated since this function can be called
    // early via sendLocalReply(), before the cached route is populated.
    if (hasCachedRoute() && cached_route_.value()->routeEntry()) {
      upgrade_map = &cached_route_.value()->routeEntry()->upgradeMap();
    }

    if (connection_manager_config_.filterFactory().createUpgradeFilterChain(
            upgrade->value().getStringView(), upgrade_map, *this)) {
      state_.successful_upgrade_ = true;
      connection_manager_stats_.named_.downstream_cx_upgrades_total_.inc();
      connection_manager_stats_.named_.downstream_cx_upgrades_active_.inc();
      return true;
    } else {
      upgrade_rejected = true;
      // Fall through to the default filter chain. The function calling this
      // will send a local reply indicating that the upgrade failed.
    }
  }

  connection_manager_config_.filterFactory().createFilterChain(*this);
  return !upgrade_rejected;
}

} // namespace ConnectionManager
} // namespace Http
} // namespace Envoy
