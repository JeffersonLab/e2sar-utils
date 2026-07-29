/*
 * Copyright (c) 2025, Jefferson Science Associates, all rights reserved.
 * See LICENSE.txt file.
 * Thomas Jefferson National Accelerator Facility
 * Experimental Physics Software and Computing Infrastructure Group
 *
 * ERSAP actor that receives EJFAT UDP packets, reassembles complete events
 * via the E2SAR Reassembler, and publishes each event as a raw byte payload
 * onto the ERSAP xMsg transport for consumption by the next actor in the
 * processing chain (e.g. ersap_processor_actor).
 *
 * Output payload layout (little-endian, native host order):
 *   [ 8 bytes : data_id encoded as double ][ N bytes : reassembled event body ]
 *
 * The output data type is ersap::type::BYTES, which is transported over xMsg
 * as raw bytes and surfaces on the Java side as a java.nio.ByteBuffer.
 */

#include <e2sar.hpp>

#include <ersap/engine.hpp>
#include <ersap/engine_data.hpp>
#include <ersap/engine_data_type.hpp>
#include <ersap/stdlib/json_utils.hpp>
#include <ersap/third_party/json11.hpp>

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace ersap {
namespace ejfat {

class EjfatReceiverActor : public ersap::Engine {
public:
    EjfatReceiverActor() = default;
    ~EjfatReceiverActor() override;

    ersap::EngineData configure(ersap::EngineData& input) override;
    ersap::EngineData execute(ersap::EngineData& input) override;
    ersap::EngineData execute_group(const std::vector<ersap::EngineData>& inputs) override;

    std::vector<ersap::EngineDataType> input_data_types() const override;
    std::vector<ersap::EngineDataType> output_data_types() const override;
    std::set<std::string> states() const override;

    std::string name() const override      { return "EjfatReceiverActor"; }
    std::string author() const override    { return "JLab EPSCI"; }
    std::string description() const override {
        return "Receives EJFAT UDP packets, reassembles events via E2SAR, "
               "and publishes each event as raw BYTES over xMsg.";
    }
    std::string version() const override   { return "1.0.0"; }

private:
    // JSON configuration keys
    static constexpr const char* KEY_URI            = "ejfat_uri";
    static constexpr const char* KEY_RECV_IP        = "recv_ip";
    static constexpr const char* KEY_RECV_PORT      = "recv_port";
    static constexpr const char* KEY_RECV_THREADS   = "recv_threads";
    static constexpr const char* KEY_EVT_TIMEOUT_MS = "event_timeout_ms";
    static constexpr const char* KEY_MAX_WAIT_MS    = "max_wait_ms";
    static constexpr const char* KEY_WITH_CP        = "with_cp";
    static constexpr const char* KEY_VALIDATE_CERT  = "validate_cert";
    static constexpr const char* KEY_VERBOSE        = "verbose";

    // Configuration (populated by configure())
    std::string ejfat_uri_;
    std::string recv_ip_          = "0.0.0.0";
    std::uint16_t recv_port_      = 19522;
    std::size_t recv_threads_     = 1;
    int event_timeout_ms_         = 500;
    int max_wait_ms_              = 5000;
    bool with_cp_                 = false;
    bool validate_cert_           = true;
    bool verbose_                 = false;

    // Poll granularity when waiting for a reassembled event
    static constexpr int POLL_STEP_MS = 100;

    // Runtime state
    std::unique_ptr<e2sar::Reassembler> reassembler_;
    std::mutex init_mutex_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> shutting_down_{false};
    bool worker_registered_ = false;

    // Statistics
    std::atomic<std::uint64_t> event_count_{0};
    std::atomic<std::uint64_t> timeout_count_{0};
    std::atomic<std::uint64_t> error_count_{0};
    std::atomic<std::uint64_t> total_bytes_{0};

    bool initializeReassembler(std::string& err);
    void shutdownReassembler();
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

EjfatReceiverActor::~EjfatReceiverActor()
{
    shutdownReassembler();
}

ersap::EngineData EjfatReceiverActor::configure(ersap::EngineData& input)
{
    ersap::EngineData output;

    try {
        auto cfg = ersap::stdlib::parse_json(input);

        if (ersap::stdlib::has_key(cfg, KEY_URI)) {
            ejfat_uri_ = ersap::stdlib::get_string(cfg, KEY_URI);
        }
        if (ersap::stdlib::has_key(cfg, KEY_RECV_IP)) {
            recv_ip_ = ersap::stdlib::get_string(cfg, KEY_RECV_IP);
        }
        if (ersap::stdlib::has_key(cfg, KEY_RECV_PORT)) {
            recv_port_ = static_cast<std::uint16_t>(
                ersap::stdlib::get_int(cfg, KEY_RECV_PORT));
        }
        if (ersap::stdlib::has_key(cfg, KEY_RECV_THREADS)) {
            recv_threads_ = static_cast<std::size_t>(
                ersap::stdlib::get_int(cfg, KEY_RECV_THREADS));
        }
        if (ersap::stdlib::has_key(cfg, KEY_EVT_TIMEOUT_MS)) {
            event_timeout_ms_ = ersap::stdlib::get_int(cfg, KEY_EVT_TIMEOUT_MS);
        }
        if (ersap::stdlib::has_key(cfg, KEY_MAX_WAIT_MS)) {
            max_wait_ms_ = ersap::stdlib::get_int(cfg, KEY_MAX_WAIT_MS);
        }
        if (ersap::stdlib::has_key(cfg, KEY_WITH_CP)) {
            with_cp_ = ersap::stdlib::get_bool(cfg, KEY_WITH_CP);
        }
        if (ersap::stdlib::has_key(cfg, KEY_VALIDATE_CERT)) {
            validate_cert_ = ersap::stdlib::get_bool(cfg, KEY_VALIDATE_CERT);
        }
        if (ersap::stdlib::has_key(cfg, KEY_VERBOSE)) {
            verbose_ = ersap::stdlib::get_bool(cfg, KEY_VERBOSE);
        }

        if (ejfat_uri_.empty()) {
            throw ersap::stdlib::JsonError(
                "EjfatReceiverActor: required config key 'ejfat_uri' is missing");
        }
        if (event_timeout_ms_ <= 0) {
            throw ersap::stdlib::JsonError(
                "EjfatReceiverActor: 'event_timeout_ms' must be greater than 0");
        }
        if (max_wait_ms_ <= 0) {
            throw ersap::stdlib::JsonError(
                "EjfatReceiverActor: 'max_wait_ms' must be greater than 0");
        }

        std::string err;
        if (!initializeReassembler(err)) {
            output.set_status(ersap::EngineStatus::ERROR);
            output.set_description("EjfatReceiverActor configure failed: " + err);
            std::cerr << "EjfatReceiverActor: " << err << std::endl;
            return output;
        }

        if (verbose_) {
            std::cout << "EjfatReceiverActor configured:"
                      << "\n  uri              = " << ejfat_uri_
                      << "\n  recv_ip          = " << recv_ip_
                      << "\n  recv_port        = " << recv_port_
                      << "\n  recv_threads     = " << recv_threads_
                      << "\n  event_timeout_ms = " << event_timeout_ms_
                      << "\n  max_wait_ms      = " << max_wait_ms_
                      << "\n  with_cp          = " << (with_cp_ ? "true" : "false")
                      << "\n  validate_cert    = " << (validate_cert_ ? "true" : "false")
                      << std::endl;
        }

    } catch (const std::exception& e) {
        output.set_status(ersap::EngineStatus::ERROR);
        output.set_description(std::string("EjfatReceiverActor configure error: ") + e.what());
        std::cerr << "EjfatReceiverActor configure error: " << e.what() << std::endl;
    }

    return output;
}

bool EjfatReceiverActor::initializeReassembler(std::string& err)
{
    std::lock_guard<std::mutex> lock(init_mutex_);
    if (initialized_.load()) {
        return true;
    }

    auto uri_result = e2sar::EjfatURI::getFromString(
        ejfat_uri_, e2sar::EjfatURI::TokenType::instance, false);
    if (uri_result.has_error()) {
        err = "failed to parse EJFAT URI: " + uri_result.error().message();
        return false;
    }

    boost::asio::ip::address ip;
    try {
        ip = boost::asio::ip::make_address(recv_ip_);
    } catch (const std::exception& e) {
        err = std::string("invalid recv_ip: ") + e.what();
        return false;
    }

    e2sar::Reassembler::ReassemblerFlags rflags;
    rflags.useCP = with_cp_;
    // Without the control plane the segmenter's LB header stays on the wire.
    rflags.withLBHeader = !with_cp_;
    rflags.eventTimeout_ms = event_timeout_ms_;
    if (with_cp_) {
        rflags.validateCert = validate_cert_;
    }

    try {
        reassembler_ = std::make_unique<e2sar::Reassembler>(
            uri_result.value(), ip, recv_port_, recv_threads_, rflags);
    } catch (const std::exception& e) {
        err = std::string("failed to construct Reassembler: ") + e.what();
        return false;
    }

    auto hostname_res = e2sar::NetUtil::getHostName();
    if (!hostname_res.has_error()) {
        auto regres = reassembler_->registerWorker(hostname_res.value());
        if (regres.has_error()) {
            std::cerr << "EjfatReceiverActor: warning, registerWorker failed: "
                      << regres.error().message() << std::endl;
        } else {
            worker_registered_ = true;
        }
    }

    auto open_result = reassembler_->openAndStart();
    if (open_result.has_error()) {
        err = "failed to start Reassembler: " + open_result.error().message();
        reassembler_.reset();
        return false;
    }

    initialized_.store(true);
    return true;
}

void EjfatReceiverActor::shutdownReassembler()
{
    shutting_down_.store(true);

    std::lock_guard<std::mutex> lock(init_mutex_);
    if (!reassembler_) {
        return;
    }

    if (worker_registered_) {
        auto deregres = reassembler_->deregisterWorker();
        if (deregres.has_error()) {
            std::cerr << "EjfatReceiverActor: warning, deregisterWorker failed: "
                      << deregres.error().message() << std::endl;
        }
        worker_registered_ = false;
    }

    reassembler_->stopThreads();
    reassembler_.reset();
    initialized_.store(false);
}

// ---------------------------------------------------------------------------
// Event pump
// ---------------------------------------------------------------------------

ersap::EngineData EjfatReceiverActor::execute(ersap::EngineData& /*input*/)
{
    ersap::EngineData output;

    if (!initialized_.load()) {
        output.set_status(ersap::EngineStatus::ERROR);
        output.set_description("EjfatReceiverActor: reassembler is not initialized");
        return output;
    }

    std::uint8_t* event_buffer = nullptr;
    std::size_t event_size = 0;
    e2sar::EventNum_t event_num = 0;
    std::uint16_t data_id = 0;

    // Poll in short steps so shutdown is responsive.
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(max_wait_ms_);

    bool received = false;
    while (!shutting_down_.load()) {
        auto result = reassembler_->recvEvent(
            &event_buffer, &event_size, &event_num, &data_id, POLL_STEP_MS);

        if (result.has_error()) {
            error_count_++;
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            continue;
        }
        if (result.value() == -1) {
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            continue;
        }

        received = true;
        break;
    }

    if (!received) {
        timeout_count_++;
        output.set_status(ersap::EngineStatus::WARNING);
        output.set_description("EjfatReceiverActor: no event within max_wait_ms");
        return output;
    }

    // Build augmented payload: [data_id as double][event body]
    const std::size_t augmented_size = sizeof(double) + event_size;
    std::vector<std::uint8_t> payload(augmented_size);
    const double data_id_as_double = static_cast<double>(data_id);
    std::memcpy(payload.data(), &data_id_as_double, sizeof(double));
    if (event_size > 0 && event_buffer != nullptr) {
        std::memcpy(payload.data() + sizeof(double), event_buffer, event_size);
    }

    // Reassembler allocates the buffer with new[]; caller must free.
    delete[] event_buffer;

    event_count_++;
    total_bytes_ += event_size;

    output.set_data(ersap::type::BYTES, std::move(payload));
    output.set_communication_id(static_cast<long>(event_num));

    if (verbose_) {
        std::cout << "EjfatReceiverActor: published event "
                  << event_num << " (data_id=" << data_id
                  << ", body=" << event_size << " bytes, total="
                  << augmented_size << " bytes)" << std::endl;
    }

    return output;
}

ersap::EngineData EjfatReceiverActor::execute_group(
    const std::vector<ersap::EngineData>& /*inputs*/)
{
    // No group semantics for a source-style receiver.
    ersap::EngineData output;
    output.set_status(ersap::EngineStatus::WARNING);
    output.set_description("EjfatReceiverActor: execute_group is not supported");
    return output;
}

// ---------------------------------------------------------------------------
// Data type / metadata declarations
// ---------------------------------------------------------------------------

std::vector<ersap::EngineDataType> EjfatReceiverActor::input_data_types() const
{
    // Any input acts as a trigger; SINT32 mirrors the Java trigger-source pattern
    // used elsewhere in the pipeline, and BYTES allows chaining behind another
    // native actor without a translation stage.
    return {ersap::type::SINT32, ersap::type::BYTES};
}

std::vector<ersap::EngineDataType> EjfatReceiverActor::output_data_types() const
{
    return {ersap::type::BYTES};
}

std::set<std::string> EjfatReceiverActor::states() const
{
    return {};
}

}  // namespace ejfat
}  // namespace ersap

// ---------------------------------------------------------------------------
// ERSAP plugin entry point
// ---------------------------------------------------------------------------

extern "C" std::unique_ptr<ersap::Engine> create_engine()
{
    return std::make_unique<ersap::ejfat::EjfatReceiverActor>();
}
