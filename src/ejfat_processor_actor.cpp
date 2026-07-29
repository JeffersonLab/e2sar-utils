/*
 * Copyright (c) 2025, Jefferson Science Associates, all rights reserved.
 * See LICENSE.txt file.
 * Thomas Jefferson National Accelerator Facility
 * Experimental Physics Software and Computing Infrastructure Group
 *
 * ERSAP actor that consumes reassembled EJFAT events published by
 * ejfat_receiver_actor over the xMsg transport.
 *
 * Input payload contract (produced by ejfat_receiver_actor):
 *   [ 8 bytes : data_id encoded as double ][ N bytes : reassembled event body ]
 *
 * Processing is intentionally omitted: the actor decodes the envelope,
 * optionally logs the event, and passes the reassembled body downstream
 * as BYTES so it can be chained with other engines.
 */

#include <ersap/engine.hpp>
#include <ersap/engine_data.hpp>
#include <ersap/engine_data_type.hpp>
#include <ersap/stdlib/json_utils.hpp>
#include <ersap/third_party/json11.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace ersap {
namespace ejfat {

class EjfatProcessorActor : public ersap::Engine {
public:
    EjfatProcessorActor() = default;
    ~EjfatProcessorActor() override = default;

    ersap::EngineData configure(ersap::EngineData& input) override;
    ersap::EngineData execute(ersap::EngineData& input) override;
    ersap::EngineData execute_group(const std::vector<ersap::EngineData>& inputs) override;

    std::vector<ersap::EngineDataType> input_data_types() const override;
    std::vector<ersap::EngineDataType> output_data_types() const override;
    std::set<std::string> states() const override;

    std::string name() const override      { return "EjfatProcessorActor"; }
    std::string author() const override    { return "JLab EPSCI"; }
    std::string description() const override {
        return "Consumes reassembled EJFAT events published by "
               "ejfat_receiver_actor. Decodes the [data_id][payload] envelope "
               "and forwards the payload downstream. Processing is omitted.";
    }
    std::string version() const override   { return "1.0.0"; }

private:
    static constexpr const char* KEY_VERBOSE      = "verbose";
    static constexpr const char* KEY_FORWARD_BODY = "forward_body";

    // Prepended data_id occupies exactly one double (see ejfat_receiver_actor).
    static constexpr std::size_t HEADER_SIZE = sizeof(double);

    bool verbose_      = false;
    bool forward_body_ = true;

    std::atomic<std::uint64_t> event_count_  = {0};
    std::atomic<std::uint64_t> reject_count_ = {0};
    std::atomic<std::uint64_t> total_bytes_  = {0};
};

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

ersap::EngineData EjfatProcessorActor::configure(ersap::EngineData& input)
{
    ersap::EngineData output;

    try {
        auto cfg = ersap::stdlib::parse_json(input);

        if (ersap::stdlib::has_key(cfg, KEY_VERBOSE)) {
            verbose_ = ersap::stdlib::get_bool(cfg, KEY_VERBOSE);
        }
        if (ersap::stdlib::has_key(cfg, KEY_FORWARD_BODY)) {
            forward_body_ = ersap::stdlib::get_bool(cfg, KEY_FORWARD_BODY);
        }

        if (verbose_) {
            std::cout << "EjfatProcessorActor configured:"
                      << "\n  verbose      = " << (verbose_ ? "true" : "false")
                      << "\n  forward_body = " << (forward_body_ ? "true" : "false")
                      << std::endl;
        }

    } catch (const std::exception& e) {
        output.set_status(ersap::EngineStatus::ERROR);
        output.set_description(std::string("EjfatProcessorActor configure error: ") + e.what());
        std::cerr << "EjfatProcessorActor configure error: " << e.what() << std::endl;
    }

    return output;
}

// ---------------------------------------------------------------------------
// Event consumption
// ---------------------------------------------------------------------------

ersap::EngineData EjfatProcessorActor::execute(ersap::EngineData& input)
{
    ersap::EngineData output;

    if (input.mime_type() != ersap::type::BYTES.mime_type()) {
        reject_count_++;
        output.set_status(ersap::EngineStatus::ERROR);
        output.set_description("EjfatProcessorActor: unexpected mime-type '"
                               + input.mime_type() + "', expected '"
                               + ersap::type::BYTES.mime_type() + "'");
        return output;
    }

    const auto& payload = ersap::data_cast<std::vector<std::uint8_t>>(input);

    if (payload.size() < HEADER_SIZE) {
        reject_count_++;
        output.set_status(ersap::EngineStatus::ERROR);
        output.set_description("EjfatProcessorActor: payload smaller than data_id header ("
                               + std::to_string(payload.size()) + " bytes)");
        return output;
    }

    double data_id_as_double = 0.0;
    std::memcpy(&data_id_as_double, payload.data(), HEADER_SIZE);
    const auto data_id = static_cast<std::uint16_t>(data_id_as_double);
    const std::size_t body_size = payload.size() - HEADER_SIZE;

    event_count_++;
    total_bytes_ += body_size;

    if (verbose_) {
        std::cout << "EjfatProcessorActor: received event #" << event_count_.load()
                  << " (data_id=" << data_id
                  << ", body=" << body_size << " bytes, total="
                  << payload.size() << " bytes, comm_id="
                  << input.communication_id() << ")"
                  << std::endl;
    }

    // Processing is intentionally omitted.

    if (forward_body_) {
        std::vector<std::uint8_t> body(payload.begin() + HEADER_SIZE, payload.end());
        output.set_data(ersap::type::BYTES, std::move(body));
        output.set_communication_id(input.communication_id());
    } else {
        output.set_status(ersap::EngineStatus::INFO);
        output.set_description("EjfatProcessorActor: event consumed (forward_body=false)");
    }

    return output;
}

ersap::EngineData EjfatProcessorActor::execute_group(
    const std::vector<ersap::EngineData>& /*inputs*/)
{
    ersap::EngineData output;
    output.set_status(ersap::EngineStatus::WARNING);
    output.set_description("EjfatProcessorActor: execute_group is not supported");
    return output;
}

// ---------------------------------------------------------------------------
// Data type / metadata declarations
// ---------------------------------------------------------------------------

std::vector<ersap::EngineDataType> EjfatProcessorActor::input_data_types() const
{
    return {ersap::type::BYTES};
}

std::vector<ersap::EngineDataType> EjfatProcessorActor::output_data_types() const
{
    return {ersap::type::BYTES};
}

std::set<std::string> EjfatProcessorActor::states() const
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
    return std::make_unique<ersap::ejfat::EjfatProcessorActor>();
}
