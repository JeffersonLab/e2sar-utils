#include <et.h>
#include <boost/program_options.hpp>
#include <boost/asio.hpp>
#include <e2sar.hpp>
#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <atomic>
#include <signal.h>

namespace po = boost::program_options;

// Forward declaration of global flag for signal handling
extern std::atomic<bool> keep_receiving;

// Signal handler for Ctrl+C
void signalHandler(int signal) {
    if (signal == SIGINT) {
        std::cout << "\nReceived interrupt signal, stopping..." << std::endl;
        keep_receiving = false;
    }
}

struct CommandLineArgs {
    // E2SAR receiving options
    std::string ejfat_uri;
    std::string recv_ip;
    uint16_t recv_port = 19522;
    size_t recv_threads = 1;
    int event_timeout_ms = 500;
    bool withCP = false;
    bool validate = true;

    // ET system options
    std::string et_file;
    std::string et_host;
    int et_port = 11111;
    size_t et_event_size = 2097152;  // 2 MB default
};

// ET system state
struct ETSystem {
    et_sys_id system = nullptr;
    et_att_id attachment = 0;
    size_t event_size = 0;
    bool initialized = false;
};

// Initialize and attach to ET system
bool initializeET(ETSystem& et, const CommandLineArgs& args) {
    std::cout << "\nInitializing ET system..." << std::endl;

    et_openconfig openConfig;
    et_open_config_init(&openConfig);

    // Set wait mode (standard pattern from ET examples)
    et_open_config_setwait(openConfig, ET_OPEN_WAIT);

    // Configure connection method
    if (!args.et_host.empty()) {
        std::cout << "  Using direct connection to host: " << args.et_host << std::endl;
        et_open_config_sethost(openConfig, args.et_host.c_str());
        et_open_config_setcast(openConfig, ET_DIRECT);
        if (args.et_port > 0) {
            et_open_config_setserverport(openConfig, args.et_port);
        }
    } else {
        std::cout << "  Using broadcast discovery" << std::endl;
        et_open_config_setcast(openConfig, ET_BROADCAST);
    }

    // Set timeout
    struct timespec timeout;
    timeout.tv_sec = 10;
    timeout.tv_nsec = 0;
    et_open_config_settimeout(openConfig, timeout);

    // Open ET system
    std::cout << "  Opening ET system file: " << args.et_file << std::endl;
    int status = et_open(&et.system, args.et_file.c_str(), openConfig);
    et_open_config_destroy(openConfig);

    if (status != ET_OK) {
        std::cerr << "Error: Failed to open ET system (status " << status << ")" << std::endl;
        return false;
    }

    // Query actual ET system event size
    size_t system_event_size;
    status = et_system_geteventsize(et.system, &system_event_size);
    if (status != ET_OK) {
        std::cerr << "Error: Failed to get ET system event size (status " << status << ")" << std::endl;
        et_close(et.system);
        return false;
    }

    // Verify requested size doesn't exceed system capacity
    if (args.et_event_size > system_event_size) {
        std::cerr << "Error: Requested event size (" << args.et_event_size
                  << " bytes) exceeds ET system event size (" << system_event_size << " bytes)" << std::endl;
        et_close(et.system);
        return false;
    }

    // Attach to GRAND_CENTRAL station (ID 0)
    std::cout << "  Attaching to GRAND_CENTRAL station..." << std::endl;
    status = et_station_attach(et.system, ET_GRANDCENTRAL, &et.attachment);
    if (status != ET_OK) {
        std::cerr << "Error: Failed to attach to ET station (status " << status << ")" << std::endl;
        et_close(et.system);
        return false;
    }

    et.event_size = args.et_event_size;
    et.initialized = true;

    std::cout << "ET system initialized successfully" << std::endl;
    std::cout << "  ET system event size: " << system_event_size << " bytes" << std::endl;
    std::cout << "  Using event buffer size: " << et.event_size << " bytes" << std::endl;
    std::cout << "  Attachment ID: " << et.attachment << std::endl;

    return true;
}

// Cleanup ET system
void cleanupET(ETSystem& et) {
    if (et.initialized) {
        std::cout << "\nCleaning up ET system..." << std::endl;
        if (et.attachment != 0) {
            et_station_detach(et.system, et.attachment);
        }
        if (et.system != nullptr) {
            et_close(et.system);
        }
        et.initialized = false;
    }
}

// Write data to ET system
bool writeToET(ETSystem& et, const uint8_t* data, size_t size) {
    if (!et.initialized) {
        std::cerr << "Error: ET system not initialized" << std::endl;
        return false;
    }

    if (size > et.event_size) {
        std::cerr << "Error: Data size (" << size << " bytes) exceeds ET event size ("
                  << et.event_size << " bytes)" << std::endl;
        return false;
    }

    et_event* pe;
    struct timespec timeout;
    timeout.tv_sec = 2;
    timeout.tv_nsec = 0;

    // Request new event with 2-second timeout (singular version)
    int status = et_event_new(et.system, et.attachment, &pe, ET_TIMED,
                              &timeout, size);

    if (status != ET_OK) {
        std::cerr << "Error: Failed to get ET event (status " << status << ")" << std::endl;
        return false;
    }

    // Get event data pointer
    void* eventData = nullptr;
    et_event_getdata(pe, &eventData);

    // Copy data to ET event
    std::memcpy(eventData, data, size);
    et_event_setlength(pe, size);

    // Return event to ET system (singular version)
    status = et_event_put(et.system, et.attachment, pe);
    if (status != ET_OK) {
        std::cerr << "Error: Failed to put ET event (status " << status << ")" << std::endl;
        return false;
    }

    return true;
}

// Initialize and start E2SAR Reassembler
std::unique_ptr<e2sar::Reassembler> initializeReassembler(
    const std::string& uri_str,
    const std::string& recv_ip,
    uint16_t recv_port,
    size_t num_threads,
    int event_timeout_ms,
    bool withCP = false,
    bool validateCert = true) {

    std::cout << "\nInitializing E2SAR Reassembler..." << std::endl;
    if (withCP) {
        std::cout << "  SSL certificate validation: " << (validateCert ? "enabled" : "disabled") << std::endl;
    }

    // Parse EJFAT URI
    auto uri_result = e2sar::EjfatURI::getFromString(uri_str,
        e2sar::EjfatURI::TokenType::instance, false);

    if (uri_result.has_error()) {
        std::cerr << "Error parsing URI: " << uri_result.error().message() << std::endl;
        return nullptr;
    }

    e2sar::EjfatURI uri = uri_result.value();

    // Parse IP address
    boost::asio::ip::address ip;
    try {
        ip = boost::asio::ip::make_address(recv_ip);
    } catch (const std::exception& e) {
        std::cerr << "Error parsing IP address: " << e.what() << std::endl;
        return nullptr;
    }

    // Create Reassembler flags
    e2sar::Reassembler::ReassemblerFlags rflags;
    rflags.useCP = withCP;
    // When NOT using control plane (direct send), packets have LB header from segmenter
    // When using control plane, LB strips/modifies the header
    rflags.withLBHeader = !withCP;
    rflags.eventTimeout_ms = event_timeout_ms;

    // Create Reassembler
    auto reassembler = std::make_unique<e2sar::Reassembler>(
        uri, ip, recv_port, num_threads, rflags);

    std::cout << "Using IP address: " << reassembler->get_dataIP() << std::endl;
    std::cout << "Receiving on ports: " << reassembler->get_recvPorts().first
              << ":" << reassembler->get_recvPorts().second << std::endl;

    // Register worker (NOOP if not using control plane)
    auto hostname_res = e2sar::NetUtil::getHostName();
    if (!hostname_res.has_error()) {
        auto regres = reassembler->registerWorker(hostname_res.value());
        if (regres.has_error()) {
            std::cerr << "Warning: Unable to register worker: "
                      << regres.error().message() << std::endl;
        }
    }

    // Open and start receiving threads
    auto open_result = reassembler->openAndStart();
    if (open_result.has_error()) {
        std::cerr << "Error starting reassembler: " << open_result.error().message() << std::endl;
        return nullptr;
    }

    std::cout << "Reassembler started successfully" << std::endl;
    std::cout << "  Event timeout: " << event_timeout_ms << " ms" << std::endl;
    std::cout << "  Receive threads: " << num_threads << std::endl;

    return reassembler;
}

// Statistics for receiving events
struct ReceiveStats {
    std::atomic<uint64_t> events_received{0};
    std::atomic<uint64_t> events_written{0};
    std::atomic<uint64_t> write_errors{0};
    std::atomic<uint64_t> total_bytes{0};

    void printProgress() const {
        std::cout << "  Events received: " << events_received
                  << " | Written to ET: " << events_written
                  << " | Errors: " << write_errors
                  << " | Total MB: " << (total_bytes / (1024.0 * 1024.0))
                  << std::endl;
    }
};

// Global flag for signal handling
std::atomic<bool> keep_receiving{true};

// Receive events and write to ET system
bool receiveEvents(e2sar::Reassembler& reassembler, ETSystem& et) {

    std::cout << "\nStarting event reception..." << std::endl;
    std::cout << "Press Ctrl+C to stop\n" << std::endl;

    ReceiveStats stats;
    uint8_t* event_buffer = nullptr;
    size_t event_size;
    e2sar::EventNum_t event_num;
    uint16_t data_id;

    auto start_time = std::chrono::steady_clock::now();
    auto last_progress = start_time;

    while (keep_receiving) {
        // Receive event with 1000ms timeout
        auto result = reassembler.recvEvent(&event_buffer, &event_size,
                                           &event_num, &data_id, 1000);

        auto now = std::chrono::steady_clock::now();

        // Print progress every 5 seconds
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_progress).count() >= 5) {
            stats.printProgress();
            last_progress = now;
        }

        // Check for errors
        if (result.has_error()) {
            // Timeout or other error - continue
            continue;
        }

        // No event available (timeout)
        if (result.value() == -1) {
            continue;
        }

        // Event received successfully
        stats.events_received++;
        stats.total_bytes += event_size;

        // Write to ET system
        if (writeToET(et, event_buffer, event_size)) {
            stats.events_written++;
        } else {
            stats.write_errors++;
            std::cerr << "Failed to write event " << event_num << " to ET" << std::endl;
        }

        // Free event buffer (allocated by E2SAR)
        delete[] event_buffer;
        event_buffer = nullptr;
    }

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    // Final statistics
    std::cout << "\n========== Reception Complete ==========" << std::endl;
    std::cout << "Events received: " << stats.events_received << std::endl;
    std::cout << "Events written to ET: " << stats.events_written << std::endl;
    std::cout << "Write errors: " << stats.write_errors << std::endl;
    std::cout << "Total data: " << (stats.total_bytes / (1024.0 * 1024.0)) << " MB" << std::endl;
    std::cout << "Duration: " << duration.count() << " ms" << std::endl;

    if (stats.events_received > 0) {
        double mbps = (stats.total_bytes * 8.0 / 1000000.0) / (duration.count() / 1000.0);
        std::cout << "Average rate: " << mbps << " Mbps" << std::endl;
    }

    // Get reassembler stats
    auto reas_stats = reassembler.getStats();
    std::cout << "\nReassembler Statistics:" << std::endl;
    std::cout << "  Total packets: " << reas_stats.totalPackets << std::endl;
    std::cout << "  Total bytes: " << reas_stats.totalBytes << std::endl;
    std::cout << "  Event success: " << reas_stats.eventSuccess << std::endl;
    std::cout << "  Reassembly loss: " << reas_stats.reassemblyLoss << std::endl;
    std::cout << "  Enqueue loss: " << reas_stats.enqueueLoss << std::endl;
    std::cout << "  Data errors: " << reas_stats.dataErrCnt << std::endl;
    std::cout << "  gRPC errors: " << reas_stats.grpcErrCnt << std::endl;

    // Report lost events
    std::vector<boost::tuple<e2sar::EventNum_t, u_int16_t, size_t>> lostEvents;

    while(true)
    {
        auto res = reassembler.get_LostEvent();
        if (res.has_error())
            break;
        lostEvents.push_back(res.value());
    }

    std::cout << "\tEvents lost so far (<Evt ID:Data ID/num frags rcvd>): ";
    for(auto evt: lostEvents)
    {
        std::cout << "<" << evt.get<0>() << ":" << evt.get<1>() << "/" << evt.get<2>() << "> ";
    }
    std::cout << std::endl;

    return stats.write_errors == 0;
}

CommandLineArgs parseArgs(int argc, char* argv[]) {
    CommandLineArgs args;

    po::options_description desc("ERSAP ET Receiver - Receive E2SAR data and write to ET system");
    desc.add_options()
        ("help,h", "Show this help message")
        ("uri,u", po::value<std::string>(&args.ejfat_uri)->required(),
         "EJFAT URI for E2SAR (required)")
        ("recv-ip,r", po::value<std::string>(&args.recv_ip)->required(),
         "IP address for receiver to listen on (required)")
        ("recv-port", po::value<uint16_t>(&args.recv_port)->default_value(19522),
         "Starting UDP port for receiver (default: 19522)")
        ("recv-threads", po::value<size_t>(&args.recv_threads)->default_value(1),
         "Number of receiver threads (default: 1)")
        ("event-timeout", po::value<int>(&args.event_timeout_ms)->default_value(500),
         "Event reassembly timeout in milliseconds (default: 500)")
        ("withcp,c", po::bool_switch(&args.withCP)->default_value(false),
         "Enable control plane interactions (default: false)")
        ("novalidate,v", po::bool_switch()->default_value(false),
         "Don't validate server SSL certificate (default: validate)")
        ("et-file", po::value<std::string>(&args.et_file)->required(),
         "ET system file path (required)")
        ("et-host", po::value<std::string>(&args.et_host),
         "ET system host (for direct connection)")
        ("et-port", po::value<int>(&args.et_port)->default_value(11111),
         "ET system port (default: 11111)")
        ("et-event-size", po::value<size_t>(&args.et_event_size)->default_value(2097152),
         "ET event buffer size in bytes (default: 2097152 = 2 MB)");

    po::variables_map vm;

    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.count("help")) {
            std::cout << "Usage: " << argv[0] << " -u <ejfat_uri> -r <ip> --et-file <et_file> [OPTIONS]\n\n"
                      << desc << "\n"
                      << "Examples:\n"
                      << "  " << argv[0] << " --uri ejfat://... --recv-ip 127.0.0.1 --et-file /tmp/et_sys\n"
                      << "  " << argv[0] << " -u ejfat://... -r 192.168.1.100 --et-file /tmp/et_sys --et-host localhost --et-port 11111\n"
                      << "  " << argv[0] << " -u ejfat://... -r 10.0.0.1 --et-file /tmp/et_sys --recv-threads 4 --et-event-size 4194304\n";
            std::exit(0);
        }

        po::notify(vm);

        // Validate event timeout
        if (args.event_timeout_ms <= 0) {
            throw std::runtime_error("--event-timeout must be greater than 0");
        }

        // Validate ET event size
        if (args.et_event_size < 1024) {
            throw std::runtime_error("--et-event-size must be at least 1024 bytes");
        }

        // Parse validation flag (inverted logic: novalidate = true means validate = false)
        args.validate = !vm["novalidate"].as<bool>();

    } catch (const po::error& e) {
        std::cerr << "Error: " << e.what() << "\n\n";
        std::cout << "Usage: " << argv[0]
                  << " -u <ejfat_uri> -r <ip> --et-file <et_file> [OPTIONS]\n\n"
                  << desc << std::endl;
        throw;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n\n";
        std::cout << "Usage: " << argv[0]
                  << " -u <ejfat_uri> -r <ip> --et-file <et_file> [OPTIONS]\n\n"
                  << desc << std::endl;
        throw;
    }

    return args;
}

int main(int argc, char* argv[]) {
    try {
        // Parse arguments
        auto args = parseArgs(argc, argv);

        // Install signal handler for Ctrl+C
        signal(SIGINT, signalHandler);

        // Initialize ET system
        ETSystem et;
        if (!initializeET(et, args)) {
            std::cerr << "Failed to initialize ET system" << std::endl;
            return 1;
        }

        // Initialize reassembler
        auto reassembler = initializeReassembler(
            args.ejfat_uri,
            args.recv_ip,
            args.recv_port,
            args.recv_threads,
            args.event_timeout_ms,
            args.withCP,
            args.validate);

        if (!reassembler) {
            std::cerr << "Failed to initialize E2SAR reassembler" << std::endl;
            cleanupET(et);
            return 1;
        }

        // Receive events and write to ET
        bool success = receiveEvents(*reassembler, et);

        // Deregister worker and stop reassembler
        std::cout << "\nDeregistering worker..." << std::endl;
        auto deregres = reassembler->deregisterWorker();
        if (deregres.has_error()) {
            std::cerr << "Unable to deregister worker on exit: "
                      << deregres.error().message() << std::endl;
        }

        std::cout << "Stopping reassembler..." << std::endl;
        reassembler->stopThreads();

        // Cleanup ET system
        cleanupET(et);

        return success ? 0 : 1;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
