/*
 *  Copyright (c) 2015-present, Facebook, Inc.
 *
 *  This source code is licensed under the MIT license found in the LICENSE
 *  file in the root directory of this source tree.
 *
 */
#include "TestClientServerUtil.h"

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <glog/logging.h>

#include <folly/Conv.h>
#include <folly/fibers/EventBaseLoopController.h>
#include <folly/fibers/FiberManager.h>
#include <folly/io/async/EventBase.h>
#include <wangle/ssl/TLSTicketKeySeeds.h>

#include "mcrouter/lib/IOBufUtil.h"
#include "mcrouter/lib/carbon/RequestReplyUtil.h"
#include "mcrouter/lib/fbi/cpp/util.h"
#include "mcrouter/lib/network/AsyncMcClient.h"
#include "mcrouter/lib/network/AsyncMcServer.h"
#include "mcrouter/lib/network/AsyncMcServerWorker.h"
#include "mcrouter/lib/network/RpcStatsContext.h"
#include "mcrouter/lib/network/SecurityOptions.h"
#include "mcrouter/lib/network/ThreadLocalSSLContextProvider.h"
#include "mcrouter/lib/network/test/ListenSocket.h"

namespace folly {
class AsyncSocket;
} // namespace folly

namespace facebook {
namespace memcache {
namespace test {

const char* const kBrokenKeyPath = "mcrouter/lib/network/test/broken_key.pem";
const char* const kBrokenCertPath = "mcrouter/lib/network/test/broken_cert.pem";

const char* const kInvalidKeyPath = "/do/not/exist";
const char* const kInvalidCertPath = "/do/not/exist";

const char* const kServerVersion = "TestServer-1.0";

SSLTestPaths validClientSsl() {
  return {getDefaultCertPath(), getDefaultKeyPath(), getDefaultCaPath()};
}

SSLTestPaths invalidClientSsl() {
  return {kInvalidCertPath, kInvalidKeyPath, getDefaultCaPath()};
}

SSLTestPaths brokenClientSsl() {
  return {kBrokenCertPath, kBrokenKeyPath, getDefaultCaPath()};
}

SSLTestPaths noCertClientSsl() {
  return {"", "", ""};
}

SSLTestPaths validSsl() {
  // valid client creds will work for the server
  // as well
  return validClientSsl();
}

TestServerOnRequest::TestServerOnRequest(
    folly::fibers::Baton& shutdownLock,
    bool outOfOrder)
    : shutdownLock_(shutdownLock), outOfOrder_(outOfOrder) {}

void TestServerOnRequest::onRequest(
    McServerRequestContext&& ctx,
    McGetRequest&& req) {
  using Reply = McGetReply;

  if (req.key().fullKey() == "sleep") {
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::seconds(1));
    processReply(std::move(ctx), Reply(mc_res_notfound));
  } else if (req.key().fullKey() == "shutdown") {
    shutdownLock_.post();
    processReply(std::move(ctx), Reply(mc_res_notfound));
    flushQueue();
  } else if (req.key().fullKey() == "busy") {
    processReply(std::move(ctx), Reply(mc_res_busy));
  } else {
    std::string value;
    if (req.key().fullKey().startsWith("value_size:")) {
      auto key = req.key().fullKey();
      key.removePrefix("value_size:");
      size_t valSize = folly::to<size_t>(key);
      value = std::string(valSize, 'a');
    } else if (req.key().fullKey() == "trace_id") {
      const auto traceMetadata = req.traceToInts();
      value =
          folly::sformat("{}:{}", traceMetadata.first, traceMetadata.second);
    } else if (req.key().fullKey() != "empty") {
      value = req.key().fullKey().str();
    }

    Reply foundReply(mc_res_found);
    foundReply.value() = folly::IOBuf(folly::IOBuf::COPY_BUFFER, value);

    if (req.key().fullKey() == "hold") {
      waitingReplies_.push_back(
          [ctx = std::move(ctx), reply = std::move(foundReply)]() mutable {
            McServerRequestContext::reply(std::move(ctx), std::move(reply));
          });
    } else if (req.key().fullKey() == "flush") {
      processReply(std::move(ctx), std::move(foundReply));
      flushQueue();
    } else {
      processReply(std::move(ctx), std::move(foundReply));
    }
  }
}

void TestServerOnRequest::onRequest(
    McServerRequestContext&& ctx,
    McSetRequest&&) {
  processReply(std::move(ctx), McSetReply(mc_res_stored));
}

void TestServerOnRequest::onRequest(
    McServerRequestContext&& ctx,
    McVersionRequest&&) {
  McVersionReply reply(mc_res_ok);
  reply.value() = folly::IOBuf(folly::IOBuf::COPY_BUFFER, kServerVersion);
  processReply(std::move(ctx), std::move(reply));
}

void TestServerOnRequest::flushQueue() {
  for (size_t i = 0; i < waitingReplies_.size(); ++i) {
    waitingReplies_[i]();
  }
  waitingReplies_.clear();
}

TestServer::TestServer(Config config)
    : outOfOrder_(config.outOfOrder),
      useTicketKeySeeds_(config.useSsl && config.useTicketKeySeeds),
      onConnectionAcceptedAdditionalCb_(
          std::move(config.onConnectionAcceptedAdditionalCb)) {
  opts_.existingSocketFd = sock_.getSocketFd();
  opts_.numThreads = config.numThreads;
  opts_.worker.defaultVersionHandler = config.useDefaultVersion;
  opts_.worker.maxInFlight = config.maxInflight;
  opts_.worker.sendTimeout = std::chrono::milliseconds{config.timeoutMs};
  opts_.worker.goAwayTimeout =
      std::chrono::milliseconds{config.goAwayTimeoutMs};
  opts_.setMaxConnections(config.maxConns, opts_.numThreads);
  if (config.useSsl) {
    opts_.pemKeyPath = config.keyPath;
    opts_.pemCertPath = config.certPath;
    opts_.pemCaPath = config.caPath;
    opts_.sslRequirePeerCerts = config.requirePeerCerts;
    if (config.tfoEnabled) {
      opts_.tfoEnabledForSsl = true;
      opts_.tfoQueueSize = 100000;
    }
  }
}

void TestServer::run(std::function<void(AsyncMcServerWorker&)> init) {
  LOG(INFO) << "Spawning AsyncMcServer";

  folly::fibers::Baton startupLock;
  serverThread_ = std::thread([this, &startupLock, init] {
    // take ownership of the socket before starting the server as
    // its closed in the server
    sock_.releaseSocketFd();

    server_ = std::make_unique<AsyncMcServer>(opts_);
    if (useTicketKeySeeds_) {
      wangle::TLSTicketKeySeeds seeds{
          .oldSeeds = {std::string(96, 'a')},
          .currentSeeds = {std::string(96, 'b')},
          .newSeeds = {std::string(96, 'c')},
      };
      server_->setTicketKeySeeds(std::move(seeds));
    }
    server_->spawn(
        [this, init](
            size_t, folly::EventBase& evb, AsyncMcServerWorker& worker) {
          init(worker);
          worker.setOnConnectionAccepted([this](McServerSession& session) {
            ++acceptedConns_;
            if (onConnectionAcceptedAdditionalCb_) {
              onConnectionAcceptedAdditionalCb_(session);
            }
          });

          evb.loop();
        });

    // allow server some time to startup
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    startupLock.post();

    shutdownLock_.wait();
    server_->shutdown();
    server_->join();
  });
  startupLock.wait();
}

TestServer::~TestServer() {
  shutdown();
  join();
}

std::string TestServer::version() const {
  if (opts_.worker.defaultVersionHandler) {
    return opts_.worker.versionString;
  } else {
    return kServerVersion;
  }
}

TestClient::TestClient(
    std::string host,
    uint16_t port,
    int timeoutMs,
    mc_protocol_t protocol,
    folly::Optional<SSLTestPaths> ssl,
    uint64_t qosClass,
    uint64_t qosPath,
    std::string serviceIdentity,
    const CompressionCodecMap* compressionCodecMap,
    bool enableTfo,
    bool offloadHandshakes)
    : fm_(std::make_unique<folly::fibers::EventBaseLoopController>()) {
  dynamic_cast<folly::fibers::EventBaseLoopController&>(fm_.loopController())
      .attachEventBase(eventBase_);
  auto mech = ssl ? ssl->mech : SecurityMech::NONE;
  ConnectionOptions opts(host, port, protocol, mech);
  opts.connectTimeout = std::chrono::milliseconds(timeoutMs);
  opts.writeTimeout = std::chrono::milliseconds(timeoutMs);
  opts.compressionCodecMap = compressionCodecMap;
  if (ssl) {
    opts.securityOpts.sslPemCertPath = ssl->sslCertPath;
    opts.securityOpts.sslPemKeyPath = ssl->sslKeyPath;
    opts.securityOpts.sslPemCaPath = ssl->sslCaPath;
    opts.securityOpts.sessionCachingEnabled = true;
    opts.securityOpts.sslServiceIdentity = serviceIdentity;
    opts.securityOpts.tfoEnabledForSsl = enableTfo;
    opts.securityOpts.sslHandshakeOffload = offloadHandshakes;
  }
  if (qosClass != 0 || qosPath != 0) {
    opts.enableQoS = true;
    opts.qosClass = qosClass;
    opts.qosPath = qosPath;
  }
  client_ = std::make_unique<AsyncMcClient>(eventBase_, opts);
  client_->setStatusCallbacks(
      [](const folly::AsyncTransportWrapper&, int64_t) {
        LOG(INFO) << "Client UP.";
      },
      [](AsyncMcClient::ConnectionDownReason reason, int64_t) {
        if (reason == AsyncMcClient::ConnectionDownReason::SERVER_GONE_AWAY) {
          LOG(INFO) << "Server gone Away.";
        } else {
          LOG(INFO) << "Client DOWN.";
        }
      });
  client_->setRequestStatusCallbacks(
      [this](int pendingDiff, int inflightDiff) {
        CHECK(pendingDiff != inflightDiff)
            << "A request can't be pending and inflight at the same time";

        pendingStat_ += pendingDiff;
        inflightStat_ += inflightDiff;

        CHECK(pendingStat_ >= 0 && inflightStat_ >= 0)
            << "Pending and inflight stats should always be 0 or more.";

        pendingStatMax_ = std::max(pendingStatMax_, pendingStat_);
        inflightStatMax_ = std::max(inflightStatMax_, inflightStat_);
      },
      nullptr);
}

void TestClient::setStatusCallbacks(
    std::function<void(const folly::AsyncTransportWrapper&, int64_t)> onUp,
    std::function<void(AsyncMcClient::ConnectionDownReason, int64_t)> onDown) {
  client_->setStatusCallbacks(
      [onUp](
          const folly::AsyncTransportWrapper& socket,
          int64_t numConnectRetries) {
        LOG(INFO) << "Client UP.";
        if (onUp) {
          onUp(socket, numConnectRetries);
        }
      },
      [onDown](
          AsyncMcClient::ConnectionDownReason reason,
          int64_t numConnectRetries) {
        if (reason == AsyncMcClient::ConnectionDownReason::SERVER_GONE_AWAY) {
          LOG(INFO) << "Server gone Away.";
        } else {
          LOG(INFO) << "Client DOWN.";
        }
        if (onDown) {
          onDown(reason, numConnectRetries);
        }
      });
}

void TestClient::sendGet(
    std::string key,
    mc_res_t expectedResult,
    uint32_t timeoutMs,
    std::function<void(const RpcStatsContext&)> rpcStatsCallback) {
  inflight_++;
  fm_.addTask([key = std::move(key),
               expectedResult,
               rpcStatsCallback = std::move(rpcStatsCallback),
               this,
               timeoutMs]() {
    McGetRequest req(key);
    if (req.key().fullKey() == "trace_id") {
      req.setTraceId({12345, 67890});
    }

    try {
      RpcStatsContext rpcStatsContext;
      auto reply = client_->sendSync(
          req, std::chrono::milliseconds(timeoutMs), &rpcStatsContext);
      if (rpcStatsCallback) {
        rpcStatsCallback(rpcStatsContext);
      }

      if (reply.result() == mc_res_found) {
        auto value = carbon::valueRangeSlow(reply);
        if (req.key().fullKey() == "empty") {
          checkLogic(value.empty(), "Expected empty value, got {}", value);
        } else if (req.key().fullKey().startsWith("value_size:")) {
          auto key = req.key().fullKey();
          key.removePrefix("value_size:");
          size_t valSize = folly::to<size_t>(key);
          checkLogic(
              value.size() == valSize,
              "Expected value of size {}, got {}",
              valSize,
              value.size());
        } else if (req.key().fullKey() == "trace_id") {
          checkLogic(
              value == "12345:67890",
              "Expected value to equal trace ID {}, got {}",
              "12345:67890",
              value);
        } else {
          checkLogic(
              value == req.key().fullKey(),
              "Expected {}, got {}",
              req.key().fullKey(),
              value);
        }
      }
      checkLogic(
          expectedResult == reply.result(),
          "Expected {}, got {} for key '{}'. Reply message: {}",
          mc_res_to_string(expectedResult),
          mc_res_to_string(reply.result()),
          req.key().fullKey(),
          carbon::getMessage(reply));
    } catch (const std::exception& e) {
      CHECK(false) << "Failed: " << e.what();
    }
    inflight_--;
  });
}

void TestClient::sendSet(
    std::string key,
    std::string value,
    mc_res_t expectedResult,
    uint32_t timeoutMs,
    std::function<void(const RpcStatsContext&)> rpcStatsCallback) {
  inflight_++;
  fm_.addTask([key = std::move(key),
               value = std::move(value),
               expectedResult,
               rpcStatsCallback = std::move(rpcStatsCallback),
               this,
               timeoutMs]() {
    McSetRequest req(key);
    req.value() = folly::IOBuf::wrapBufferAsValue(folly::StringPiece(value));

    RpcStatsContext rpcStatsContext;
    auto reply = client_->sendSync(
        req, std::chrono::milliseconds(timeoutMs), &rpcStatsContext);
    if (rpcStatsCallback) {
      rpcStatsCallback(rpcStatsContext);
    }

    CHECK(expectedResult == reply.result())
        << "Expected: " << mc_res_to_string(expectedResult) << " got "
        << mc_res_to_string(reply.result())
        << ". Reply message: " << carbon::getMessage(reply);

    inflight_--;
  });
}

void TestClient::sendVersion(std::string expectedVersion) {
  ++inflight_;
  fm_.addTask([this, expectedVersion = std::move(expectedVersion)]() {
    McVersionRequest req;

    auto reply = client_->sendSync(req, std::chrono::milliseconds(200));

    CHECK_EQ(mc_res_ok, reply.result())
        << "Expected result " << mc_res_to_string(mc_res_ok) << ", got "
        << mc_res_to_string(reply.result());

    CHECK_EQ(expectedVersion, carbon::valueRangeSlow(reply))
        << "Expected version " << expectedVersion << ", got "
        << carbon::valueRangeSlow(reply);

    --inflight_;
  });
}

void TestClient::waitForReplies(size_t remaining) {
  while (inflight_ > remaining) {
    loopOnce();
  }
  if (remaining == 0) {
    CHECK(pendingStat_ == 0) << "pendingStat_ should be 0";
    CHECK(inflightStat_ == 0) << "inflightStat_ should be 0";
  }
}

std::string genBigValue() {
  const size_t kBigValueSize = 1024 * 1024 * 16;
  std::string bigValue(kBigValueSize, '.');
  for (size_t i = 0; i < kBigValueSize; ++i) {
    bigValue[i] = 65 + (i % 26);
  }
  return bigValue;
}
} // namespace test
} // namespace memcache
} // namespace facebook
