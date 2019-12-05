
ConnectionManager::ActiveStream::ActiveStream(ConnectionManagerImpl& connection_manager)
    : connection_manager_(connection_manager),
      stream_id_(connection_manager.random_generator_.random()),
      request_response_timespan_(new Stats::HistogramCompletableTimespanImpl(
          connection_manager_.stats_.named_.downstream_rq_time_, connection_manager_.timeSource())),
      stream_info_(connection_manager_.codec_->protocol(), connection_manager_.timeSource()),
      upstream_options_(std::make_shared<Network::Socket::Options>()) {
  ASSERT(!connection_manager.config_.isRoutable() ||
             ((connection_manager.config_.routeConfigProvider() == nullptr &&
               connection_manager.config_.scopedRouteConfigProvider() != nullptr) ||
              (connection_manager.config_.routeConfigProvider() != nullptr &&
               connection_manager.config_.scopedRouteConfigProvider() == nullptr)),
         "Either routeConfigProvider or scopedRouteConfigProvider should be set in "
         "ConnectionManagerImpl.");

  ScopeTrackerScopeState scope(this,
                               connection_manager_.read_callbacks_->connection().dispatcher());

  connection_manager_.stats_.named_.downstream_rq_total_.inc();
  connection_manager_.stats_.named_.downstream_rq_active_.inc();
  if (connection_manager_.codec_->protocol() == Protocol::Http2) {
    connection_manager_.stats_.named_.downstream_rq_http2_total_.inc();
  } else if (connection_manager_.codec_->protocol() == Protocol::Http3) {
    connection_manager_.stats_.named_.downstream_rq_http3_total_.inc();
  } else {
    connection_manager_.stats_.named_.downstream_rq_http1_total_.inc();
  }
  stream_info_.setDownstreamLocalAddress(
      connection_manager_.read_callbacks_->connection().localAddress());
  stream_info_.setDownstreamDirectRemoteAddress(
      connection_manager_.read_callbacks_->connection().remoteAddress());
  // Initially, the downstream remote address is the source address of the
  // downstream connection. That can change later in the request's lifecycle,
  // based on XFF processing, but setting the downstream remote address here
  // prevents surprises for logging code in edge cases.
  stream_info_.setDownstreamRemoteAddress(
      connection_manager_.read_callbacks_->connection().remoteAddress());

  stream_info_.setDownstreamSslConnection(connection_manager_.read_callbacks_->connection().ssl());

  if (connection_manager_.config_.streamIdleTimeout().count()) {
    idle_timeout_ms_ = connection_manager_.config_.streamIdleTimeout();
    stream_idle_timer_ = connection_manager_.read_callbacks_->connection().dispatcher().createTimer(
        [this]() -> void { onIdleTimeout(); });
    resetIdleTimer();
  }

  if (connection_manager_.config_.requestTimeout().count()) {
    std::chrono::milliseconds request_timeout_ms_ = connection_manager_.config_.requestTimeout();
    request_timer_ = connection_manager.read_callbacks_->connection().dispatcher().createTimer(
        [this]() -> void { onRequestTimeout(); });
    request_timer_->enableTimer(request_timeout_ms_, this);
  }

  stream_info_.setRequestedServerName(
      connection_manager_.read_callbacks_->connection().requestedServerName());
}
