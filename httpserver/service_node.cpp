#include "service_node.h"

#include "Database.hpp"
#include "Item.hpp"
#include "http.h"
#include "http_connection.h"
#include "https_client.h"
#include "omq_server.h"
#include "oxen_common.h"
#include "oxen_logger.h"
#include "oxend_key.h"
#include "serialization.h"
#include "signature.h"
#include "string_utils.hpp"
#include "utils.hpp"
#include "version.h"
#include <nlohmann/json.hpp>
#include <oxenmq/base32z.h>
#include <oxenmq/base64.h>
#include <oxenmq/hex.h>
#include <oxenmq/oxenmq.h>

#include "request_handler.h"

#include <algorithm>
#include <chrono>
#include <string_view>

#include <boost/bind/bind.hpp>
#include <boost/endian/conversion.hpp>

using json = nlohmann::json;

namespace oxen {

using storage::Item;

constexpr std::chrono::milliseconds RELAY_INTERVAL = 350ms;

// Threshold of missing data records at which we start warning and consult bootstrap nodes (mainly
// so that we don't bother producing warning spam or going to the bootstrap just for a few new nodes
// that will often have missing info for a few minutes).
using MISSING_PUBKEY_THRESHOLD = std::ratio<3, 100>;

static void make_sn_request(boost::asio::io_context& ioc, const sn_record_t& sn,
                            std::shared_ptr<request_t> req,
                            http_callback_t&& cb) {
    OXEN_LOG(trace, "make sn_request to {} @ {}:{}", sn.pubkey_legacy, sn.ip, sn.port);
    // TODO: Return to using snode address instead of ip
    make_https_request_to_sn(ioc, sn, std::move(req), std::move(cb));
}

/// TODO: there should be config.h to store constants like these
constexpr std::chrono::seconds STATS_CLEANUP_INTERVAL = 60min;
constexpr std::chrono::seconds OXEND_PING_INTERVAL = 30s;
constexpr int CLIENT_RETRIEVE_MESSAGE_LIMIT = 100;

ServiceNode::ServiceNode(
        sn_record_t address,
        const legacy_seckey& skey,
        OxenmqServer& omq_server,
        const std::filesystem::path& db_location,
        const bool force_start) :
      force_start_{force_start},
      db_{std::make_unique<Database>(db_location)},
      our_address_{std::move(address)},
      our_seckey_{skey},
      omq_server_{omq_server} {

    swarm_ = std::make_unique<Swarm>(our_address_);

    OXEN_LOG(info, "Requesting initial swarm state");

#ifdef INTEGRATION_TEST
    this->syncing_ = false;
#endif

    omq_server->add_timer([this] { std::lock_guard l{sn_mutex_}; db_->clean_expired(); },
            Database::CLEANUP_PERIOD);

    omq_server_->add_timer([this] { std::lock_guard l{sn_mutex_}; all_stats_.cleanup(); },
            STATS_CLEANUP_INTERVAL);

    // boost asio's event loop exits immediately if there isn't an active job on it.  The "solution"
    // is to put this fake work object on it.  Yuck.
    ioc_fake_work_ = std::make_unique<boost::asio::io_service::work>(ioc_);
    ioc_thread_ = std::thread{[this] { ioc_.run(); }};

    // We really want to make sure nodes don't get stuck in "syncing" mode,
    // so if we are still "syncing" after a long time, activate SN regardless
    auto delay_timer = std::make_shared<boost::asio::steady_timer>(ioc_);

    delay_timer->expires_after(std::chrono::minutes(60));
    delay_timer->async_wait([this, delay_timer](const auto& /*ec*/) {
        std::lock_guard lock{sn_mutex_};
        if (!syncing_)
            return;
        OXEN_LOG(warn, "Block syncing is taking too long, activating SS regardless");
        syncing_ = false;
    });
}

void ServiceNode::on_oxend_connected() {
    update_swarms();
    oxend_ping();
    omq_server_->add_timer([this] { oxend_ping(); }, OXEND_PING_INTERVAL);
    omq_server_->add_timer([this] { ping_peers(); },
            reachability_testing::TESTING_TIMER_INTERVAL);
    omq_server_->add_timer([this] { relay_buffered_messages(); },
            RELAY_INTERVAL);
}

static block_update_t
parse_swarm_update(const std::string& response_body, bool from_json_rpc = false) {

    if (response_body.empty()) {
        OXEN_LOG(critical, "Bad oxend rpc response: no response body");
        throw std::runtime_error("Failed to parse swarm update");
    }

    std::map<swarm_id_t, std::vector<sn_record_t>> swarm_map;
    block_update_t bu;

    OXEN_LOG(trace, "swarm repsonse: <{}>", response_body);

    try {
        json result = json::parse(response_body, nullptr, true);
        if (from_json_rpc)
            result = result.at("result");

        bu.height = result.at("height").get<uint64_t>();
        bu.block_hash = result.at("block_hash").get<std::string>();
        bu.hardfork = result.at("hardfork").get<int>();
        bu.unchanged =
            result.count("unchanged") && result.at("unchanged").get<bool>();
        if (bu.unchanged)
            return bu;

        const json service_node_states = result.at("service_node_states");

        int missing_aux_pks = 0, total = 0;

        for (const auto& sn_json : service_node_states) {
            /// We want to include (test) decommissioned nodes, but not
            /// partially funded ones.
            if (!sn_json.at("funded").get<bool>()) {
                continue;
            }

            total++;
            const auto& pk_hex = sn_json.at("service_node_pubkey").get_ref<const std::string&>();
            const auto& pk_x25519_hex =
                sn_json.at("pubkey_x25519").get_ref<const std::string&>();
            const auto& pk_ed25519_hex =
                sn_json.at("pubkey_ed25519").get_ref<const std::string&>();

            if (pk_x25519_hex.empty() || pk_ed25519_hex.empty()) {
                // These will always either both be present or neither present.  If they are missing
                // there isn't much we can do: it means the remote hasn't transmitted them yet (or
                // our local oxend hasn't received them yet).
                missing_aux_pks++;
                OXEN_LOG(debug, "ed25519/x25519 pubkeys are missing from service node info {}", pk_hex);
                continue;
            }

            auto sn = sn_record_t{
                sn_json.at("public_ip").get_ref<const std::string&>(),
                sn_json.at("storage_port").get<uint16_t>(),
                sn_json.at("storage_lmq_port").get<uint16_t>(),
                legacy_pubkey::from_hex(pk_hex),
                ed25519_pubkey::from_hex(pk_ed25519_hex),
                x25519_pubkey::from_hex(pk_x25519_hex)};

            const swarm_id_t swarm_id =
                sn_json.at("swarm_id").get<swarm_id_t>();

            /// Storing decommissioned nodes (with dummy swarm id) in
            /// a separate data structure as it seems less error prone
            if (swarm_id == INVALID_SWARM_ID) {
                bu.decommissioned_nodes.push_back(std::move(sn));
            } else {
                bu.active_x25519_pubkeys.emplace(sn.pubkey_x25519.view());

                swarm_map[swarm_id].push_back(std::move(sn));
            }
        }

        if (missing_aux_pks >
                MISSING_PUBKEY_THRESHOLD::num*total/MISSING_PUBKEY_THRESHOLD::den) {
            OXEN_LOG(warn, "Missing ed25519/x25519 pubkeys for {}/{} service nodes; "
                    "oxend may be out of sync with the network", missing_aux_pks, total);
        }

    } catch (const std::exception& e) {
        OXEN_LOG(critical, "Bad oxend rpc response: invalid json ({})", e.what());
        throw std::runtime_error("Failed to parse swarm update");
    }

    for (auto const& swarm : swarm_map) {
        bu.swarms.emplace_back(SwarmInfo{swarm.first, swarm.second});
    }

    return bu;
}

void ServiceNode::bootstrap_data() {

    std::lock_guard guard(sn_mutex_);

    OXEN_LOG(trace, "Bootstrapping peer data");

    json params{
        {"fields", {
            {"service_node_pubkey", true},
            {"swarm_id", true},
            {"storage_port", true},
            {"public_ip", true},
            {"height", true},
            {"block_hash", true},
            {"hardfork", true},
            {"funded", true},
            {"pubkey_x25519", true},
            {"pubkey_ed25519", true},
            {"storage_lmq_port", true}
        }}
    };

    std::vector<std::pair<std::string, uint16_t>> seed_nodes;
    if (oxen::is_mainnet) {
        seed_nodes = {{{"public.loki.foundation", 22023},
                       {"storage.seed1.loki.network", 22023},
                       {"storage.seed3.loki.network", 22023},
                       {"imaginary.stream", 22023}}};
    } else {
        seed_nodes = {{{"public.loki.foundation", 38157}}};
    }

    auto req_counter = std::make_shared<size_t>(0);

    for (const auto& [addr, port] : seed_nodes) {
        oxend_json_rpc_request(
            ioc_, addr, port, "get_service_nodes", params,
            [this, addr=addr, req_counter, node_count = seed_nodes.size()]
            (sn_response_t&& res) {
                if (res.error_code == SNodeError::NO_ERROR && res.body) {
                    OXEN_LOG(info, "Parsing response from seed {}", addr);
                    try {
                        block_update_t bu = parse_swarm_update(*res.body, true);

                        // TODO: this should be disabled in the "testnet" mode
                        // (or changed to point to testnet seeds)
                        if (!bu.unchanged) {
                            this->on_bootstrap_update(std::move(bu));
                        }

                        OXEN_LOG(info, "Bootstrapped from {}", addr);
                    } catch (const std::exception& e) {
                        OXEN_LOG(
                            err,
                            "Exception caught while bootstrapping from {}: {}",
                            addr, e.what());
                    }
                } else {
                    OXEN_LOG(err, "Failed to contact bootstrap node {}", addr);
                }

                (*req_counter)++;

                if (*req_counter == node_count) {
                    OXEN_LOG(info, "Bootstrapping done");
                    if (this->target_height_ > 0) {
                        update_swarms();
                    } else {
                        // If target height is still 0 after having contacted
                        // (successfully or not) all seed nodes, just assume we have
                        // finished syncing. (Otherwise we will never get a chance
                        // to update syncing status.)
                        OXEN_LOG(
                            warn,
                            "Could not contact any of the seed nodes to get target "
                            "height. Going to assume our height is correct.");
                        this->syncing_ = false;
                    }
                }
            });
    }
}

void ServiceNode::shutdown() {
    shutting_down_ = true;
    ioc_fake_work_.reset();
    ioc_.stop();
    if (ioc_thread_.joinable())
        ioc_thread_.join();
}

bool ServiceNode::snode_ready(std::string* reason) {
    if (shutting_down()) {
        if (reason) *reason = "shutting down";
        return false;
    }

    std::lock_guard guard(sn_mutex_);

    std::vector<std::string> problems;

    if (!hf_at_least(STORAGE_SERVER_HARDFORK))
        problems.push_back("not yet on hardfork " + std::to_string(STORAGE_SERVER_HARDFORK));
    if (!swarm_ || !swarm_->is_valid())
        problems.push_back("not in any swarm");
    if (syncing_)
        problems.push_back("not done syncing");

    if (reason)
        *reason = util::join("; ", problems);

    return problems.empty() || force_start_;
}

void ServiceNode::send_onion_to_sn(const sn_record_t& sn,
                                   std::string_view payload,
                                   OnionRequestMetadata&& data,
                                   ss_client::Callback cb) const {

    if (!hf_at_least(HARDFORK_OMQ_ONION_REQ_BENCODE)) {
        // use the _v2 endpoint up until the hf:
        omq_server_->request(
            sn.pubkey_x25519.view(), "sn.onion_req_v2", std::move(cb),
            oxenmq::send_option::request_timeout{30s}, data.ephem_key.hex(), payload);
    } else {
        // Use the newer (v3, I suppose, though it's internal) where we bencode everything (which is
        // a bit more compact than sending the eph_key in hex, plus allows other metadata such as
        // the hop number and the encryption type).
        data.hop_no++;
        omq_server_->request(
            sn.pubkey_x25519.view(), "sn.onion_request", std::move(cb),
            oxenmq::send_option::request_timeout{30s},
            omq_server_.encode_onion_data(payload, data));
    }
}

// Calls callback on success only?
void ServiceNode::send_to_sn(const sn_record_t& sn, ss_client::ReqMethod method,
                             Request req, ss_client::Callback cb) const {

    std::lock_guard guard(sn_mutex_);

    switch (method) {
    case ss_client::ReqMethod::DATA: {
        OXEN_LOG(debug, "Sending sn.data request to {}",
                 oxenmq::to_hex(sn.pubkey_x25519.view()));
        omq_server_->request(sn.pubkey_x25519.view(), "sn.data", std::move(cb),
                             req.body);
        break;
    }
    case ss_client::ReqMethod::PROXY_EXIT: {
        auto client_key = req.headers.find(http::SENDER_KEY_HEADER);

        // I could just always assume that we are passing the right
        // parameters...
        if (client_key != req.headers.end()) {
            OXEN_LOG(debug, "Sending sn.proxy_exit request to {}",
                     oxenmq::to_hex(sn.pubkey_x25519.view()));
            omq_server_->request(sn.pubkey_x25519.view(), "sn.proxy_exit",
                                 std::move(cb), client_key->second, req.body);
        } else {
            OXEN_LOG(debug, "Developer error: no {} passed in headers",
                     http::SENDER_KEY_HEADER);
            // TODO: call cb?
            assert(false);
        }
        break;
    }
    case ss_client::ReqMethod::ONION_REQUEST: {
        // Onion reqeusts always use oxenmq, so they use it
        // directly, no need for the "send_to_sn" abstraction
        OXEN_LOG(err, "Onion requests should not use this interface");
        assert(false);
        break;
    }
    }
}

void ServiceNode::relay_data_reliable(const std::string& blob,
                                      const sn_record_t& sn) const {

    OXEN_LOG(debug, "Relaying data to: {}", sn.pubkey_legacy);

    send_to_sn(sn, ss_client::ReqMethod::DATA, Request{blob},
            [](bool success, auto&& data) {
                if (!success) OXEN_LOG(err, "Failed to send batch data: time-out");
            });
}

void ServiceNode::record_proxy_request() { all_stats_.bump_proxy_requests(); }

void ServiceNode::record_onion_request() { all_stats_.bump_onion_requests(); }

/// do this asynchronously on a different thread? (on the same thread?)
bool ServiceNode::process_store(const message_t& msg) {

    std::lock_guard guard(sn_mutex_);

    /// only accept a message if we are in a swarm
    if (!swarm_) {
        // This should never be printed now that we have "snode_ready"
        OXEN_LOG(err, "error: my swarm in not initialized");
        return false;
    }

    all_stats_.bump_store_requests();

    /// store in the database
    this->save_if_new(msg);

    // Instead of sending the messages immediatly, store them in a buffer
    // and periodically send all messages from there as batches
    this->relay_buffer_.push_back(msg);

    return true;
}

void ServiceNode::save_if_new(const message_t& msg) {

    std::lock_guard guard(sn_mutex_);

    if (db_->store(msg.hash, msg.pub_key, msg.data, msg.ttl, msg.timestamp,
                   msg.nonce)) {
        OXEN_LOG(trace, "saved message: {}", msg.data);
    }
}

void ServiceNode::save_bulk(const std::vector<Item>& items) {

    std::lock_guard guard(sn_mutex_);

    if (!db_->bulk_store(items)) {
        OXEN_LOG(err, "failed to save batch to the database");
        return;
    }

    OXEN_LOG(trace, "saved messages count: {}", items.size());
}

void ServiceNode::on_bootstrap_update(block_update_t&& bu) {

    // Used in a callback to needs a mutex even if it is private
    std::lock_guard guard(sn_mutex_);

    swarm_->apply_swarm_changes(bu.swarms);
    target_height_ = std::max(target_height_, bu.height);

    if (syncing_)
        omq_server_->set_active_sns(std::move(bu.active_x25519_pubkeys));
}

template <typename OStream>
OStream& operator<<(OStream& os, const SnodeStatus& status) {
    switch (status) {
    case SnodeStatus::UNSTAKED:
        return os << "Unstaked";
    case SnodeStatus::DECOMMISSIONED:
        return os << "Decommissioned";
    case SnodeStatus::ACTIVE:
        return os << "Active";
    default:
        return os << "Unknown";
    }
}

static SnodeStatus derive_snode_status(const block_update_t& bu,
                                       const sn_record_t& our_address) {

    // TODO: try not to do this again in `derive_swarm_events`
    const auto our_swarm_it =
        std::find_if(bu.swarms.begin(), bu.swarms.end(),
                     [&our_address](const SwarmInfo& swarm_info) {
                         const auto& snodes = swarm_info.snodes;
                         return std::find(snodes.begin(), snodes.end(),
                                          our_address) != snodes.end();
                     });

    if (our_swarm_it != bu.swarms.end()) {
        return SnodeStatus::ACTIVE;
    }

    if (std::find(bu.decommissioned_nodes.begin(),
                  bu.decommissioned_nodes.end(),
                  our_address) != bu.decommissioned_nodes.end()) {
        return SnodeStatus::DECOMMISSIONED;
    }

    return SnodeStatus::UNSTAKED;
}

void ServiceNode::on_swarm_update(block_update_t&& bu) {

    if (this->hardfork_ != bu.hardfork) {
        OXEN_LOG(debug, "New hardfork: {}", bu.hardfork);
        hardfork_ = bu.hardfork;
    }

    if (syncing_ && target_height_ != 0) {
        syncing_ = bu.height < target_height_;
    }

    /// We don't have anything to do until we have synced
    if (syncing_) {
        OXEN_LOG(debug, "Still syncing: {}/{}", bu.height, target_height_);
        // Note that because we are still syncing, we won't update our swarm id
        return;
    }

    if (bu.block_hash != block_hash_) {

        OXEN_LOG(debug, "new block, height: {}, hash: {}", bu.height,
                 bu.block_hash);

        if (bu.height > block_height_ + 1 && block_height_ != 0) {
            OXEN_LOG(warn, "Skipped some block(s), old: {} new: {}",
                     block_height_, bu.height);
            /// TODO: if we skipped a block, should we try to run peer tests for
            /// them as well?
        } else if (bu.height <= block_height_) {
            // TODO: investigate how testing will be affected under reorg
            OXEN_LOG(warn,
                     "new block height is not higher than the current height");
        }

        block_height_ = bu.height;
        block_hash_ = bu.block_hash;

        block_hashes_cache_.push_back(std::make_pair(bu.height, bu.block_hash));

    } else {
        OXEN_LOG(trace, "already seen this block");
        return;
    }

    omq_server_->set_active_sns(std::move(bu.active_x25519_pubkeys));

    const SwarmEvents events = swarm_->derive_swarm_events(bu.swarms);

    // TODO: check our node's state

    const auto status = derive_snode_status(bu, our_address_);

    if (this->status_ != status) {
        OXEN_LOG(info, "Node status updated: {}", status);
        this->status_ = status;
    }

    swarm_->set_swarm_id(events.our_swarm_id);

    if (std::string reason; !snode_ready(&reason)) {
        OXEN_LOG(warn, "Storage server is still not ready: {}", reason);
        swarm_->update_state(bu.swarms, bu.decommissioned_nodes, events, false);
        return;
    } else {
        if (!active_) {
            // NOTE: because we never reset `active_` after we get
            // decommissioned, this code won't run when the node comes back
            // again
            OXEN_LOG(info, "Storage server is now active!");
            active_ = true;
        }
    }

    swarm_->update_state(bu.swarms, bu.decommissioned_nodes, events, true);

    if (!events.new_snodes.empty()) {
        this->bootstrap_peers(events.new_snodes);
    }

    if (!events.new_swarms.empty()) {
        this->bootstrap_swarms(events.new_swarms);
    }

    if (events.dissolved) {
        /// Go through all our PK and push them accordingly
        this->salvage_data();
    }

#ifndef INTEGRATION_TEST
    this->initiate_peer_test();
#endif
}

void ServiceNode::relay_buffered_messages() {

    std::lock_guard guard(sn_mutex_);

    if (relay_buffer_.empty())
        return;

    OXEN_LOG(debug, "Relaying {} messages from buffer to {} nodes",
             relay_buffer_.size(), swarm_->other_nodes().size());

    this->relay_messages(relay_buffer_, swarm_->other_nodes());
    relay_buffer_.clear();
}

void ServiceNode::update_swarms() {

    if (updating_swarms_.exchange(true)) {
        OXEN_LOG(debug, "Swarm update already in progress, not sending another update request");
        return;
    }

    std::lock_guard guard(sn_mutex_);

    OXEN_LOG(debug, "Swarm update triggered");

    json params{
        {"fields", {
            {"service_node_pubkey", true},
            {"swarm_id", true},
            {"storage_port", true},
            {"public_ip", true},
            {"height", true},
            {"block_hash", true},
            {"hardfork", true},
            {"funded", true},
            {"pubkey_x25519", true},
            {"pubkey_ed25519", true},
            {"storage_lmq_port", true}
        }},
        {"active_only", false}
    };
    if (!got_first_response_ && !block_hash_.empty())
        params["poll_block_hash"] = block_hash_;

    omq_server_.oxend_request("rpc.get_service_nodes",
        [this](bool success, std::vector<std::string> data) {
            updating_swarms_ = false;
            if (!success || data.size() < 2) {
                OXEN_LOG(critical, "Failed to contact local oxend for service node list");
                return;
            }
            try {
                std::lock_guard guard(sn_mutex_);
                block_update_t bu = parse_swarm_update(data[1]);
                if (!got_first_response_) {
                    OXEN_LOG(
                        info,
                        "Got initial swarm information from local Oxend");
                    got_first_response_ = true;

#ifndef INTEGRATION_TEST
                    // If this is our very first response then we *may* want to try falling back to
                    // the bootstrap node *if* our response looks sparse: this will typically happen
                    // for a fresh service node because IP/port distribution through the network can
                    // take up to an hour.  We don't really want to hit the bootstrap nodes when we
                    // don't have to, though, so only do it if our responses is missing more than 3%
                    // of proof data (IPs/ports/ed25519/x25519 pubkeys) or we got back fewer than
                    // 100 SNs (10 on testnet).
                    //
                    // (In the future it would be nice to eliminate this by putting all the required
                    // data on chain, and get rid of needing to consult bootstrap nodes: but
                    // currently we still need this to deal with the lag).

                    auto [missing, total] = count_missing_data(bu);
                    if (total >= (oxen::is_mainnet ? 100 : 10)
                            && missing <=
                                MISSING_PUBKEY_THRESHOLD::num*total/MISSING_PUBKEY_THRESHOLD::den) {
                        OXEN_LOG(info, "Initialized from oxend with {}/{} SN records",
                                total-missing, total);
                        syncing_ = false;
                    } else {
                        OXEN_LOG(info, "Detected some missing SN data ({}/{}); "
                                "querying bootstrap nodes for help", missing, total);
                        this->bootstrap_data();
                    }
#endif
                }

                if (!bu.unchanged) {
                    OXEN_LOG(debug, "Blockchain updated, rebuilding swarm list");
                    on_swarm_update(std::move(bu));
                }
            } catch (const std::exception& e) {
                OXEN_LOG(err, "Exception caught on swarm update: {}",
                         e.what());
            }
        },
        params.dump()
    );
}

void ServiceNode::update_last_ping(ReachType type) {
    reach_records_.incoming_ping(type);
}

void ServiceNode::ping_peers() {

    std::lock_guard lock{sn_mutex_};

    // TODO: Don't do anything until we are fully funded

    if (this->status_ == SnodeStatus::UNSTAKED ||
        this->status_ == SnodeStatus::UNKNOWN) {
        OXEN_LOG(trace, "Skipping peer testing (unstaked)");
        return;
    }

    auto now = std::chrono::steady_clock::now();

    // Check if we've been tested (reached) recently ourselves
    reach_records_.check_incoming_tests(now);

    if (this->status_ == SnodeStatus::DECOMMISSIONED) {
        OXEN_LOG(trace, "Skipping peer testing (decommissioned)");
        return;
    }

    /// We always test nodes due to be tested plus one general, non-failing node.

    auto to_test = reach_records_.get_failing(*swarm_, now);
    if (auto rando = reach_records_.next_random(*swarm_, now))
        to_test.emplace_back(std::move(*rando), 0);

    if (to_test.empty())
        OXEN_LOG(trace, "no nodes to test this tick");
    else
        OXEN_LOG(debug, "{} nodes to test", to_test.size());
    for (const auto& [sn, prev_fails] : to_test)
        test_reachability(sn, prev_fails);
}

void ServiceNode::sign_request(request_t& req) const {
    // TODO: investigate why we are not signing headers
    const auto hash = hash_data(req.body());
    const auto signature = generate_signature(hash, {our_address_.pubkey_legacy, our_seckey_});
    attach_signature(req, signature);
}

void ServiceNode::test_reachability(const sn_record_t& sn, int previous_failures) {

    OXEN_LOG(debug, "Testing {} SN {} for reachability",
            previous_failures > 0 ? "previously failing" : "random",
            sn.pubkey_legacy);

    static constexpr uint8_t TEST_WAITING = 0, TEST_FAILED = 1, TEST_PASSED = 2;

    // We start off two separate tests below; they share this pair and use the atomic int here to
    // figure out whether they were called first (in which case they do nothing) or second (in which
    // case they have to report the final result to oxend).
    auto test_results = std::make_shared<std::pair<const sn_record_t, std::atomic<uint8_t>>>(
            sn, 0);

    auto http_callback = [this, test_results, previous_failures](sn_response_t&& res) {
        auto& [sn, result] = *test_results;

        const bool success = res.error_code == SNodeError::NO_ERROR;
        OXEN_LOG(debug, "{} response for HTTP ping test of {}",
                success ? "Successful" : "FAILED", sn.pubkey_legacy);

        if (result.exchange(success ? TEST_PASSED : TEST_FAILED) != TEST_WAITING)
            report_reachability(sn, success && result == TEST_PASSED, previous_failures);
    };
    auto req = build_post_request(sn.pubkey_ed25519, "/swarms/ping_test/v1", "{}");
    this->sign_request(*req);
    make_sn_request(ioc_, sn, std::move(req), std::move(http_callback));

    // test omq port:
    // TODO: remove the backwards compat endpoint alternative ternary after HF18
    omq_server_->request(
        sn.pubkey_x25519.view(), hf_at_least(HARDFORK_SN_PING) ? "sn.ping" : "sn.onion_req",
        [this, test_results=std::move(test_results), previous_failures](bool success, const auto&) {
            auto& [sn, result] = *test_results;

            OXEN_LOG(debug, "{} response for OxenMQ ping test of {}",
                    success ? "Successful" : "FAILED", sn.pubkey_legacy);

            if (result.exchange(success ? TEST_PASSED : TEST_FAILED) != TEST_WAITING)
                report_reachability(sn, success && result == TEST_PASSED, previous_failures);
        },
        "ping", // TODO: remove this after HF18 (it is just ignored by sn.ping)
        // Only use an existing (or new) outgoing connection:
        oxenmq::send_option::outgoing{});
}

void ServiceNode::oxend_ping() {

    std::lock_guard guard(sn_mutex_);

    /// TODO: Note that this is not actually an SN response! (but Oxend)
    json params{
        {"version", STORAGE_SERVER_VERSION},
        {"https_port", our_address_.port},
        {"omq_port", our_address_.omq_port}};

    omq_server_.oxend_request("admin.storage_server_ping",
        [this](bool success, std::vector<std::string> data) {
            if (!success)
                OXEN_LOG(critical, "Could not ping oxend: Request failed ({})", data.front());
            else if (data.size() < 2 || data[1].empty())
                OXEN_LOG(critical, "Could not ping oxend: Empty body on reply");
            else
                try {
                    if (const auto status = json::parse(data[1]).at("status").get<std::string>();
                            status == "OK") {
                        auto good_pings = ++oxend_pings_;
                        if (good_pings == 1) // First ping after startup or after ping failure
                            OXEN_LOG(info, "Successfully pinged oxend");
                        else if (good_pings % (1h / OXEND_PING_INTERVAL) == 0) // Once an hour
                            OXEN_LOG(info, "{} successful oxend pings", good_pings);
                        else
                            OXEN_LOG(debug, "Successfully pinged Oxend ({} consecutive times)", good_pings);
                    } else {
                        OXEN_LOG(critical, "Could not ping oxend: {}", status);
                        oxend_pings_ = 0;
                    }
                } catch (...) {
                    OXEN_LOG(critical, "Could not ping oxend: bad json in response");
                }
        },
        params.dump()
    );

    // Also re-subscribe (or subscribe, in case oxend restarted) to block subscriptions.  This makes
    // oxend start firing notify.block messages at as whenever new blocks arrive, but we have to
    // renew the subscription within 30min to keep it alive, so do it here (it doesn't hurt anything
    // for it to be much faster than 30min).
    omq_server_.oxend_request("sub.block", [](bool success, auto&& result) {
        if (!success || result.empty())
            OXEN_LOG(critical, "Failed to subscribe to oxend block notifications: {}",
                    result.empty() ? "response is empty" : result.front());
        else if (result.front() == "OK")
            OXEN_LOG(info, "Subscribed to oxend new block notifications");
        else if (result.front() == "ALREADY")
            OXEN_LOG(debug, "Renewed oxend new block notification subscription");
    });
}

void ServiceNode::attach_signature(request_t& request,
                                   const signature& sig) const {

    std::string raw_sig;
    raw_sig.reserve(sig.c.size() + sig.r.size());
    raw_sig.insert(raw_sig.begin(), sig.c.begin(), sig.c.end());
    raw_sig.insert(raw_sig.end(), sig.r.begin(), sig.r.end());

    const std::string sig_b64 = oxenmq::to_base64(raw_sig);
    request.set(http::SNODE_SIGNATURE_HEADER, sig_b64);

    request.set(http::SNODE_SENDER_HEADER,
                 oxenmq::to_base32z(our_address_.pubkey_legacy.view()));
}

void ServiceNode::process_storage_test_response(const sn_record_t& testee,
                                                const Item& item,
                                                uint64_t test_height,
                                                sn_response_t&& res) {

    std::lock_guard guard(sn_mutex_);

    if (res.error_code != SNodeError::NO_ERROR) {
        // TODO: retry here, otherwise tests sometimes fail (when SN not
        // running yet)
        this->all_stats_.record_storage_test_result(testee.pubkey_legacy, ResultType::OTHER);
        OXEN_LOG(debug, "Failed to send a storage test request to snode: {}",
                 testee.pubkey_legacy);
        return;
    }

    // If we got here, the response is 200 OK, but we still need to check
    // status in response body and check the answer
    if (!res.body) {
        this->all_stats_.record_storage_test_result(testee.pubkey_legacy, ResultType::OTHER);
        OXEN_LOG(debug, "Empty body in storage test response");
        return;
    }

    ResultType result = ResultType::OTHER;

    try {

        json res_json = json::parse(*res.body);

        const auto status = res_json.at("status").get<std::string>();

        if (status == "OK") {

            const auto value = res_json.at("value").get<std::string>();
            if (value == item.data) {
                OXEN_LOG(debug,
                         "Storage test is successful for: {} at height: {}",
                         testee.pubkey_legacy, test_height);
                result = ResultType::OK;
            } else {
                OXEN_LOG(debug,
                         "Test answer doesn't match for: {} at height {}",
                         testee.pubkey_legacy, test_height);
#ifdef INTEGRATION_TEST
                OXEN_LOG(warn, "got: {} expected: {}", value, item.data);
#endif
                result = ResultType::MISMATCH;
            }

        } else if (status == "wrong request") {
            OXEN_LOG(debug, "Storage test rejected by testee");
            result = ResultType::REJECTED;
        } else {
            result = ResultType::OTHER;
            OXEN_LOG(debug, "Storage test failed for some other reason");
        }
    } catch (...) {
        result = ResultType::OTHER;
        OXEN_LOG(debug, "Invalid json in storage test response");
    }

    this->all_stats_.record_storage_test_result(testee.pubkey_legacy, result);
}

void ServiceNode::send_storage_test_req(const sn_record_t& testee,
                                        uint64_t test_height,
                                        const Item& item) {

    // Used as a callback to needs a mutex even if it is private
    std::lock_guard guard(sn_mutex_);

    nlohmann::json json_body;

    json_body["height"] = test_height;
    json_body["hash"] = item.hash;

    auto req = build_post_request(testee.pubkey_ed25519, "/swarms/storage_test/v1", json_body.dump());

    this->sign_request(*req);

    make_sn_request(ioc_, testee, req,
                    [testee, item, height = this->block_height_,
                     this](sn_response_t&& res) {
                        this->process_storage_test_response(
                            testee, item, height, std::move(res));
                    });
}

void ServiceNode::report_reachability(const sn_record_t& sn, bool reachable, int previous_failures) {
    auto cb = [sn_pk=sn.pubkey_legacy, reachable](bool success, std::vector<std::string> data) {
        if (!success) {
            OXEN_LOG(warn, "Could not report node status: {}",
                    data.empty() ? "unknown reason" : data[0]);
            return;
        }

        if (data.size() < 2 || data[1].empty()) {
            OXEN_LOG(warn, "Empty body on Oxend report node status");
            return;
        }

        try {
            const auto status = json::parse(data[1]).at("status").get<std::string>();

            if (status == "OK") {
                OXEN_LOG(debug, "Successfully reported {} node: {}",
                        reachable ? "reachable" : "UNREACHABLE", sn_pk);
            } else {
                OXEN_LOG(warn, "Could not report node: {}", status);
            }
        } catch (...) {
            OXEN_LOG(err,
                     "Could not report node status: bad json in response");
        }
    };

    json params{
        {"type", "reachability"},
        {"pubkey", sn.pubkey_legacy.hex()},
        {"passed", reachable}
    };

    omq_server_.oxend_request("admin.report_peer_storage_server_status",
            std::move(cb), params.dump());

    if (!reachable) {
        std::lock_guard guard(sn_mutex_);
        reach_records_.add_failing_node(sn.pubkey_legacy, previous_failures);
    }
}

// Deterministically selects two random swarm members; returns true on success
bool ServiceNode::derive_tester_testee(uint64_t blk_height, sn_record_t& tester,
                                       sn_record_t& testee) {

    std::lock_guard guard(sn_mutex_);

    std::vector<sn_record_t> members = swarm_->other_nodes();
    members.push_back(our_address_);

    if (members.size() < 2) {
        OXEN_LOG(trace, "Could not initiate peer test: swarm too small");
        return false;
    }

    // Ew ew ew: pre-HF18 sorted via ascii on lower-case hex strings.
    // TODO: remove this after HF18.
    if (!hf_at_least(HARDFORK_NO_HEX_SORT_HACK))
        std::sort(members.begin(), members.end(), [](const auto& a, const auto& b) {
            std::array<char, 64> a_hex, b_hex;
            oxenmq::to_hex(a.pubkey_legacy.begin(), a.pubkey_legacy.end(), a_hex.begin());
            oxenmq::to_hex(b.pubkey_legacy.begin(), b.pubkey_legacy.end(), b_hex.begin());
            return std::string_view{a_hex.data(), a_hex.size()}
                <  std::string_view{b_hex.data(), b_hex.size()};
        });
    else
        std::sort(members.begin(), members.end(),
                [](const auto& a, const auto& b) { return a.pubkey_legacy < b.pubkey_legacy; });

    std::string block_hash;
    if (blk_height == block_height_) {
        block_hash = block_hash_;
    } else if (blk_height < block_height_) {

        OXEN_LOG(trace, "got storage test request for an older block: {}/{}",
                 blk_height, block_height_);

        const auto it =
            std::find_if(block_hashes_cache_.begin(), block_hashes_cache_.end(),
                         [=](const std::pair<uint64_t, std::string>& val) {
                             return val.first == blk_height;
                         });

        if (it != block_hashes_cache_.end()) {
            block_hash = it->second;
        } else {
            OXEN_LOG(trace, "Could not find hash for a given block height");
            // TODO: request from oxend?
            return false;
        }
    } else {
        assert(false);
        OXEN_LOG(debug, "Could not find hash: block height is in the future");
        return false;
    }

    uint64_t seed;
    if (block_hash.size() < sizeof(seed)) {
        OXEN_LOG(err, "Could not initiate peer test: invalid block hash");
        return false;
    }

    std::memcpy(&seed, block_hash.data(), sizeof(seed));
    boost::endian::little_to_native_inplace(seed);
    std::mt19937_64 mt(seed);
    const auto tester_idx =
        util::uniform_distribution_portable(mt, members.size());
    tester = members[tester_idx];

    uint64_t testee_idx;
    do {
        testee_idx = util::uniform_distribution_portable(mt, members.size());
    } while (testee_idx == tester_idx);

    testee = members[testee_idx];

    return true;
}

std::pair<MessageTestStatus, std::string> ServiceNode::process_storage_test_req(
    uint64_t blk_height,
    const legacy_pubkey& tester_pk,
    const std::string& msg_hash) {

    std::lock_guard guard(sn_mutex_);

    // 1. Check height, retry if we are behind
    std::string block_hash;

    if (blk_height > block_height_) {
        OXEN_LOG(debug, "Our blockchain is behind, height: {}, requested: {}",
                 block_height_, blk_height);
        return {MessageTestStatus::RETRY, ""};
    }

    // 2. Check tester/testee pair
    {
        sn_record_t tester;
        sn_record_t testee;
        this->derive_tester_testee(blk_height, tester, testee);

        if (testee != our_address_) {
            OXEN_LOG(err, "We are NOT the testee for height: {}", blk_height);
            return {MessageTestStatus::WRONG_REQ, ""};
        }

        if (tester.pubkey_legacy != tester_pk) {
            OXEN_LOG(debug, "Wrong tester: {}, expected: {}", tester_pk,
                     tester.pubkey_legacy);
#ifdef INTEGRATION_TEST
            OXEN_LOG(critical, "ABORT in integration test");
            std::abort();
#endif
            return {MessageTestStatus::WRONG_REQ, ""};
        } else {
            OXEN_LOG(trace, "Tester is valid: {}", tester_pk);
        }
    }

    // 3. If for a current/past block, try to respond right away
    Item item;
    if (!db_->retrieve_by_hash(msg_hash, item)) {
        return {MessageTestStatus::RETRY, ""};
    }

    return {MessageTestStatus::SUCCESS, std::move(item.data)};
}

std::optional<Item> ServiceNode::select_random_message() {

    uint64_t message_count;
    if (!db_->get_message_count(message_count)) {
        OXEN_LOG(err, "Could not count messages in the database");
        return {};
    }

    OXEN_LOG(debug, "total messages: {}", message_count);

    if (message_count == 0) {
        OXEN_LOG(debug, "No messages in the database to initiate a peer test");
        return {};
    }

    // SNodes don't have to agree on this, rather they should use different
    // messages
    const auto msg_idx = util::uniform_distribution_portable(message_count);

    auto item = std::make_optional<Item>();
    if (!db_->retrieve_by_index(msg_idx, *item)) {
        OXEN_LOG(err, "Could not retrieve message by index: {}", msg_idx);
        return {};
    }

    return item;
}

void ServiceNode::initiate_peer_test() {

    std::lock_guard guard(sn_mutex_);

    // 1. Select the tester/testee pair
    sn_record_t tester, testee;

    /// We test based on the height a few blocks back to minimise discrepancies
    /// between nodes (we could also use checkpoints, but that is still not
    /// bulletproof: swarms are calculated based on the latest block, so they
    /// might be still different and thus derive different pairs)
    constexpr uint64_t TEST_BLOCKS_BUFFER = 4;

    if (block_height_ < TEST_BLOCKS_BUFFER) {
        OXEN_LOG(debug, "Height {} is too small, skipping all tests",
                 block_height_);
        return;
    }

    const uint64_t test_height = block_height_ - TEST_BLOCKS_BUFFER;

    if (!this->derive_tester_testee(test_height, tester, testee)) {
        return;
    }

    OXEN_LOG(trace, "For height {}; tester: {} testee: {}", test_height,
            tester.pubkey_legacy, testee.pubkey_legacy);

    if (tester != our_address_) {
        /// Not our turn to initiate a test
        return;
    }

    /// 2. Storage Testing: initiate a testing request with a randomly selected message
    if (auto item = select_random_message()) {
        OXEN_LOG(trace, "Selected random message: {}, {}", item->hash, item->data);
        send_storage_test_req(testee, test_height, *item);
    } else {
        OXEN_LOG(debug, "Could not select a message for testing");
    }
}

void ServiceNode::bootstrap_peers(const std::vector<sn_record_t>& peers) const {

    std::vector<Item> all_entries;
    this->get_all_messages(all_entries);

    this->relay_messages(all_entries, peers);
}

void ServiceNode::bootstrap_swarms(
    const std::vector<swarm_id_t>& swarms) const {

    std::lock_guard guard(sn_mutex_);

    if (swarms.empty()) {
        OXEN_LOG(info, "Bootstrapping all swarms");
    } else {
        OXEN_LOG(info, "Bootstrapping swarms: [{}]", util::join(" ", swarms));
    }

    const auto& all_swarms = swarm_->all_valid_swarms();

    std::vector<Item> all_entries;
    if (!get_all_messages(all_entries)) {
        OXEN_LOG(err, "Could not retrieve entries from the database");
        return;
    }

    std::unordered_map<swarm_id_t, size_t> swarm_id_to_idx;
    for (size_t i = 0; i < all_swarms.size(); ++i) {
        swarm_id_to_idx.insert({all_swarms[i].swarm_id, i});
    }

    /// See what pubkeys we have
    std::unordered_map<std::string, swarm_id_t> cache;

    OXEN_LOG(debug, "We have {} messages", all_entries.size());

    std::unordered_map<swarm_id_t, std::vector<Item>> to_relay;

    for (auto& entry : all_entries) {

        swarm_id_t swarm_id;
        const auto it = cache.find(entry.pub_key);
        if (it == cache.end()) {

            bool success;
            auto pk = user_pubkey_t::create(entry.pub_key, success);

            if (!success) {
                OXEN_LOG(err, "Invalid pubkey in a message while "
                                "bootstrapping other nodes");
                continue;
            }

            swarm_id = get_swarm_by_pk(all_swarms, pk).swarm_id;
            cache.insert({entry.pub_key, swarm_id});
        } else {
            swarm_id = it->second;
        }

        bool relevant = false;
        for (const auto swarm : swarms) {

            if (swarm == swarm_id) {
                relevant = true;
            }
        }

        if (relevant || swarms.empty()) {

            to_relay[swarm_id].emplace_back(std::move(entry));
        }
    }

    OXEN_LOG(trace, "Bootstrapping {} swarms", to_relay.size());

    for (const auto& [swarm_id, items] : to_relay) {
        /// what if not found?
        const size_t idx = swarm_id_to_idx[swarm_id];

        relay_messages(items, all_swarms[idx].snodes);
    }
}

template <typename Message>
void ServiceNode::relay_messages(const std::vector<Message>& messages,
                                 const std::vector<sn_record_t>& snodes) const {
    std::vector<std::string> batches = serialize_messages(messages);

    OXEN_LOG(debug, "Relayed messages:");
    for (auto msg : batches) {
        OXEN_LOG(debug, "    {}", msg);
    }
    OXEN_LOG(debug, "To Snodes:");
    for (auto sn : snodes) {
        OXEN_LOG(debug, "    {}", sn.pubkey_legacy);
    }

    OXEN_LOG(debug, "Serialised batches: {}", batches.size());
    for (const sn_record_t& sn : snodes) {
        for (auto& batch : batches) {
            // TODO: I could probably avoid copying here
            this->relay_data_reliable(batch, sn);
        }
    }
}

void ServiceNode::salvage_data() const {

    /// This is very similar to ServiceNode::bootstrap_swarms, so just reuse it
    bootstrap_swarms({});
}

bool ServiceNode::retrieve(const std::string& pubKey,
                           const std::string& last_hash,
                           std::vector<Item>& items) {

    std::lock_guard guard(sn_mutex_);

    all_stats_.bump_retrieve_requests();

    return db_->retrieve(pubKey, items, last_hash,
                         CLIENT_RETRIEVE_MESSAGE_LIMIT);
}

void to_json(nlohmann::json& j, const test_result_t& val) {
    j["timestamp"] = val.timestamp;
    j["result"] = to_str(val.result);
}

static nlohmann::json to_json(const all_stats_t& stats) {

    nlohmann::json json;

    json["total_store_requests"] = stats.get_total_store_requests();
    json["recent_store_requests"] = stats.get_recent_store_requests();
    json["previous_period_store_requests"] =
        stats.get_previous_period_store_requests();

    json["total_retrieve_requests"] = stats.get_total_retrieve_requests();
    json["recent_store_requests"] = stats.get_recent_store_requests();
    json["previous_period_retrieve_requests"] =
        stats.get_previous_period_retrieve_requests();

    json["previous_period_onion_requests"] =
        stats.get_previous_period_onion_requests();

    json["reset_time"] = std::chrono::duration_cast<std::chrono::seconds>(
                             stats.get_reset_time().time_since_epoch())
                             .count();

    nlohmann::json peers;

    for (const auto& [pk, stats] : stats.peer_report_) {
        auto pubkey = pk.hex();

        peers[pubkey]["requests_failed"] = stats.requests_failed;
        peers[pubkey]["pushes_failed"] = stats.requests_failed;
        peers[pubkey]["storage_tests"] = stats.storage_tests;
    }

    json["peers"] = peers;
    return json;
}

std::string ServiceNode::get_stats_for_session_client() const {

    nlohmann::json res;
    res["version"] = STORAGE_SERVER_VERSION_STRING;

    constexpr bool PRETTY = true;
    constexpr int indent = PRETTY ? 4 : 0;
    return res.dump(indent);
}

std::string ServiceNode::get_stats() const {

    std::lock_guard guard(sn_mutex_);

    auto val = to_json(all_stats_);

    val["version"] = STORAGE_SERVER_VERSION_STRING;
    val["height"] = block_height_;
    val["target_height"] = target_height_;

    uint64_t total_stored;
    if (db_->get_message_count(total_stored)) {
        val["total_stored"] = total_stored;
    }

    // TODO: figure out some more interesting stats (these counters don't tell us much at all except
    // for, maybe, a slow loris attack in progress, and so were removed)
    val["connections_in"] = -1;
    val["http_connections_out"] = -1;
    val["https_connections_out"] = -1;

    /// we want pretty (indented) json, but might change that in the future
    constexpr bool PRETTY = true;
    constexpr int indent = PRETTY ? 4 : 0;
    return val.dump(indent);
}

std::string ServiceNode::get_status_line() const {
    // This produces a short, single-line status string, used when running as a
    // systemd Type=notify service to update the service Status line.  The
    // status message has to be fairly short: has to fit on one line, and if
    // it's too long systemd just truncates it when displaying it.

    std::lock_guard guard(sn_mutex_);

    std::ostringstream s;
    s << 'v' << STORAGE_SERVER_VERSION_STRING;
    if (!oxen::is_mainnet)
        s << " (TESTNET)";

    if (syncing_)
        s << "; SYNCING";
    s << "; sw=";
    if (!swarm_ || !swarm_->is_valid())
        s << "NONE";
    else {
        std::string swarm = std::to_string(swarm_->our_swarm_id());
        if (swarm.size() <= 6)
            s << swarm;
        else
            s << swarm.substr(0, 4) << u8"…" << swarm.back();
        s << "(n=" << (1 + swarm_->other_nodes().size()) << ")";
    }
    uint64_t total_stored;
    if (db_->get_message_count(total_stored))
        s << "; " << total_stored << " msgs";
    s << "; reqs(S/R): " << all_stats_.get_total_store_requests() << '/'
      << all_stats_.get_total_retrieve_requests();
    // FIXME: something better?
    /*s << "; conns(in/http/https): " << get_net_stats().connections_in << '/'
      << get_net_stats().http_connections_out << '/'
      << get_net_stats().https_connections_out;*/
    return s.str();
}

bool ServiceNode::get_all_messages(std::vector<Item>& all_entries) const {

    std::lock_guard guard(sn_mutex_);

    OXEN_LOG(trace, "Get all messages");

    return db_->retrieve("", all_entries, "");
}

void ServiceNode::process_push_batch(const std::string& blob) {

    std::lock_guard guard(sn_mutex_);

    if (blob.empty())
        return;

    std::vector<message_t> messages = deserialize_messages(blob);

    OXEN_LOG(trace, "Saving all: begin");

    OXEN_LOG(debug, "Got {} messages from peers, size: {}", messages.size(),
             blob.size());

    std::vector<Item> items;
    items.reserve(messages.size());

    // TODO: avoid copying m.data
    // Promoting message_t to Item:
    std::transform(messages.begin(), messages.end(), std::back_inserter(items),
                   [](const message_t& m) {
                       return Item{m.hash, m.pub_key,           m.timestamp,
                                   m.ttl,  m.timestamp + m.ttl, m.nonce,
                                   m.data};
                   });

    this->save_bulk(items);

    OXEN_LOG(trace, "Saving all: end");
}

bool ServiceNode::is_pubkey_for_us(const user_pubkey_t& pk) const {

    std::lock_guard guard(sn_mutex_);

    if (!swarm_) {
        OXEN_LOG(err, "Swarm data missing");
        return false;
    }
    return swarm_->is_pubkey_for_us(pk);
}

std::vector<sn_record_t>
ServiceNode::get_snodes_by_pk(const user_pubkey_t& pk) {

    std::lock_guard guard(sn_mutex_);

    if (!swarm_) {
        OXEN_LOG(err, "Swarm data missing");
        return {};
    }

    return get_swarm_by_pk(swarm_->all_valid_swarms(), pk).snodes;
}

} // namespace oxen
