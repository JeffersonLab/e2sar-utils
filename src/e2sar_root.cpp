#include <TFile.h>
#include <TTree.h>
#include <TLorentzVector.h>
#include <TVector3.h>
#include <TROOT.h>
#include <boost/program_options.hpp>
#include <e2sar.hpp>
#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sstream>
#include <iomanip>
#include <atomic>
#include <signal.h>
#include <future>
#include <mutex>

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
    std::string tree_name;
    std::vector<std::string> file_paths;
    // E2SAR sending options
    bool send_data = false;
    std::string ejfat_uri;
    uint16_t data_id = 4321;
    uint32_t event_src_id = 1234;
    size_t bufsize_mb = 10;      // Batch size in MB
    uint16_t mtu = 1500;         // MTU for Segmenter
    // E2SAR receiving options
    bool recv_data = false;
    std::string recv_ip;
    uint16_t recv_port = 19522;
    size_t recv_threads = 1;
    std::string output_pattern = "event_{:08d}.dat";  // File naming pattern
    int event_timeout_ms = 500;  // Event reassembly timeout
    bool withCP;
    float rateGbps;
    bool validate;
};

// Structure to hold the four reconstructed particles for one event
struct DalitzEvent {
    TLorentzVector pi_plus;    // π+ (positive pion)
    TLorentzVector pi_minus;   // π- (negative pion)
    TLorentzVector gamma1;     // γ1 (first photon)
    TLorentzVector gamma2;     // γ2 (second photon)
};

// Track statistics during streaming send
struct StreamingStats {
    size_t total_events_processed = 0;
    size_t total_batches_sent = 0;
    size_t total_buffers_sent = 0;
    size_t total_bytes_sent = 0;

    void addBatch(size_t events, size_t buffers, size_t bytes) {
        total_events_processed += events;
        total_batches_sent++;
        total_buffers_sent += buffers;
        total_bytes_sent += bytes;
    }

    void printProgress() const {
        std::cout << "  Batches: " << total_batches_sent
                  << " | Events: " << total_events_processed
                  << " | Buffers: " << total_buffers_sent
                  << " | MB sent: " << (total_bytes_sent / (1024.0 * 1024.0))
                  << std::endl;
    }
};

// Helper function to create a Lorentz vector from spherical coordinates and mass
// Based on set_LorentzVector() from read_dalitz_root.py
TLorentzVector createLorentzVector(Double_t mag, Double_t theta, Double_t phi, Double_t mass) {
    TVector3 vector;
    vector.SetMagThetaPhi(mag, theta, phi);

    TLorentzVector lorentz_vector;
    lorentz_vector.SetVectM(vector, mass);

    return lorentz_vector;
}

// Serialize DalitzEvent to binary (pack 16 doubles: 4 particles x 4-vector each)
void serializeEvent(const DalitzEvent& event, uint8_t* buffer) {
    double* dest = reinterpret_cast<double*>(buffer);
    // π+ (4 doubles: E, px, py, pz)
    dest[0] = event.pi_plus.E();
    dest[1] = event.pi_plus.Px();
    dest[2] = event.pi_plus.Py();
    dest[3] = event.pi_plus.Pz();
    // π- (4 doubles)
    dest[4] = event.pi_minus.E();
    dest[5] = event.pi_minus.Px();
    dest[6] = event.pi_minus.Py();
    dest[7] = event.pi_minus.Pz();
    // γ1 (4 doubles)
    dest[8] = event.gamma1.E();
    dest[9] = event.gamma1.Px();
    dest[10] = event.gamma1.Py();
    dest[11] = event.gamma1.Pz();
    // γ2 (4 doubles)
    dest[12] = event.gamma2.E();
    dest[13] = event.gamma2.Px();
    dest[14] = event.gamma2.Py();
    dest[15] = event.gamma2.Pz();
}

// Append DalitzEvent as 16 doubles to a vector (ensures proper alignment)
void appendEventToVector(const DalitzEvent& event, std::vector<double>& vec) {
    // Reserve space for 16 doubles to avoid multiple reallocations
    vec.reserve(vec.size() + 16);

    // π+ (4 doubles: E, px, py, pz)
    vec.push_back(event.pi_plus.E());
    vec.push_back(event.pi_plus.Px());
    vec.push_back(event.pi_plus.Py());
    vec.push_back(event.pi_plus.Pz());

    // π- (4 doubles)
    vec.push_back(event.pi_minus.E());
    vec.push_back(event.pi_minus.Px());
    vec.push_back(event.pi_minus.Py());
    vec.push_back(event.pi_minus.Pz());

    // γ1 (4 doubles)
    vec.push_back(event.gamma1.E());
    vec.push_back(event.gamma1.Px());
    vec.push_back(event.gamma1.Py());
    vec.push_back(event.gamma1.Pz());

    // γ2 (4 doubles)
    vec.push_back(event.gamma2.E());
    vec.push_back(event.gamma2.Px());
    vec.push_back(event.gamma2.Py());
    vec.push_back(event.gamma2.Pz());
}

// Callback for freeing sent buffers (dynamically allocated vector of doubles)
void freeBuffer(boost::any a) {
    auto* p = boost::any_cast<std::vector<double>*>(a);
    delete p;
}

// Format filename using pattern and event number
// Supports patterns like "event_{:08d}.dat" or "data_{:06d}.bin"
std::string formatFilename(const std::string& pattern, uint64_t event_num) {
    std::ostringstream oss;
    size_t pos = 0;

    while (pos < pattern.size()) {
        size_t start = pattern.find("{:", pos);
        if (start == std::string::npos) {
            // No more format specifiers, copy rest of string
            oss << pattern.substr(pos);
            break;
        }

        // Copy everything before the format specifier
        oss << pattern.substr(pos, start - pos);

        // Find the end of format specifier
        size_t end = pattern.find("}", start);
        if (end == std::string::npos) {
            // Malformed pattern, just copy the rest
            oss << pattern.substr(start);
            break;
        }

        // Extract format spec (e.g., ":08d")
        std::string format_spec = pattern.substr(start + 2, end - start - 3);

        // Parse width (e.g., "08" from "08d")
        int width = 0;
        char fill_char = '0';
        if (format_spec.size() >= 2) {
            if (format_spec[0] == '0') {
                fill_char = '0';
                width = std::stoi(format_spec.substr(0, format_spec.size() - 1));
            } else {
                width = std::stoi(format_spec.substr(0, format_spec.size() - 1));
            }
        }

        // Format the event number
        oss << std::setfill(fill_char) << std::setw(width) << event_num;

        pos = end + 1;
    }

    return oss.str();
}

// Write data to memory-mapped file
// Returns true on success, false on error
bool writeMemoryMappedFile(const std::string& filename, const uint8_t* data, size_t size) {
    // Create and open file
    int fd = open(filename.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        std::cerr << "Error creating file " << filename << ": " << strerror(errno) << std::endl;
        return false;
    }

    // Resize file to match data size
    if (ftruncate(fd, size) < 0) {
        std::cerr << "Error resizing file " << filename << ": " << strerror(errno) << std::endl;
        close(fd);
        return false;
    }

    // Memory-map the file
    void* mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        std::cerr << "Error memory-mapping file " << filename << ": " << strerror(errno) << std::endl;
        close(fd);
        return false;
    }

    // Copy data to mapped memory
    memcpy(mapped, data, size);

    // Sync to disk
    if (msync(mapped, size, MS_SYNC) < 0) {
        std::cerr << "Warning: msync failed for " << filename << ": " << strerror(errno) << std::endl;
    }

    // Cleanup
    munmap(mapped, size);
    close(fd);

    return true;
}

// Initialize and start E2SAR Segmenter
// Returns unique_ptr for RAII-based cleanup
// Also creates LBManager and registers sender when control plane is enabled
std::unique_ptr<e2sar::Segmenter> initializeSegmenter(
    const std::string& uri_str,
    uint16_t data_id,
    uint32_t event_src_id,
    uint16_t mtu,
    bool withCP,
    float rateGbps,
    bool validateCert) {

    std::cout << "\nInitializing E2SAR Segmenter..." << std::endl;

    // Parse EJFAT URI
    auto uri_result = e2sar::EjfatURI::getFromString(uri_str,
        e2sar::EjfatURI::TokenType::instance, false);

    if (uri_result.has_error()) {
        std::cerr << "Error parsing URI: " << uri_result.error().message() << std::endl;
        return nullptr;
    }

    e2sar::EjfatURI uri = uri_result.value();

    // If using control plane, register sender with LBManager
    if (withCP) {
        std::cout << "Registering sender with load balancer..." << std::endl;

        // Create LBManager with validation setting
        e2sar::LBManager lbm(uri, validateCert);

        // Add sender using auto-detected IP address
        auto addres = lbm.addSenderSelf();
        if (addres.has_error()) {
            std::cerr << "Unable to add sender to allow list: " << addres.error().message() << std::endl;
            return nullptr;
        }
        std::cout << "  Sender registered successfully" << std::endl;
    }

    // Create Segmenter with configurable MTU
    e2sar::Segmenter::SegmenterFlags sflags;
    sflags.mtu = mtu;
    sflags.useCP = withCP;
    sflags.numSendSockets = 4;
    sflags.rateGbps = rateGbps;

    auto segmenter = std::make_unique<e2sar::Segmenter>(uri, data_id, event_src_id, sflags);

    // Open and start sending threads
    auto open_result = segmenter->openAndStart();
    if (open_result.has_error()) {
        std::cerr << "Error starting segmenter: " << open_result.error().message() << std::endl;
        return nullptr;
    }

    std::cout << "Segmenter started successfully" << std::endl;
    std::cout << "  MTU: " << segmenter->getMTU() << " bytes" << std::endl;
    std::cout << "  Max payload: " << segmenter->getMaxPldLen() << " bytes" << std::endl;
    std::cout << "  Send rate: " << sflags.rateGbps << " Gbps" << std::endl;

    return segmenter;
}

// Initialize and start E2SAR Reassembler
// Returns unique_ptr for RAII-based cleanup
std::unique_ptr<e2sar::Reassembler> initializeReassembler(
    const std::string& uri_str,
    const std::string& recv_ip,
    uint16_t recv_port,
    size_t num_threads,
    int event_timeout_ms,
    bool withCP) {

    std::cout << "\nInitializing E2SAR Reassembler..." << std::endl;

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
                  << " | Written: " << events_written
                  << " | Errors: " << write_errors
                  << " | Total MB: " << (total_bytes / (1024.0 * 1024.0))
                  << std::endl;
    }
};

// Global flag for signal handling
std::atomic<bool> keep_receiving{true};

// Global atomic buffer ID counter for thread-safe unique IDs
std::atomic<size_t> global_buffer_id{0};

// Mutex for thread-safe console output
std::mutex cout_mutex;

// Thread-safe print helper
void thread_print(size_t file_idx, const std::string& msg) {
    std::lock_guard<std::mutex> lock(cout_mutex);
    std::cout << "[File " << file_idx << "] " << msg << std::endl;
}

void thread_print(size_t file_idx, const std::ostringstream& oss) {
    std::lock_guard<std::mutex> lock(cout_mutex);
    std::cout << "[File " << file_idx << "] " << oss.str() << std::endl;
}

// Receive events and write to memory-mapped files
bool receiveEvents(e2sar::Reassembler& reassembler,
                   const std::string& output_pattern) {

    std::cout << "\nStarting event reception..." << std::endl;
    std::cout << "Output pattern: " << output_pattern << std::endl;
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

        // Generate filename from pattern
        std::string filename = formatFilename(output_pattern, event_num);

        // Write to memory-mapped file
        if (writeMemoryMappedFile(filename, event_buffer, event_size)) {
            stats.events_written++;
        } else {
            stats.write_errors++;
            std::cerr << "Failed to write event " << event_num << std::endl;
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
    std::cout << "Events written: " << stats.events_written << std::endl;
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

    po::options_description desc("ROOT File Reader - Extract named trees from ROOT files and send/receive via E2SAR");
    desc.add_options()
        ("help,h", "Show this help message")
        ("tree,t", po::value<std::string>(&args.tree_name),
         "Name of the tree to extract (required for sender mode)")
        ("send,s", po::bool_switch(&args.send_data)->default_value(false),
         "Enable E2SAR network sending")
        ("recv,r", po::bool_switch(&args.recv_data)->default_value(false),
         "Enable E2SAR network receiving")
        ("uri,u", po::value<std::string>(&args.ejfat_uri),
         "EJFAT URI for E2SAR (required for --send or --recv)")
        ("dataid", po::value<uint16_t>(&args.data_id)->default_value(1),
         "Data ID for E2SAR (default: 1)")
        ("eventsrcid", po::value<uint32_t>(&args.event_src_id)->default_value(1),
         "Event source ID for E2SAR (default: 1)")
        ("bufsize-mb", po::value<size_t>(&args.bufsize_mb)->default_value(10),
         "Batch size in MB for streaming (default: 10)")
        ("mtu", po::value<uint16_t>(&args.mtu)->default_value(1500),
         "MTU size in bytes for E2SAR segmenter (default: 1500)")
        ("recv-ip", po::value<std::string>(&args.recv_ip),
         "IP address for receiver to listen on (required for --recv)")
        ("recv-port", po::value<uint16_t>(&args.recv_port)->default_value(19522),
         "Starting UDP port for receiver (default: 19522)")
        ("recv-threads", po::value<size_t>(&args.recv_threads)->default_value(1),
         "Number of receiver threads (default: 1)")
        ("output-pattern,o", po::value<std::string>(&args.output_pattern)->default_value("event_{:08d}.dat"),
         "Output file naming pattern for received events (default: event_{:08d}.dat)")
        ("event-timeout", po::value<int>(&args.event_timeout_ms)->default_value(500),
         "Event reassembly timeout in milliseconds (default: 500)")
        ("files", po::value<std::vector<std::string>>(&args.file_paths),
         "ROOT files to process (required for sender mode)")
        ("withcp,c", po::bool_switch()->default_value(false),
            "enable control plane interactions")
        ("rate", po::value<float>(&args.rateGbps)->default_value(1.0),
            "send rate in Gbps (defaults to 1.0, negative value means no limit)")
        ("novalidate,v", po::bool_switch()->default_value(false),
            "don't validate server SSL certificate");


    po::positional_options_description pos;
    pos.add("files", -1);

    po::variables_map vm;

    try {
        po::store(po::command_line_parser(argc, argv)
                  .options(desc)
                  .positional(pos)
                  .run(), vm);

        if (vm.count("help")) {
            std::cout << "Usage:\n"
                      << "  Sender: " << argv[0] << " --tree <tree_name> --send --uri <ejfat_uri> [OPTIONS] <file1.root> ...\n"
                      << "  Receiver: " << argv[0] << " --recv --uri <ejfat_uri> --recv-ip <ip> [OPTIONS]\n\n"
                      << desc << "\n"
                      << "Examples:\n"
                      << "  Read only: " << argv[0] << " --tree dalitz_root_tree data/file.root\n"
                      << "  Send:      " << argv[0] << " -t dalitz_root_tree --send -u ejfat://... --bufsize-mb 5 file.root\n"
                      << "  Send (jumbo): " << argv[0] << " -t dalitz_root_tree --send -u ejfat://... --mtu 9000 file.root\n"
                      << "  Receive:   " << argv[0] << " --recv -u ejfat://... --recv-ip 127.0.0.1 -o output_{:06d}.dat\n";
            std::exit(0);
        }

        po::notify(vm);  // Throws if required options missing

        // Validate mode selection
        if (args.send_data && args.recv_data) {
            throw std::runtime_error("Cannot use --send and --recv simultaneously");
        }

        // Validate sender requirements
        if (args.send_data) {
            if (args.ejfat_uri.empty()) {
                throw std::runtime_error("--uri is required when --send is enabled");
            }
            if (args.tree_name.empty()) {
                throw std::runtime_error("--tree is required when --send is enabled");
            }
            if (args.file_paths.empty()) {
                throw std::runtime_error("ROOT file(s) required when --send is enabled");
            }
            if (args.bufsize_mb == 0) {
                throw std::runtime_error("--bufsize-mb must be greater than 0");
            }
            if (args.mtu < 576 || args.mtu > 9000) {
                throw std::runtime_error("--mtu must be between 576 and 9000 bytes");
            }
        }

        // Validate receiver requirements
        if (args.recv_data) {
            if (args.ejfat_uri.empty()) {
                throw std::runtime_error("--uri is required when --recv is enabled");
            }
            if (args.recv_ip.empty()) {
                throw std::runtime_error("--recv-ip is required when --recv is enabled");
            }
            if (args.event_timeout_ms <= 0) {
                throw std::runtime_error("--event-timeout must be greater than 0");
            }
        }

        // Validate read-only mode (no send, no recv)
        if (!args.send_data && !args.recv_data) {
            if (args.tree_name.empty()) {
                throw std::runtime_error("--tree is required for read-only mode");
            }
            if (args.file_paths.empty()) {
                throw std::runtime_error("ROOT file(s) required for read-only mode");
            }
        }

    } catch (const po::error& e) {
        std::cerr << "Error: " << e.what() << "\n\n";
        std::cout << "Usage: " << argv[0]
                  << " --tree <tree_name> [--send --uri <ejfat_uri>] [--bufsize-mb N] [--mtu N] <file1.root> ...\n\n"
                  << desc << std::endl;
        throw;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n\n";
        std::cout << "Usage: " << argv[0]
                  << " --tree <tree_name> [--send --uri <ejfat_uri>] [--bufsize-mb N] [--mtu N] <file1.root> ...\n\n"
                  << desc << std::endl;
        throw;
    }

    args.withCP = vm["withcp"].as<bool>();
    args.validate = !vm["novalidate"].as<bool>();

    return args;
}

bool processRootFile(const std::string& file_path, const std::string& tree_name,
                     const CommandLineArgs& args, e2sar::Segmenter* segmenter,
                     size_t file_index) {
    // Open ROOT file
    auto file = std::unique_ptr<TFile>(TFile::Open(file_path.c_str(), "READ"));

    // Check if file opened successfully
    if (!file || file->IsZombie()) {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cerr << "[File " << file_index << "] Error: Cannot open file " << file_path << std::endl;
        return false;
    }

    // Get the tree
    TTree* tree = file->Get<TTree>(tree_name.c_str());

    // Check if tree exists
    if (!tree) {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cerr << "[File " << file_index << "] Error: Tree '" << tree_name
                  << "' not found in file " << file_path << std::endl;
        return false;
    }

    // Get number of entries
    Long64_t nEntries = tree->GetEntries();
    {
        std::ostringstream oss;
        oss << "Found tree '" << tree_name << "' with " << nEntries << " entries";
        thread_print(file_index, oss);
    }

    // Branch variables for positive pion (π+)
    Double_t mag_plus_rec, theta_plus_rec, phi_plus_rec;

    // Branch variables for negative pion (π-)
    Double_t mag_neg_rec, theta_neg_rec, phi_neg_rec;

    // Branch variables for first photon (γ1)
    Double_t mag_neutral1_rec, theta_neutral1_rec, phi_neutral1_rec;

    // Branch variables for second photon (γ2)
    Double_t mag_neutral2_rec, theta_neutral2_rec, phi_neutral2_rec;

    // Set branch addresses
    tree->SetBranchAddress("mag_plus_rec", &mag_plus_rec);
    tree->SetBranchAddress("theta_plus_rec", &theta_plus_rec);
    tree->SetBranchAddress("phi_plus_rec", &phi_plus_rec);

    tree->SetBranchAddress("mag_neg_rec", &mag_neg_rec);
    tree->SetBranchAddress("theta_neg_rec", &theta_neg_rec);
    tree->SetBranchAddress("phi_neg_rec", &phi_neg_rec);

    tree->SetBranchAddress("mag_neutral1_rec", &mag_neutral1_rec);
    tree->SetBranchAddress("theta_neutral1_rec", &theta_neutral1_rec);
    tree->SetBranchAddress("phi_neutral1_rec", &phi_neutral1_rec);

    tree->SetBranchAddress("mag_neutral2_rec", &mag_neutral2_rec);
    tree->SetBranchAddress("theta_neutral2_rec", &theta_neutral2_rec);
    tree->SetBranchAddress("phi_neutral2_rec", &phi_neutral2_rec);

    // Particle masses (in GeV/c²)
    const Double_t PION_MASS = 0.139;   // π± mass
    const Double_t PHOTON_MASS = 0.0;   // γ mass

    // ========== Streaming setup ==========

    // Calculate batch size in events
    const size_t EVENT_SIZE = 128;  // 16 doubles * 8 bytes
    const size_t BATCH_SIZE_BYTES = args.bufsize_mb * 1024 * 1024;
    const size_t BATCH_SIZE_EVENTS = BATCH_SIZE_BYTES / EVENT_SIZE;

    {
        std::ostringstream oss;
        oss << "Batch size: " << args.bufsize_mb << " MB (" << BATCH_SIZE_EVENTS << " events)";
        thread_print(file_index, oss);
    }

    // Local statistics for this file
    StreamingStats stats;

    // ========== Streaming read-send loop ==========

    {
        std::ostringstream oss;
        oss << "Streaming " << nEntries << " events...";
        thread_print(file_index, oss);
    }

    DalitzEvent first_event;  // Save for sample output
    bool saved_first = false;

    // Allocate first batch dynamically (vector of doubles for proper alignment)
    auto* batch = new std::vector<double>();
    batch->reserve(BATCH_SIZE_EVENTS * 16);  // 16 doubles per event
    size_t events_in_batch = 0;

    for (Long64_t i = 0; i < nEntries; ++i) {
        // Read event from ROOT
        tree->GetEntry(i);

        DalitzEvent event;
        event.pi_plus = createLorentzVector(mag_plus_rec, theta_plus_rec, phi_plus_rec, PION_MASS);
        event.pi_minus = createLorentzVector(mag_neg_rec, theta_neg_rec, phi_neg_rec, PION_MASS);
        event.gamma1 = createLorentzVector(mag_neutral1_rec, theta_neutral1_rec, phi_neutral1_rec, PHOTON_MASS);
        event.gamma2 = createLorentzVector(mag_neutral2_rec, theta_neutral2_rec, phi_neutral2_rec, PHOTON_MASS);

        // Save first event for sample output
        if (!saved_first) {
            first_event = event;
            saved_first = true;
        }

        // Append event as 16 doubles to batch vector (proper alignment)
        appendEventToVector(event, *batch);
        events_in_batch++;

        // When batch is full OR last event, send it
        if (events_in_batch >= BATCH_SIZE_EVENTS || i == nEntries - 1) {

            // Send batch if E2SAR enabled and segmenter is available
            if (args.send_data && segmenter) {
                uint8_t* buffer_ptr = reinterpret_cast<uint8_t*>(batch->data());
                size_t buffer_size = batch->size() * sizeof(double);

                // Get unique buffer ID from global atomic counter
                size_t current_buffer_id = global_buffer_id.fetch_add(1);

                // Send buffer via E2SAR with retry on queue full
                bool sent = false;
                int retry_count = 0;
                const int MAX_RETRIES = 10000;

                while (!sent && retry_count < MAX_RETRIES) {
                    auto send_result = segmenter->addToSendQueue(buffer_ptr, buffer_size,
                        current_buffer_id, 0, 0, &freeBuffer, batch);

                    if (send_result.has_error()) {
                        if (send_result.error().code() == e2sar::E2SARErrorc::MemoryError) {
                            // Queue full, retry after short sleep
                            std::this_thread::sleep_for(std::chrono::microseconds(100));
                            retry_count++;
                            continue;
                        } else {
                            std::lock_guard<std::mutex> lock(cout_mutex);
                            std::cerr << "[File " << file_index << "] Send error: "
                                      << send_result.error().message() << std::endl;
                            delete batch;
                            return false;
                        }
                    }
                    sent = true;
                }

                if (!sent) {
                    std::lock_guard<std::mutex> lock(cout_mutex);
                    std::cerr << "[File " << file_index << "] Failed to send buffer after "
                              << MAX_RETRIES << " retries" << std::endl;
                    delete batch;
                    return false;
                }

                stats.addBatch(events_in_batch, 1, buffer_size);

                // Progress update every 10 batches
                if (stats.total_batches_sent % 10 == 0) {
                    std::lock_guard<std::mutex> lock(cout_mutex);
                    std::cout << "[File " << file_index << "] ";
                    stats.printProgress();
                }
            } else if (!args.send_data) {
                // If not sending, just delete the batch
                delete batch;
            }

            // Allocate new batch for next iteration (if not last event)
            if (i < nEntries - 1) {
                batch = new std::vector<double>();
                batch->reserve(BATCH_SIZE_EVENTS * 16);  // 16 doubles per event
                events_in_batch = 0;
            }
        }

        // Progress indicator for large files (reading progress)
        if ((i + 1) % 500000 == 0) {
            std::ostringstream oss;
            oss << "Read " << (i + 1) << " / " << nEntries << " events";
            thread_print(file_index, oss);
        }
    }

    {
        std::ostringstream oss;
        oss << "Successfully processed " << nEntries << " events from " << file_path;
        thread_print(file_index, oss);
    }

    // ========== Sample output (use saved first_event) ==========

    if (saved_first) {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "[File " << file_index << "] Sample (first event):" << std::endl;
        std::cout << "[File " << file_index << "]   π+ : E=" << first_event.pi_plus.E()
                  << " GeV, p=(" << first_event.pi_plus.Px()
                  << ", " << first_event.pi_plus.Py()
                  << ", " << first_event.pi_plus.Pz() << ") GeV/c" << std::endl;
        std::cout << "[File " << file_index << "]   π- : E=" << first_event.pi_minus.E()
                  << " GeV, p=(" << first_event.pi_minus.Px()
                  << ", " << first_event.pi_minus.Py()
                  << ", " << first_event.pi_minus.Pz() << ") GeV/c" << std::endl;
        std::cout << "[File " << file_index << "]   γ1 : E=" << first_event.gamma1.E()
                  << " GeV, p=(" << first_event.gamma1.Px()
                  << ", " << first_event.gamma1.Py()
                  << ", " << first_event.gamma1.Pz() << ") GeV/c" << std::endl;
        std::cout << "[File " << file_index << "]   γ2 : E=" << first_event.gamma2.E()
                  << " GeV, p=(" << first_event.gamma2.Px()
                  << ", " << first_event.gamma2.Py()
                  << ", " << first_event.gamma2.Pz() << ") GeV/c" << std::endl;
    }

    // ========== Per-file completion stats ==========

    if (args.send_data && segmenter) {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "[File " << file_index << "] ========== File Processing Complete ==========" << std::endl;
        std::cout << "[File " << file_index << "] Events processed: " << stats.total_events_processed << std::endl;
        std::cout << "[File " << file_index << "] Batches sent: " << stats.total_batches_sent << std::endl;
        std::cout << "[File " << file_index << "] Data volume: " << (stats.total_bytes_sent / (1024.0 * 1024.0)) << " MB" << std::endl;
    }

    // Note: Segmenter stats are printed in main() after all threads complete

    // File closes automatically when unique_ptr goes out of scope
    return true;
}

int main(int argc, char* argv[]) {
    // Enable ROOT thread safety for parallel file processing
    ROOT::EnableThreadSafety();

    try {
        // Parse arguments
        auto args = parseArgs(argc, argv);

        // Receiver mode
        if (args.recv_data) {
            // Install signal handler for Ctrl+C
            signal(SIGINT, signalHandler);

            // Initialize reassembler
            auto reassembler = initializeReassembler(
                args.ejfat_uri,
                args.recv_ip,
                args.recv_port,
                args.recv_threads,
                args.event_timeout_ms,
                args.withCP);

            if (!reassembler) {
                std::cerr << "Failed to initialize E2SAR reassembler" << std::endl;
                return 1;
            }

            // Receive events and write to files
            bool success = receiveEvents(*reassembler, args.output_pattern);

            // Deregister worker and stop reassembler
            std::cout << "\nDeregistering worker..." << std::endl;
            auto deregres = reassembler->deregisterWorker();
            if (deregres.has_error()) {
                std::cerr << "Unable to deregister worker on exit: "
                          << deregres.error().message() << std::endl;
            }

            std::cout << "Stopping reassembler..." << std::endl;
            reassembler->stopThreads();

            return success ? 0 : 1;
        }

        // Sender/Read-only mode
        int success_count = 0;
        int failure_count = 0;

        // Create shared Segmenter if sending
        std::unique_ptr<e2sar::Segmenter> segmenter;

        if (args.send_data) {
            std::cout << "Initializing shared E2SAR Segmenter for "
                      << args.file_paths.size() << " file(s)..." << std::endl;

            segmenter = initializeSegmenter(args.ejfat_uri, args.data_id,
                                           args.event_src_id, args.mtu,
                                           args.withCP, args.rateGbps,
                                           args.validate);
            if (!segmenter) {
                std::cerr << "Failed to initialize E2SAR segmenter" << std::endl;
                return 1;
            }

            std::cout << "Segmenter ready. Max payload: "
                      << segmenter->getMaxPldLen() << " bytes" << std::endl;
        }

        // Reset global buffer ID counter
        global_buffer_id = 0;

        // Spawn threads for parallel file processing
        std::cout << "\nSpawning " << args.file_paths.size()
                  << " thread(s) for file processing..." << std::endl;

        std::vector<std::future<bool>> futures;
        for (size_t i = 0; i < args.file_paths.size(); ++i) {
            std::cout << "  Thread " << i << ": " << args.file_paths[i] << std::endl;

            futures.push_back(std::async(std::launch::async,
                processRootFile,
                args.file_paths[i],
                args.tree_name,
                std::cref(args),
                segmenter.get(),
                i));
        }

        // Wait for all threads to complete
        std::cout << "\nWaiting for all threads to complete..." << std::endl;
        for (size_t i = 0; i < futures.size(); ++i) {
            bool result = futures[i].get();
            if (result) {
                success_count++;
            } else {
                failure_count++;
                std::cerr << "Thread " << i << " failed" << std::endl;
            }
        }

        // Print final Segmenter statistics if sending
        if (segmenter) {
            // Give the segmenter time to flush remaining data
            std::cout << "\nWaiting for send queues to drain..." << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            auto send_stats = segmenter->getSendStats();

            std::cout << "\n========== E2SAR Final Statistics ==========" << std::endl;
            std::cout << "Total network frames sent: " << send_stats.msgCnt << std::endl;
            std::cout << "Send errors: " << send_stats.errCnt << std::endl;
            std::cout << "Total buffers submitted: " << global_buffer_id.load() << std::endl;

            if (send_stats.errCnt > 0) {
                std::cerr << "WARNING: Errors occurred during sending" << std::endl;
            }
        }

        // Summary
        std::cout << "\nProcessing complete: "
                  << success_count << " file(s) processed successfully";
        if (failure_count > 0) {
            std::cout << ", " << failure_count << " file(s) failed";
        }
        std::cout << std::endl;

        return failure_count > 0 ? 1 : 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
