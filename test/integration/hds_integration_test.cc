#include <memory>

#include "envoy/api/v2/eds.pb.h"
#include "envoy/api/v2/endpoint/endpoint.pb.h"
#include "envoy/service/discovery/v2/hds.pb.h"
#include "envoy/upstream/upstream.h"

#include "common/config/metadata.h"
#include "common/config/resources.h"
#include "common/network/utility.h"
#include "common/protobuf/utility.h"
#include "common/upstream/health_checker_impl.h"
#include "common/upstream/health_discovery_service.h"

#include "test/common/upstream/utility.h"
#include "test/config/utility.h"
#include "test/integration/http_integration.h"
#include "test/test_common/network_utility.h"
#include "test/test_common/simulated_time_system.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace {

// TODO(jmarantz): switch this to simulated-time after debugging flakes.
class HdsIntegrationTest : public testing::TestWithParam<Network::Address::IpVersion>,
                           public HttpIntegrationTest {
public:
  HdsIntegrationTest() : HttpIntegrationTest(Http::CodecClient::Type::HTTP1, GetParam()) {}

  void createUpstreams() override {
    fake_upstreams_.emplace_back(
        new FakeUpstream(0, FakeHttpConnection::Type::HTTP2, version_, timeSystem()));
    hds_upstream_ = fake_upstreams_.back().get();
    hds_upstream_->set_allow_unexpected_disconnects(true);
    HttpIntegrationTest::createUpstreams();
  }

  void initialize() override {
    setUpstreamCount(upstream_endpoints_);
    config_helper_.addConfigModifier([](envoy::config::bootstrap::v2::Bootstrap& bootstrap) {
      // Setup hds and corresponding gRPC cluster.
      auto* hds_config = bootstrap.mutable_hds_config();
      hds_config->set_api_type(envoy::api::v2::core::ApiConfigSource::GRPC);
      hds_config->add_grpc_services()->mutable_envoy_grpc()->set_cluster_name("hds_cluster");
      auto* hds_cluster = bootstrap.mutable_static_resources()->add_clusters();
      hds_cluster->MergeFrom(bootstrap.static_resources().clusters()[0]);
      hds_cluster->mutable_circuit_breakers()->Clear();
      hds_cluster->set_name("hds_cluster");
      hds_cluster->mutable_http2_protocol_options();
      auto* cluster_0 = bootstrap.mutable_static_resources()->mutable_clusters(0);
      cluster_0->mutable_hosts()->Clear();
    });

    HttpIntegrationTest::initialize();

    // Endpoint connections
    host_upstream_ =
        std::make_unique<FakeUpstream>(0, FakeHttpConnection::Type::HTTP1, version_, timeSystem());
    host2_upstream_ =
        std::make_unique<FakeUpstream>(0, FakeHttpConnection::Type::HTTP1, version_, timeSystem());
  }

  // Sets up a connection between Envoy and the management server.
  void waitForHdsStream() {
    AssertionResult result =
        hds_upstream_->waitForHttpConnection(*dispatcher_, hds_fake_connection_);
    RELEASE_ASSERT(result, result.message());
    result = hds_fake_connection_->waitForNewStream(*dispatcher_, hds_stream_);
    RELEASE_ASSERT(result, result.message());
  }

  // Envoy sends health check messages to the endpoints
  void healthcheckEndpoints(std::string cluster2 = "") {
    ASSERT_TRUE(host_upstream_->waitForHttpConnection(*dispatcher_, host_fake_connection_));
    ASSERT_TRUE(host_fake_connection_->waitForNewStream(*dispatcher_, host_stream_));
    ASSERT_TRUE(host_stream_->waitForEndStream(*dispatcher_));

    host_upstream_->set_allow_unexpected_disconnects(true);
    EXPECT_EQ(host_stream_->headers().Path()->value().getStringView(), "/healthcheck");
    EXPECT_EQ(host_stream_->headers().Method()->value().getStringView(), "GET");
    EXPECT_EQ(host_stream_->headers().Host()->value().getStringView(), "anna");

    if (!cluster2.empty()) {
      ASSERT_TRUE(host2_upstream_->waitForHttpConnection(*dispatcher_, host2_fake_connection_));
      ASSERT_TRUE(host2_fake_connection_->waitForNewStream(*dispatcher_, host2_stream_));
      ASSERT_TRUE(host2_stream_->waitForEndStream(*dispatcher_));

      host2_upstream_->set_allow_unexpected_disconnects(true);
      EXPECT_EQ(host2_stream_->headers().Path()->value().getStringView(), "/healthcheck");
      EXPECT_EQ(host2_stream_->headers().Method()->value().getStringView(), "GET");
      EXPECT_EQ(host2_stream_->headers().Host()->value().getStringView(), cluster2);
    }
  }

  // Clean up the connection between Envoy and the management server
  void cleanupHdsConnection() {
    if (hds_fake_connection_ != nullptr) {
      AssertionResult result = hds_fake_connection_->close();
      RELEASE_ASSERT(result, result.message());
      result = hds_fake_connection_->waitForDisconnect();
      RELEASE_ASSERT(result, result.message());
    }
  }

  // Clean up connections between Envoy and endpoints
  void cleanupHostConnections() {
    if (host_fake_connection_ != nullptr) {
      AssertionResult result = host_fake_connection_->close();
      RELEASE_ASSERT(result, result.message());
      result = host_fake_connection_->waitForDisconnect();
      RELEASE_ASSERT(result, result.message());
    }
    if (host2_fake_connection_ != nullptr) {
      AssertionResult result = host2_fake_connection_->close();
      RELEASE_ASSERT(result, result.message());
      result = host2_fake_connection_->waitForDisconnect();
      RELEASE_ASSERT(result, result.message());
    }
  }

  // Creates a basic HealthCheckSpecifier message containing one endpoint and
  // one HTTP health_check
  envoy::service::discovery::v2::HealthCheckSpecifier makeHttpHealthCheckSpecifier() {
    envoy::service::discovery::v2::HealthCheckSpecifier server_health_check_specifier_;
    server_health_check_specifier_.mutable_interval()->set_nanos(100000000); // 0.1 seconds

    auto* health_check = server_health_check_specifier_.add_cluster_health_checks();

    health_check->set_cluster_name("anna");
    Network::Utility::addressToProtobufAddress(
        *host_upstream_->localAddress(),
        *health_check->add_locality_endpoints()->add_endpoints()->mutable_address());
    health_check->mutable_locality_endpoints(0)->mutable_locality()->set_region("middle_earth");
    health_check->mutable_locality_endpoints(0)->mutable_locality()->set_zone("shire");
    health_check->mutable_locality_endpoints(0)->mutable_locality()->set_sub_zone("hobbiton");

    health_check->add_health_checks()->mutable_timeout()->set_seconds(MaxTimeout);
    health_check->mutable_health_checks(0)->mutable_interval()->set_seconds(MaxTimeout);
    health_check->mutable_health_checks(0)->mutable_unhealthy_threshold()->set_value(2);
    health_check->mutable_health_checks(0)->mutable_healthy_threshold()->set_value(2);
    health_check->mutable_health_checks(0)->mutable_grpc_health_check();
    health_check->mutable_health_checks(0)->mutable_http_health_check()->set_use_http2(false);
    health_check->mutable_health_checks(0)->mutable_http_health_check()->set_path("/healthcheck");

    return server_health_check_specifier_;
  }

  // Creates a basic HealthCheckSpecifier message containing one endpoint and
  // one TCP health_check
  envoy::service::discovery::v2::HealthCheckSpecifier makeTcpHealthCheckSpecifier() {
    envoy::service::discovery::v2::HealthCheckSpecifier server_health_check_specifier_;
    server_health_check_specifier_.mutable_interval()->set_nanos(100000000); // 0.1 seconds

    auto* health_check = server_health_check_specifier_.add_cluster_health_checks();

    health_check->set_cluster_name("anna");
    Network::Utility::addressToProtobufAddress(
        *host_upstream_->localAddress(),
        *health_check->add_locality_endpoints()->add_endpoints()->mutable_address());
    health_check->mutable_locality_endpoints(0)->mutable_locality()->set_region("middle_earth");
    health_check->mutable_locality_endpoints(0)->mutable_locality()->set_zone("eriador");
    health_check->mutable_locality_endpoints(0)->mutable_locality()->set_sub_zone("rivendell");

    health_check->add_health_checks()->mutable_timeout()->set_seconds(MaxTimeout);
    health_check->mutable_health_checks(0)->mutable_interval()->set_seconds(MaxTimeout);
    health_check->mutable_health_checks(0)->mutable_unhealthy_threshold()->set_value(2);
    health_check->mutable_health_checks(0)->mutable_healthy_threshold()->set_value(2);
    auto* tcp_health_check = health_check->mutable_health_checks(0)->mutable_tcp_health_check();
    tcp_health_check->mutable_send()->set_text("50696E67");
    tcp_health_check->add_receive()->set_text("506F6E67");

    return server_health_check_specifier_;
  }

  // Checks if Envoy reported the health status of an endpoint correctly
  bool checkEndpointHealthResponse(envoy::service::discovery::v2::EndpointHealth endpoint,
                                   envoy::api::v2::core::HealthStatus healthy,
                                   Network::Address::InstanceConstSharedPtr address) {

    if (healthy != endpoint.health_status()) {
      return false;
    }
    if (address->ip()->port() != endpoint.endpoint().address().socket_address().port_value()) {
      return false;
    }
    if (address->ip()->addressAsString() !=
        endpoint.endpoint().address().socket_address().address()) {
      return false;
    }
    return true;
  }

  // Checks if the cluster counters are correct
  void checkCounters(int requests, int responses, int successes, int failures) {
    EXPECT_EQ(requests, test_server_->counter("hds_delegate.requests")->value());
    EXPECT_LE(responses, test_server_->counter("hds_delegate.responses")->value());
    EXPECT_EQ(successes, test_server_->counter("cluster.anna.health_check.success")->value());
    EXPECT_EQ(failures, test_server_->counter("cluster.anna.health_check.failure")->value());
  }

  void waitForEndpointHealthResponse(envoy::api::v2::core::HealthStatus healthy) {
    ASSERT_TRUE(hds_stream_->waitForGrpcMessage(*dispatcher_, response_));
    while (!checkEndpointHealthResponse(response_.endpoint_health_response().endpoints_health(0),
                                        healthy, host_upstream_->localAddress())) {
      ASSERT_TRUE(hds_stream_->waitForGrpcMessage(*dispatcher_, response_));
    }
  }

  static constexpr uint32_t upstream_endpoints_ = 0;

  FakeHttpConnectionPtr hds_fake_connection_;
  FakeStreamPtr hds_stream_;
  FakeUpstream* hds_upstream_{};
  uint32_t hds_requests_{};
  FakeUpstreamPtr host_upstream_{};
  FakeUpstreamPtr host2_upstream_{};
  FakeStreamPtr host_stream_;
  FakeStreamPtr host2_stream_;
  FakeHttpConnectionPtr host_fake_connection_;
  FakeHttpConnectionPtr host2_fake_connection_;
  FakeRawConnectionPtr host_fake_raw_connection_;

  static constexpr int MaxTimeout = 100;
  envoy::service::discovery::v2::HealthCheckRequestOrEndpointHealthResponse envoy_msg_;
  envoy::service::discovery::v2::HealthCheckRequestOrEndpointHealthResponse response_;
  envoy::service::discovery::v2::HealthCheckSpecifier server_health_check_specifier_;
};

INSTANTIATE_TEST_SUITE_P(IpVersions, HdsIntegrationTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()),
                         TestUtility::ipTestParamsToString);

// Tests Envoy HTTP health checking a single healthy endpoint and reporting that it is
// indeed healthy to the server.
TEST_P(HdsIntegrationTest, SingleEndpointHealthyHttp) {
  initialize();

  // Server <--> Envoy
  waitForHdsStream();
  ASSERT_TRUE(hds_stream_->waitForGrpcMessage(*dispatcher_, envoy_msg_));
  EXPECT_EQ(envoy_msg_.health_check_request().capability().health_check_protocols(0),
            envoy::service::discovery::v2::Capability::HTTP);

  // Server asks for health checking
  server_health_check_specifier_ = makeHttpHealthCheckSpecifier();
  hds_stream_->startGrpcStream();
  hds_stream_->sendGrpcMessage(server_health_check_specifier_);
  test_server_->waitForCounterGe("hds_delegate.requests", ++hds_requests_);

  // Envoy sends a health check message to an endpoint
  healthcheckEndpoints();

  // Endpoint responds to the health check
  host_stream_->encodeHeaders(Http::TestHeaderMapImpl{{":status", "200"}}, false);
  host_stream_->encodeData(1024, true);

  // Receive updates until the one we expect arrives
  waitForEndpointHealthResponse(envoy::api::v2::core::HealthStatus::HEALTHY);

  checkCounters(1, 2, 1, 0);

  // Clean up connections
  cleanupHostConnections();
  cleanupHdsConnection();
}

// Tests Envoy HTTP health checking a single endpoint that times out and reporting
// that it is unhealthy to the server.
TEST_P(HdsIntegrationTest, SingleEndpointTimeoutHttp) {
  initialize();
  server_health_check_specifier_ = makeHttpHealthCheckSpecifier();

  server_health_check_specifier_.mutable_cluster_health_checks(0)
      ->mutable_health_checks(0)
      ->mutable_timeout()
      ->set_seconds(0);
  server_health_check_specifier_.mutable_cluster_health_checks(0)
      ->mutable_health_checks(0)
      ->mutable_timeout()
      ->set_nanos(100000000); // 0.1 seconds

  // Server <--> Envoy
  waitForHdsStream();
  ASSERT_TRUE(hds_stream_->waitForGrpcMessage(*dispatcher_, envoy_msg_));

  // Server asks for health checking
  hds_stream_->startGrpcStream();
  hds_stream_->sendGrpcMessage(server_health_check_specifier_);
  test_server_->waitForCounterGe("hds_delegate.requests", ++hds_requests_);

  // Envoy sends a health check message to an endpoint
  healthcheckEndpoints();

  // Endpoint doesn't respond to the health check

  // Receive updates until the one we expect arrives
  waitForEndpointHealthResponse(envoy::api::v2::core::HealthStatus::TIMEOUT);

  checkCounters(1, 2, 0, 1);

  // Clean up connections
  cleanupHostConnections();
  cleanupHdsConnection();
}

// Tests Envoy HTTP health checking a single unhealthy endpoint and reporting that it is
// indeed unhealthy to the server.
TEST_P(HdsIntegrationTest, SingleEndpointUnhealthyHttp) {
  initialize();
  server_health_check_specifier_ = makeHttpHealthCheckSpecifier();

  // Server <--> Envoy
  waitForHdsStream();
  ASSERT_TRUE(hds_stream_->waitForGrpcMessage(*dispatcher_, envoy_msg_));

  // Server asks for health checking
  hds_stream_->startGrpcStream();
  hds_stream_->sendGrpcMessage(server_health_check_specifier_);
  test_server_->waitForCounterGe("hds_delegate.requests", ++hds_requests_);

  // Envoy sends a health check message to an endpoint
  healthcheckEndpoints();

  // Endpoint responds to the health check
  host_stream_->encodeHeaders(Http::TestHeaderMapImpl{{":status", "404"}}, false);
  host_stream_->encodeData(1024, true);

  // Receive updates until the one we expect arrives
  waitForEndpointHealthResponse(envoy::api::v2::core::HealthStatus::UNHEALTHY);

  checkCounters(1, 2, 0, 1);

  // Clean up connections
  cleanupHostConnections();
  cleanupHdsConnection();
}

// Tests Envoy TCP health checking an endpoint that doesn't respond and reporting that it is
// unhealthy to the server.
TEST_P(HdsIntegrationTest, SingleEndpointTimeoutTcp) {
  initialize();

  // Server <--> Envoy
  waitForHdsStream();
  ASSERT_TRUE(hds_stream_->waitForGrpcMessage(*dispatcher_, envoy_msg_));
  EXPECT_EQ(envoy_msg_.health_check_request().capability().health_check_protocols(1),
            envoy::service::discovery::v2::Capability::TCP);

  // Server asks for health checking
  server_health_check_specifier_ = makeTcpHealthCheckSpecifier();
  server_health_check_specifier_.mutable_cluster_health_checks(0)
      ->mutable_health_checks(0)
      ->mutable_timeout()
      ->set_seconds(0);
  server_health_check_specifier_.mutable_cluster_health_checks(0)
      ->mutable_health_checks(0)
      ->mutable_timeout()
      ->set_nanos(500000000); // 0.5 seconds

  hds_stream_->startGrpcStream();
  hds_stream_->sendGrpcMessage(server_health_check_specifier_);
  test_server_->waitForCounterGe("hds_delegate.requests", ++hds_requests_);

  // Envoys asks the endpoint if it's healthy
  host_upstream_->set_allow_unexpected_disconnects(true);
  ASSERT_TRUE(host_upstream_->waitForRawConnection(host_fake_raw_connection_));
  ASSERT_TRUE(
      host_fake_raw_connection_->waitForData(FakeRawConnection::waitForInexactMatch("Ping")));

  // No response from the endpoint

  // Receive updates until the one we expect arrives
  waitForEndpointHealthResponse(envoy::api::v2::core::HealthStatus::TIMEOUT);

  // Clean up connections
  cleanupHostConnections();
  cleanupHdsConnection();
}

// Tests Envoy TCP health checking a single healthy endpoint and reporting that it is
// indeed healthy to the server.
TEST_P(HdsIntegrationTest, SingleEndpointHealthyTcp) {
  initialize();

  // Server <--> Envoy
  waitForHdsStream();
  ASSERT_TRUE(hds_stream_->waitForGrpcMessage(*dispatcher_, envoy_msg_));

  // Server asks for health checking
  server_health_check_specifier_ = makeTcpHealthCheckSpecifier();
  hds_stream_->startGrpcStream();
  hds_stream_->sendGrpcMessage(server_health_check_specifier_);
  test_server_->waitForCounterGe("hds_delegate.requests", ++hds_requests_);

  // Envoy asks the endpoint if it's healthy
  host_upstream_->set_allow_unexpected_disconnects(true);
  ASSERT_TRUE(host_upstream_->waitForRawConnection(host_fake_raw_connection_));
  ASSERT_TRUE(
      host_fake_raw_connection_->waitForData(FakeRawConnection::waitForInexactMatch("Ping")));
  AssertionResult result = host_fake_raw_connection_->write("Pong");
  RELEASE_ASSERT(result, result.message());

  // Receive updates until the one we expect arrives
  waitForEndpointHealthResponse(envoy::api::v2::core::HealthStatus::HEALTHY);

  // Clean up connections
  cleanupHostConnections();
  cleanupHdsConnection();
}

// Tests Envoy TCP health checking a single unhealthy endpoint and reporting that it is
// indeed unhealthy to the server.
TEST_P(HdsIntegrationTest, SingleEndpointUnhealthyTcp) {
  initialize();

  // Server <--> Envoy
  waitForHdsStream();
  ASSERT_TRUE(hds_stream_->waitForGrpcMessage(*dispatcher_, envoy_msg_));

  // Server asks for health checking
  server_health_check_specifier_ = makeTcpHealthCheckSpecifier();
  server_health_check_specifier_.mutable_cluster_health_checks(0)
      ->mutable_health_checks(0)
      ->mutable_timeout()
      ->set_seconds(2);
  hds_stream_->startGrpcStream();
  hds_stream_->sendGrpcMessage(server_health_check_specifier_);
  test_server_->waitForCounterGe("hds_delegate.requests", ++hds_requests_);

  // Envoy asks the endpoint if it's healthy
  host_upstream_->set_allow_unexpected_disconnects(true);
  ASSERT_TRUE(host_upstream_->waitForRawConnection(host_fake_raw_connection_));
  ASSERT_TRUE(
      host_fake_raw_connection_->waitForData(FakeRawConnection::waitForInexactMatch("Ping")));
  AssertionResult result = host_fake_raw_connection_->write("Voronoi");
  RELEASE_ASSERT(result, result.message());

  // Receive updates until the one we expect arrives
  waitForEndpointHealthResponse(envoy::api::v2::core::HealthStatus::UNHEALTHY);

  // Clean up connections
  cleanupHostConnections();
  cleanupHdsConnection();
}

// Tests that Envoy can HTTP health check two hosts that are in the same cluster, and
// the same locality and report back the correct health statuses.
TEST_P(HdsIntegrationTest, TwoEndpointsSameLocality) {
  initialize();

  server_health_check_specifier_ = makeHttpHealthCheckSpecifier();
  Network::Utility::addressToProtobufAddress(
      *host2_upstream_->localAddress(),
      *server_health_check_specifier_.mutable_cluster_health_checks(0)
           ->mutable_locality_endpoints(0)
           ->add_endpoints()
           ->mutable_address());
  // Server <--> Envoy
  waitForHdsStream();
  ASSERT_TRUE(hds_stream_->waitForGrpcMessage(*dispatcher_, envoy_msg_));

  // Server asks for health checking
  hds_stream_->startGrpcStream();
  hds_stream_->sendGrpcMessage(server_health_check_specifier_);
  test_server_->waitForCounterGe("hds_delegate.requests", ++hds_requests_);

  healthcheckEndpoints("anna");

  // Endpoints respond to the health check
  host_stream_->encodeHeaders(Http::TestHeaderMapImpl{{":status", "404"}}, false);
  host_stream_->encodeData(1024, true);
  host2_stream_->encodeHeaders(Http::TestHeaderMapImpl{{":status", "200"}}, false);
  host2_stream_->encodeData(1024, true);

  // Receive updates until the one we expect arrives
  ASSERT_TRUE(hds_stream_->waitForGrpcMessage(*dispatcher_, response_));
  while (!(checkEndpointHealthResponse(response_.endpoint_health_response().endpoints_health(0),
                                       envoy::api::v2::core::HealthStatus::UNHEALTHY,
                                       host_upstream_->localAddress()) &&
           checkEndpointHealthResponse(response_.endpoint_health_response().endpoints_health(1),
                                       envoy::api::v2::core::HealthStatus::HEALTHY,
                                       host2_upstream_->localAddress()))) {
    ASSERT_TRUE(hds_stream_->waitForGrpcMessage(*dispatcher_, response_));
  }

  checkCounters(1, 2, 1, 1);

  // Clean up connections
  cleanupHostConnections();
  cleanupHdsConnection();
}

// Tests that Envoy can HTTP health check two hosts that are in the same cluster, and
// different localities and report back the correct health statuses.
TEST_P(HdsIntegrationTest, TwoEndpointsDifferentLocality) {
  initialize();
  server_health_check_specifier_ = makeHttpHealthCheckSpecifier();

  // Add endpoint
  auto* health_check = server_health_check_specifier_.mutable_cluster_health_checks(0);

  Network::Utility::addressToProtobufAddress(
      *host2_upstream_->localAddress(),
      *health_check->add_locality_endpoints()->add_endpoints()->mutable_address());
  health_check->mutable_locality_endpoints(1)->mutable_locality()->set_region("plakias");
  health_check->mutable_locality_endpoints(1)->mutable_locality()->set_zone("fragokastelo");
  health_check->mutable_locality_endpoints(1)->mutable_locality()->set_sub_zone("emplisi");

  // Server <--> Envoy
  waitForHdsStream();
  ASSERT_TRUE(hds_stream_->waitForGrpcMessage(*dispatcher_, envoy_msg_));

  // Server asks for health checking
  hds_stream_->startGrpcStream();
  hds_stream_->sendGrpcMessage(server_health_check_specifier_);
  test_server_->waitForCounterGe("hds_delegate.requests", ++hds_requests_);

  // Envoy sends health check messages to two endpoints
  healthcheckEndpoints("anna");

  // Endpoint responds to the health check
  host_stream_->encodeHeaders(Http::TestHeaderMapImpl{{":status", "404"}}, false);
  host_stream_->encodeData(1024, true);
  host2_stream_->encodeHeaders(Http::TestHeaderMapImpl{{":status", "200"}}, false);
  host2_stream_->encodeData(1024, true);

  // Receive updates until the one we expect arrives
  ASSERT_TRUE(hds_stream_->waitForGrpcMessage(*dispatcher_, response_));
  while (!(checkEndpointHealthResponse(response_.endpoint_health_response().endpoints_health(0),
                                       envoy::api::v2::core::HealthStatus::UNHEALTHY,
                                       host_upstream_->localAddress()) &&
           checkEndpointHealthResponse(response_.endpoint_health_response().endpoints_health(1),
                                       envoy::api::v2::core::HealthStatus::HEALTHY,
                                       host2_upstream_->localAddress()))) {
    ASSERT_TRUE(hds_stream_->waitForGrpcMessage(*dispatcher_, response_));
  }

  checkCounters(1, 2, 1, 1);

  // Clean up connections
  cleanupHostConnections();
  cleanupHdsConnection();
}

// Tests that Envoy can HTTP health check two hosts that are in different clusters, and
// report back the correct health statuses.
TEST_P(HdsIntegrationTest, TwoEndpointsDifferentClusters) {
  initialize();
  server_health_check_specifier_ = makeHttpHealthCheckSpecifier();

  // Add endpoint
  auto* health_check = server_health_check_specifier_.add_cluster_health_checks();

  health_check->set_cluster_name("cat");
  Network::Utility::addressToProtobufAddress(
      *host2_upstream_->localAddress(),
      *health_check->add_locality_endpoints()->add_endpoints()->mutable_address());
  health_check->mutable_locality_endpoints(0)->mutable_locality()->set_region("kounopetra");
  health_check->mutable_locality_endpoints(0)->mutable_locality()->set_zone("emplisi");
  health_check->mutable_locality_endpoints(0)->mutable_locality()->set_sub_zone("paris");

  health_check->add_health_checks()->mutable_timeout()->set_seconds(MaxTimeout);
  health_check->mutable_health_checks(0)->mutable_interval()->set_seconds(MaxTimeout);
  health_check->mutable_health_checks(0)->mutable_unhealthy_threshold()->set_value(2);
  health_check->mutable_health_checks(0)->mutable_healthy_threshold()->set_value(2);
  health_check->mutable_health_checks(0)->mutable_grpc_health_check();
  health_check->mutable_health_checks(0)->mutable_http_health_check()->set_use_http2(false);
  health_check->mutable_health_checks(0)->mutable_http_health_check()->set_path("/healthcheck");

  // Server <--> Envoy
  waitForHdsStream();
  ASSERT_TRUE(hds_stream_->waitForGrpcMessage(*dispatcher_, envoy_msg_));

  // Server asks for health checking
  hds_stream_->startGrpcStream();
  hds_stream_->sendGrpcMessage(server_health_check_specifier_);
  test_server_->waitForCounterGe("hds_delegate.requests", ++hds_requests_);

  // Envoy sends health check messages to two endpoints
  healthcheckEndpoints("cat");

  // Endpoint responds to the health check
  host_stream_->encodeHeaders(Http::TestHeaderMapImpl{{":status", "404"}}, false);
  host_stream_->encodeData(1024, true);
  host2_stream_->encodeHeaders(Http::TestHeaderMapImpl{{":status", "200"}}, false);
  host2_stream_->encodeData(1024, true);

  // Receive updates until the one we expect arrives
  ASSERT_TRUE(hds_stream_->waitForGrpcMessage(*dispatcher_, response_));
  while (!(checkEndpointHealthResponse(response_.endpoint_health_response().endpoints_health(0),
                                       envoy::api::v2::core::HealthStatus::UNHEALTHY,
                                       host_upstream_->localAddress()) &&
           checkEndpointHealthResponse(response_.endpoint_health_response().endpoints_health(1),
                                       envoy::api::v2::core::HealthStatus::HEALTHY,
                                       host2_upstream_->localAddress()))) {
    ASSERT_TRUE(hds_stream_->waitForGrpcMessage(*dispatcher_, response_));
  }

  checkCounters(1, 2, 0, 1);
  EXPECT_EQ(1, test_server_->counter("cluster.cat.health_check.success")->value());
  EXPECT_EQ(0, test_server_->counter("cluster.cat.health_check.failure")->value());

  // Clean up connections
  cleanupHostConnections();
  cleanupHdsConnection();
}

// Tests Envoy HTTP health checking a single endpoint, receiving an update
// message from the management server and health checking a new endpoint
TEST_P(HdsIntegrationTest, TestUpdateMessage) {
  initialize();

  // Server <--> Envoy
  waitForHdsStream();
  ASSERT_TRUE(hds_stream_->waitForGrpcMessage(*dispatcher_, envoy_msg_));

  // Server asks for health checking
  server_health_check_specifier_ = makeHttpHealthCheckSpecifier();
  hds_stream_->startGrpcStream();
  hds_stream_->sendGrpcMessage(server_health_check_specifier_);
  test_server_->waitForCounterGe("hds_delegate.requests", ++hds_requests_);

  // Envoy sends a health check message to an endpoint
  healthcheckEndpoints();

  // Endpoint responds to the health check
  host_stream_->encodeHeaders(Http::TestHeaderMapImpl{{":status", "200"}}, false);
  host_stream_->encodeData(1024, true);

  // Receive updates until the one we expect arrives
  waitForEndpointHealthResponse(envoy::api::v2::core::HealthStatus::HEALTHY);

  checkCounters(1, 2, 1, 0);

  cleanupHostConnections();

  // New HealthCheckSpecifier message
  envoy::service::discovery::v2::HealthCheckSpecifier new_message;
  new_message.mutable_interval()->set_nanos(100000000); // 0.1 seconds

  auto* health_check = new_message.add_cluster_health_checks();

  health_check->set_cluster_name("cat");
  Network::Utility::addressToProtobufAddress(
      *host2_upstream_->localAddress(),
      *health_check->add_locality_endpoints()->add_endpoints()->mutable_address());

  health_check->mutable_locality_endpoints(0)->mutable_locality()->set_region("matala");
  health_check->mutable_locality_endpoints(0)->mutable_locality()->set_zone("tilburg");
  health_check->mutable_locality_endpoints(0)->mutable_locality()->set_sub_zone("rivendell");

  health_check->add_health_checks()->mutable_timeout()->set_seconds(MaxTimeout);
  health_check->mutable_health_checks(0)->mutable_interval()->set_seconds(MaxTimeout);
  health_check->mutable_health_checks(0)->mutable_unhealthy_threshold()->set_value(2);
  health_check->mutable_health_checks(0)->mutable_healthy_threshold()->set_value(2);
  health_check->mutable_health_checks(0)->mutable_grpc_health_check();
  health_check->mutable_health_checks(0)->mutable_http_health_check()->set_use_http2(false);
  health_check->mutable_health_checks(0)->mutable_http_health_check()->set_path("/healthcheck");

  // Server asks for health checking with the new message
  hds_stream_->sendGrpcMessage(new_message);
  test_server_->waitForCounterGe("hds_delegate.requests", ++hds_requests_);

  // Envoy sends a health check message to an endpoint
  host2_upstream_->set_allow_unexpected_disconnects(true);
  ASSERT_TRUE(host2_upstream_->waitForHttpConnection(*dispatcher_, host2_fake_connection_));
  ASSERT_TRUE(host2_fake_connection_->waitForNewStream(*dispatcher_, host2_stream_));
  ASSERT_TRUE(host2_stream_->waitForEndStream(*dispatcher_));

  // Endpoint responds to the health check
  host2_stream_->encodeHeaders(Http::TestHeaderMapImpl{{":status", "404"}}, false);
  host2_stream_->encodeData(1024, true);

  // Receive updates until the one we expect arrives
  ASSERT_TRUE(hds_stream_->waitForGrpcMessage(*dispatcher_, response_));
  while (!checkEndpointHealthResponse(response_.endpoint_health_response().endpoints_health(0),
                                      envoy::api::v2::core::HealthStatus::UNHEALTHY,
                                      host2_upstream_->localAddress())) {
    ASSERT_TRUE(hds_stream_->waitForGrpcMessage(*dispatcher_, response_));
  }

  // Clean up connections
  cleanupHostConnections();
  cleanupHdsConnection();
}

// Tests Envoy HTTP health checking a single endpoint, receiving an update
// message from the management server and reporting in a new interval
TEST_P(HdsIntegrationTest, TestUpdateChangesTimer) {
  initialize();

  // Server <--> Envoy
  waitForHdsStream();
  ASSERT_TRUE(hds_stream_->waitForGrpcMessage(*dispatcher_, envoy_msg_));

  // Server asks for health checking
  server_health_check_specifier_ = makeHttpHealthCheckSpecifier();
  hds_stream_->startGrpcStream();
  hds_stream_->sendGrpcMessage(server_health_check_specifier_);
  test_server_->waitForCounterGe("hds_delegate.requests", ++hds_requests_);

  healthcheckEndpoints();

  // an update should be received after interval
  ASSERT_TRUE(
      hds_stream_->waitForGrpcMessage(*dispatcher_, response_, std::chrono::milliseconds(250)));

  // New HealthCheckSpecifier message
  server_health_check_specifier_.mutable_interval()->set_nanos(300000000); // 0.3 seconds

  // Server asks for health checking with the new message
  hds_stream_->sendGrpcMessage(server_health_check_specifier_);
  test_server_->waitForCounterGe("hds_delegate.requests", ++hds_requests_);

  // A response should not be received until the new timer is completed
  ASSERT_FALSE(
      hds_stream_->waitForGrpcMessage(*dispatcher_, response_, std::chrono::milliseconds(100)));
  // Response should be received now
  ASSERT_TRUE(
      hds_stream_->waitForGrpcMessage(*dispatcher_, response_, std::chrono::milliseconds(400)));

  // Clean up connections
  cleanupHostConnections();
  cleanupHdsConnection();
}

// Tests Envoy HTTP health checking a single endpoint when interval hasn't been defined
TEST_P(HdsIntegrationTest, TestDefaultTimer) {
  initialize();

  // Server <--> Envoy
  waitForHdsStream();
  ASSERT_TRUE(hds_stream_->waitForGrpcMessage(*dispatcher_, envoy_msg_));

  // Server asks for health checking
  server_health_check_specifier_ = makeHttpHealthCheckSpecifier();
  server_health_check_specifier_.clear_interval();
  hds_stream_->startGrpcStream();
  hds_stream_->sendGrpcMessage(server_health_check_specifier_);
  test_server_->waitForCounterGe("hds_delegate.requests", ++hds_requests_);

  healthcheckEndpoints();

  // an update should be received after interval
  ASSERT_TRUE(
      hds_stream_->waitForGrpcMessage(*dispatcher_, response_, std::chrono::milliseconds(2500)));

  // Clean up connections
  cleanupHostConnections();
  cleanupHdsConnection();
}

} // namespace
} // namespace Envoy
