#include "file_processor.hpp"
#include <TFile.h>
#include <TTree.h>
#include <TROOT.h>
#include <boost/program_options.hpp>
#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <sstream>
#include <iomanip>
#include <atomic>
#include <signal.h>
#include <future>
#include <filesystem>

namespace po = boost::program_options;

// Global flag for signal handling
std::atomic<bool> keep_receiving{true};

// Signal handler for Ctrl+C
void signalHandler(int signal) {
    if (signal == SIGINT) {
        std::cout << "\nReceived interrupt signal, stopping..." << std::endl;
        keep_receiving = false;
    }
}

// Format filename using pattern and event number
// Supports patterns like "event_{:08d}.dat" or "data_{:06d}.bin"
std::string formatFilename(const std::string& pattern, uint64_t event_num) {
    std::ostringstream oss;
    size_t pos = 0;

    while (pos < pattern.size()) {
        size_t start = pattern.find("{:", pos);
        if (start == std::string::npos) {
            oss << pattern.substr(pos);
            break;
        }

        oss << pattern.substr(pos, start - pos);

        size_t end = pattern.find("}", start);
        if (end == std::string::npos) {
            oss << pattern.substr(start);
            break;
        }

        std::string format_spec = pattern.substr(start + 2, end - start - 3);

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

        oss << std::setfill(fill_char) << std::setw(width) << event_num;
        pos = end + 1;
    }

    return oss.str();
}

// Write data to memory-mapped file
bool writeMemoryMappedFile(const std::string& filename, const uint8_t* data, size_t size) {
    int fd = open(filename.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        std::cerr << "Error creating file " << filename << ": " << strerror(errno) << std::endl;
        return false;
    }

    if (ftruncate(fd, size) < 0) {
        std::cerr << "Error resizing file " << filename << ": " << strerror(errno) << std::endl;
        close(fd);
        return false;
    }

    void* mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        std::cerr << "Error memory-mapping file " << filename << ": " << strerror(errno) << std::endl;
        close(fd);
        return false;
    }

    memcpy(mapped, data, size);

    if (msync(mapped, size, MS_SYNC) < 0) {
        std::cerr << "Warning: msync failed for " << filename << ": " << strerror(errno) << std::endl;
    }

    munmap(mapped, size);
    close(fd);
    return true;
}

static bool readAll(int fd, void* buf, size_t n) {
    auto* p = static_cast<uint8_t*>(buf);
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) return false;
        p += r; n -= r;
    }
    return true;
}

void freeRawBuffer(boost::any a) {
    delete[] boost::any_cast<uint8_t*>(a);
}

static const int PIPE_MAX_RETRIES = 10000;

bool parentReaderLoop(int pipe_read_fd, e2sar::Segmenter* seg, size_t file_idx) {
    while (true) {
        uint64_t nbytes = 0;
        if (!readAll(pipe_read_fd, &nbytes, sizeof(nbytes))) {
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cerr << "[Pipe " << file_idx << "] Read error on size header" << std::endl;
            close(pipe_read_fd);
            return false;
        }
        if (nbytes == 0) break;  // end-of-stream sentinel

        auto* buf = new uint8_t[nbytes];
        if (!readAll(pipe_read_fd, buf, nbytes)) {
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cerr << "[Pipe " << file_idx << "] Read error on batch data" << std::endl;
            delete[] buf;
            close(pipe_read_fd);
            return false;
        }

        size_t cur_id = global_buffer_id.fetch_add(1);
        bool sent = false;
        int retries = 0;
        while (!sent && retries < PIPE_MAX_RETRIES) {
            auto res = seg->addToSendQueue(buf, nbytes, cur_id, 0, 0,
                                           &freeRawBuffer, buf);
            if (!res.has_error()) {
                sent = true;
            } else if (res.error().code() == e2sar::E2SARErrorc::MemoryError) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                retries++;
            } else {
                std::lock_guard<std::mutex> lock(cout_mutex);
                std::cerr << "[Pipe " << file_idx << "] Send error: "
                          << res.error().message() << std::endl;
                delete[] buf;
                close(pipe_read_fd);
                return false;
            }
        }
        if (!sent) {
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cerr << "[Pipe " << file_idx << "] Failed to send after "
                      << PIPE_MAX_RETRIES << " retries" << std::endl;
            delete[] buf;
            close(pipe_read_fd);
            return false;
        }
    }
    close(pipe_read_fd);
    return true;
}

// Initialize and start E2SAR Segmenter
std::unique_ptr<e2sar::Segmenter> initializeSegmenter(
    const std::string& uri_str,
    uint16_t data_id,
    uint32_t event_src_id,
    uint16_t mtu,
    bool withCP,
    float rateGbps,
    uint32_t num_send_sockets,
    bool validateCert,
    const std::string& send_ip) {

    std::cout << "\nInitializing E2SAR Segmenter..." << std::endl;

    auto uri_result = e2sar::EjfatURI::getFromString(uri_str,
        e2sar::EjfatURI::TokenType::instance, false);

    if (uri_result.has_error()) {
        std::cerr << "Error parsing URI: " << uri_result.error().message() << std::endl;
        return nullptr;
    }

    e2sar::EjfatURI uri = uri_result.value();

    if (withCP) {
        e2sar::LBManager lbm(uri, validateCert);
        if (send_ip.empty()) {
            std::cout << "Auto-Registering sender with load balancer..." << std::endl;
            auto addres = lbm.addSenderSelf();
            if (addres.has_error()) {
                std::cerr << "Unable to add sender to allow list: " << addres.error().message() << std::endl;
                return nullptr;
            }
        } else {
            std::cout << "Registering sender with ip " << send_ip << " with load balancer..." << std::endl;
            std::vector<std::string> senders;
            senders.push_back(send_ip);
            auto addres = lbm.addSenders(senders);
            if (addres.has_error()) {
                std::cerr << "Unable to add sender to allow list: " << addres.error().message() << std::endl;
                return nullptr;
            }
        }
        std::cout << "  Sender registered successfully" << std::endl;
    }

    e2sar::Segmenter::SegmenterFlags sflags;
    sflags.mtu = mtu;
    sflags.useCP = withCP;
    sflags.numSendSockets = num_send_sockets;
    sflags.rateGbps = rateGbps;

    auto segmenter = std::make_unique<e2sar::Segmenter>(uri, data_id, event_src_id, sflags);

    auto open_result = segmenter->openAndStart();
    if (open_result.has_error()) {
        std::cerr << "Error starting segmenter: " << open_result.error().message() << std::endl;
        return nullptr;
    }

    std::cout << "Segmenter started successfully" << std::endl;
    std::cout << "  MTU: " << segmenter->getMTU() << " bytes" << std::endl;
    std::cout << "  Max payload: " << segmenter->getMaxPldLen() << " bytes" << std::endl;
    std::cout << "  Send rate: " << sflags.rateGbps << " Gbps" << std::endl;
    std::cout << "  Send sockets: " << sflags.numSendSockets << std::endl;

    return segmenter;
}

// Initialize and start E2SAR Reassembler
std::unique_ptr<e2sar::Reassembler> initializeReassembler(
    const std::string& uri_str,
    const std::string& recv_ip,
    uint16_t recv_port,
    size_t num_threads,
    int event_timeout_ms,
    bool withCP,
    bool validateCert) {

    std::cout << "\nInitializing E2SAR Reassembler..." << std::endl;

    auto uri_result = e2sar::EjfatURI::getFromString(uri_str,
        e2sar::EjfatURI::TokenType::instance, false);

    if (uri_result.has_error()) {
        std::cerr << "Error parsing URI: " << uri_result.error().message() << std::endl;
        return nullptr;
    }

    e2sar::EjfatURI uri = uri_result.value();

    e2sar::Reassembler::ReassemblerFlags rflags;
    rflags.useCP = withCP;
    rflags.withLBHeader = !withCP;
    rflags.eventTimeout_ms = event_timeout_ms;
    rflags.validateCert = validateCert;

    std::unique_ptr<e2sar::Reassembler> reassembler;
    try {
        if (recv_ip.empty()) {
            std::cout << "No --recv-ip specified, auto-detecting interface..." << std::endl;
            reassembler = std::make_unique<e2sar::Reassembler>(
                uri, recv_port, num_threads, rflags);
        } else {
            boost::asio::ip::address ip;
            try {
                ip = boost::asio::ip::make_address(recv_ip);
            } catch (const std::exception& e) {
                std::cerr << "Error parsing IP address: " << e.what() << std::endl;
                return nullptr;
            }
            reassembler = std::make_unique<e2sar::Reassembler>(
                uri, ip, recv_port, num_threads, rflags);
        }
    } catch (const e2sar::E2SARException& e) {
        std::cerr << "Error initializing Reassembler: " << e.what() << std::endl;
        return nullptr;
    }

    std::cout << "Using IP address: " << reassembler->get_dataIP() << std::endl;
    std::cout << "Receiving on ports: " << reassembler->get_recvPorts().first
              << ":" << reassembler->get_recvPorts().second << std::endl;

    auto hostname_res = e2sar::NetUtil::getHostName();
    if (!hostname_res.has_error()) {
        auto regres = reassembler->registerWorker(hostname_res.value());
        if (regres.has_error()) {
            std::cerr << "Warning: Unable to register worker: "
                      << regres.error().message() << std::endl;
        }
    }

    auto open_result = reassembler->openAndStart();
    if (open_result.has_error()) {
        std::cerr << "Error starting reassembler: " << open_result.error().message() << std::endl;
        return nullptr;
    }

    std::cout << "Reassembler started successfully" << std::endl;
    std::cout << "  Batch reassembly timeout: " << event_timeout_ms << " ms" << std::endl;
    std::cout << "  Receive threads: " << num_threads << std::endl;

    return reassembler;
}

// Statistics for receiving events
struct ReceiveStats {
    std::atomic<uint64_t> events_received{0};
    std::atomic<uint64_t> events_written{0};
    std::atomic<uint64_t> write_errors{0};
    std::atomic<uint64_t> total_bytes{0};
    std::atomic<uint64_t> data_id_mismatches{0};

    void printProgress() const {
        std::cout << "  Batches received: " << events_received
                  << " | Written: " << events_written
                  << " | Errors: " << write_errors
                  << " | DataID mismatches: " << data_id_mismatches
                  << " | Total MB: " << (total_bytes / (1024.0 * 1024.0))
                  << std::endl;
    }
};

// Receive events and write to memory-mapped files
bool receiveEvents(e2sar::Reassembler& reassembler, const std::string& output_pattern,
                   uint16_t expected_data_id) {
    std::cout << "\nStarting batch reception..." << std::endl;
    std::cout << "Output pattern: " << output_pattern << std::endl;
    std::cout << "Press Ctrl+C to stop\n" << std::endl;

    ReceiveStats stats;
    uint8_t* event_buffer = nullptr;
    size_t event_size;
    e2sar::EventNum_t event_num;
    uint16_t data_id;

    auto start_time   = std::chrono::steady_clock::now();
    auto last_progress = start_time;

    while (keep_receiving) {
        auto result = reassembler.recvEvent(&event_buffer, &event_size,
                                            &event_num, &data_id, 1000);

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_progress).count() >= 5) {
            stats.printProgress();
            last_progress = now;
        }

        if (result.has_error()) continue;
        if (result.value() == -1) continue;

        if (data_id != expected_data_id) {
            stats.data_id_mismatches++;
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cerr << "Warning: received data_id=" << data_id
                      << " expected=" << expected_data_id
                      << " event_num=" << event_num << std::endl;
            delete[] event_buffer;
            event_buffer = nullptr;
            continue;
        }

        stats.events_received++;
        stats.total_bytes += event_size;

        std::string filename = formatFilename(output_pattern, event_num);

        if (writeMemoryMappedFile(filename, event_buffer, event_size)) {
            stats.events_written++;
        } else {
            stats.write_errors++;
            std::cerr << "Failed to write event " << event_num << std::endl;
        }

        delete[] event_buffer;
        event_buffer = nullptr;
    }

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    std::cout << "\n========== Reception Complete ==========" << std::endl;
    std::cout << "Batches received: "       << stats.events_received    << std::endl;
    std::cout << "Batches written: "        << stats.events_written     << std::endl;
    std::cout << "Write errors: "          << stats.write_errors       << std::endl;
    std::cout << "DataID mismatches: "     << stats.data_id_mismatches << std::endl;
    std::cout << "Total data: "            << (stats.total_bytes / (1024.0 * 1024.0)) << " MB" << std::endl;
    std::cout << "Duration: "        << duration.count() << " ms" << std::endl;

    if (stats.events_received > 0) {
        double mbps = (stats.total_bytes * 8.0 / 1000000.0) / (duration.count() / 1000.0);
        std::cout << "Average rate: " << mbps << " Mbps" << std::endl;
    }

    auto reas_stats = reassembler.getStats();
    std::cout << "\nReassembler Statistics:" << std::endl;
    std::cout << "  Total packets: "    << reas_stats.totalPackets    << std::endl;
    std::cout << "  Total bytes: "      << reas_stats.totalBytes      << std::endl;
    std::cout << "  Batch success: "    << reas_stats.eventSuccess    << std::endl;
    std::cout << "  Batch loss: "  << reas_stats.reassemblyLoss  << std::endl;
    std::cout << "  Enqueue loss: "     << reas_stats.enqueueLoss     << std::endl;
    std::cout << "  Data errors: "      << reas_stats.dataErrCnt      << std::endl;
    std::cout << "  gRPC errors: "      << reas_stats.grpcErrCnt      << std::endl;

    std::vector<boost::tuple<e2sar::EventNum_t, u_int16_t, size_t>> lostEvents;
    while (true) {
        auto res = reassembler.get_LostEvent();
        if (res.has_error()) break;
        lostEvents.push_back(res.value());
    }

    std::cout << "\tBatches lost so far (<Evt ID:Data ID/num frags rcvd>): ";
    for (auto evt : lostEvents) {
        std::cout << "<" << evt.get<0>() << ":" << evt.get<1>() << "/" << evt.get<2>() << "> ";
    }
    std::cout << std::endl;

    return stats.write_errors == 0 && stats.data_id_mismatches == 0;
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
        ("dataid", po::value<uint16_t>(&args.data_id)->default_value(0),
         "Data ID for E2SAR (default: 0)")
        ("eventsrcid", po::value<uint32_t>(&args.event_src_id)->default_value(1),
         "Event source ID for E2SAR (default: 1)")
        ("bufsize-mb", po::value<size_t>(&args.bufsize_mb)->default_value(10),
         "Batch size in MB for streaming (default: 10)")
        ("mtu", po::value<uint16_t>(&args.mtu)->default_value(1500),
         "MTU size in bytes for E2SAR segmenter (default: 1500)")
        ("recv-ip", po::value<std::string>(&args.recv_ip),
         "IP address for receiver to listen on (optional; auto-detected if omitted)")
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
        ("toy", po::bool_switch(&args.use_toy)->default_value(false),
         "Use Dalitz toy-MC event schema (dalitz_root_tree branches)")
        ("gluex", po::bool_switch(&args.use_gluex)->default_value(false),
         "Use GlueX kinematic-fit event schema (myTree branches)")
        ("withcp,c", po::bool_switch()->default_value(false),
         "enable control plane interactions")
        ("rate", po::value<float>(&args.rateGbps)->default_value(1.0),
         "send rate in Gbps (defaults to 1.0, negative value means no limit)")
        ("numsocks", po::value<uint32_t>(&args.num_send_sockets)->default_value(4),
         "number of send sockets/threads in the Segmenter (default: 4)")
        ("novalidate,v", po::bool_switch()->default_value(false),
         "don't validate server SSL certificate")
        ("dir", po::value<std::string>(&args.dir_path),
         "Directory of *.root files to process (mutually exclusive with positional files)")
        ("parallel", po::value<uint32_t>(&args.parallel_streams)->default_value(4),
         "Max concurrent file streams when sending (default: 4)")
        ("send-ip", po::value<std::string>(&args.send_ip),
         "IP address for sender to register (optional; auto-detected if omitted)");

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
                      << "  Sender (files): " << argv[0] << " --toy|--gluex --tree <tree_name> --send --uri <ejfat_uri> [--parallel N] [OPTIONS] <file1.root> ...\n"
                      << "  Sender (dir):   " << argv[0] << " --toy|--gluex --tree <tree_name> --send --uri <ejfat_uri> --dir <dir> --parallel N [OPTIONS]\n"
                      << "  Receiver:       " << argv[0] << " --recv --uri <ejfat_uri> [--recv-ip <ip>] [OPTIONS]\n\n"
                      << desc << "\n"
                      << "Examples:\n"
                      << "  Read only (toy):   " << argv[0] << " --toy  --tree dalitz_root_tree data/file.root\n"
                      << "  Read only (dir):   " << argv[0] << " --toy  --tree dalitz_root_tree --dir data/\n"
                      << "  Send (toy):        " << argv[0] << " --toy  -t dalitz_root_tree --send -u ejfat://... --bufsize-mb 5 file.root\n"
                      << "  Send (dir):        " << argv[0] << " --toy  -t dalitz_root_tree --send -u ejfat://... --dir data/ --parallel 8\n"
                      << "  Send (gluex):      " << argv[0] << " --gluex -t myTree          --send -u ejfat://... --bufsize-mb 5 file.root\n"
                      << "  Send (jumbo):      " << argv[0] << " --toy  -t dalitz_root_tree --send -u ejfat://... --mtu 9000 file.root\n"
                      << "  Receive:           " << argv[0] << " --recv -u ejfat://... --recv-ip 127.0.0.1 -o output_{:06d}.dat\n";
            std::exit(0);
        }

        po::notify(vm);

        if (args.send_data && args.recv_data)
            throw std::runtime_error("Cannot use --send and --recv simultaneously");

        if (!args.dir_path.empty() && !args.file_paths.empty())
            throw std::runtime_error("--dir and positional files are mutually exclusive");

        if (args.parallel_streams == 0)
            throw std::runtime_error("--parallel must be greater than 0");

        if (!args.dir_path.empty()) {
            namespace fs = std::filesystem;
            for (const auto& entry : fs::directory_iterator(args.dir_path))
                if (entry.is_regular_file() && entry.path().extension() == ".root")
                    args.file_paths.push_back(entry.path().string());
            std::sort(args.file_paths.begin(), args.file_paths.end());
            if (args.file_paths.empty())
                throw std::runtime_error("No .root files found in: " + args.dir_path);
            std::cout << "Found " << args.file_paths.size()
                      << " .root file(s) in " << args.dir_path << std::endl;
        }

        if (!args.recv_data) {
            if (!args.use_toy && !args.use_gluex)
                throw std::runtime_error("One of --toy or --gluex must be specified");
            if (args.use_toy && args.use_gluex)
                throw std::runtime_error("--toy and --gluex are mutually exclusive");
        }

        if (args.send_data) {
            if (args.ejfat_uri.empty())
                throw std::runtime_error("--uri is required when --send is enabled");
            if (args.tree_name.empty())
                throw std::runtime_error("--tree is required when --send is enabled");
            if (args.file_paths.empty())
                throw std::runtime_error("ROOT file(s) required when --send is enabled");
            if (args.bufsize_mb == 0)
                throw std::runtime_error("--bufsize-mb must be greater than 0");
            if (args.mtu < 576 || args.mtu > 9000)
                throw std::runtime_error("--mtu must be between 576 and 9000 bytes");
        }

        if (args.recv_data) {
            if (args.ejfat_uri.empty())
                throw std::runtime_error("--uri is required when --recv is enabled");
            if (args.event_timeout_ms <= 0)
                throw std::runtime_error("--event-timeout must be greater than 0");
        }

        if (!args.send_data && !args.recv_data) {
            if (args.tree_name.empty())
                throw std::runtime_error("--tree is required for read-only mode");
            if (args.file_paths.empty())
                throw std::runtime_error("ROOT file(s) required for read-only mode");
        }

    } catch (const po::error& e) {
        std::cerr << "Error: " << e.what() << "\n\n" << desc << std::endl;
        throw;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n\n" << desc << std::endl;
        throw;
    }

    args.withCP   = vm["withcp"].as<bool>();
    args.validate = !vm["novalidate"].as<bool>();

    return args;
}

int main(int argc, char* argv[]) {
    ROOT::EnableThreadSafety();

    try {
        auto args = parseArgs(argc, argv);

        if (args.recv_data) {
            signal(SIGINT, signalHandler);

            auto reassembler = initializeReassembler(
                args.ejfat_uri, args.recv_ip, args.recv_port,
                args.recv_threads, args.event_timeout_ms,
                args.withCP, args.validate);

            if (!reassembler) {
                std::cerr << "Failed to initialize E2SAR reassembler" << std::endl;
                return 1;
            }

            bool success = receiveEvents(*reassembler, args.output_pattern, args.data_id);

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

        global_buffer_id = 0;

        if (args.send_data) {
            const size_t total_files = args.file_paths.size();
            const size_t N = std::min((size_t)args.parallel_streams, total_files);
            std::cout << "\nProcessing " << total_files << " file(s), "
                      << N << " at a time..." << std::endl;

            // Create the single shared Segmenter up front.
            // Child processes only read ROOT data and write to pipes — they never
            // call into gRPC/E2SAR, so forking after Segmenter creation is safe.
            auto segmenter = initializeSegmenter(args.ejfat_uri, args.data_id,
                                                 args.event_src_id, args.mtu,
                                                 args.withCP, args.rateGbps,
                                                 args.num_send_sockets,
                                                 args.validate,
                                                 args.send_ip);
            if (!segmenter) {
                std::cerr << "Failed to initialize E2SAR segmenter" << std::endl;
                return 1;
            }
            std::cout << "Segmenter ready. Max payload: "
                      << segmenter->getMaxPldLen() << " bytes" << std::endl;

            auto send_start_ = boost::chrono::high_resolution_clock::now();

            for (size_t batch_start = 0; batch_start < total_files; ) {
                const size_t batch_end = std::min(batch_start + N, total_files);
                std::cout << "\nBatch [" << batch_start + 1 << ".." << batch_end
                          << "] of " << total_files << " file(s)..." << std::endl;

                // Local vectors: go out of scope at end of batch → no FD/PID accumulation.
                std::vector<int>   pipe_read_fds;
                std::vector<pid_t> child_pids;

                for (size_t i = batch_start; i < batch_end; ++i) {
                    int fds[2];
                    if (pipe(fds) < 0) {
                        std::cerr << "pipe() failed: " << strerror(errno) << std::endl;
                        return 1;
                    }

                    pid_t pid = fork();
                    if (pid < 0) {
                        std::cerr << "fork() failed: " << strerror(errno) << std::endl;
                        return 1;
                    }

                    if (pid == 0) {
                        // child: close all read ends accumulated so far, then own read end
                        for (int rfd : pipe_read_fds) close(rfd);
                        close(fds[0]);

                        std::unique_ptr<RootFileProcessor> proc;
                        if (args.use_toy)
                            proc = std::make_unique<ToyFileProcessor>(args, nullptr, i, fds[1]);
                        else
                            proc = std::make_unique<GluexFileProcessor>(args, nullptr, i, fds[1]);

                        bool ok = proc->process(args.file_paths[i], args.tree_name);
                        close(fds[1]);
                        _exit(ok ? 0 : 1);
                    }

                    // parent: discard write end immediately; keep read end
                    close(fds[1]);
                    pipe_read_fds.push_back(fds[0]);
                    child_pids.push_back(pid);
                    std::cout << "  Process " << i << " (pid " << pid << "): "
                              << args.file_paths[i] << std::endl;
                }
                // Exactly (batch_end - batch_start) read FDs open in parent now.

                // One reader thread per pipe; each thread closes its fd before returning.
                std::vector<std::future<bool>> reader_futures;
                for (size_t k = 0; k < pipe_read_fds.size(); ++k) {
                    reader_futures.push_back(std::async(std::launch::async,
                        parentReaderLoop, pipe_read_fds[k], segmenter.get(), batch_start + k));
                }

                std::cout << "Waiting for reader threads..." << std::endl;
                for (size_t k = 0; k < reader_futures.size(); ++k) {
                    if (reader_futures[k].get()) success_count++;
                    else {
                        failure_count++;
                        std::cerr << "Reader thread " << (batch_start + k) << " failed" << std::endl;
                    }
                }

                // Reap this batch's children.
                for (size_t k = 0; k < child_pids.size(); ++k) {
                    int status = 0;
                    waitpid(child_pids[k], &status, 0);
                    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                        std::cerr << "Child process " << (batch_start + k)
                                  << " (pid " << child_pids[k] << ") reported failure" << std::endl;
                }

                batch_start = batch_end;
            }

            segmenter->stopThreads();

            auto send_end_ = boost::chrono::high_resolution_clock::now();
            auto elapsed_usec_ = boost::chrono::duration_cast<boost::chrono::microseconds>(
                                     send_end_ - send_start_);

            auto send_stats = segmenter->getSendStats();
            std::cout << "\n========== E2SAR Final Statistics ==========" << std::endl;
            std::cout << "Total UDP packets sent: " << send_stats.msgCnt << std::endl;
            std::cout << "Send errors: "               << send_stats.errCnt << std::endl;
            std::cout << "Total batches submitted: "   << global_buffer_id.load() << std::endl;
            std::cout << "Estimated effective throughput (Gbps): "
                      << (static_cast<double>(send_stats.msgCnt) * segmenter->getMaxPldLen() * 8.0)
                         / (elapsed_usec_.count() * 1000.0) << std::endl;
            if (send_stats.errCnt > 0)
                std::cerr << "WARNING: Errors occurred during sending" << std::endl;

        } else {
            // Read-only: process files sequentially (no network, no parallelism needed)
            std::cout << "\nProcessing " << args.file_paths.size()
                      << " file(s) (read-only)..." << std::endl;
            for (size_t i = 0; i < args.file_paths.size(); ++i) {
                std::cout << "  File " << i << ": " << args.file_paths[i] << std::endl;
                std::unique_ptr<RootFileProcessor> proc;
                if (args.use_toy)
                    proc = std::make_unique<ToyFileProcessor>(args, nullptr, i);
                else
                    proc = std::make_unique<GluexFileProcessor>(args, nullptr, i);
                bool result = proc->process(args.file_paths[i], args.tree_name);
                if (result) success_count++;
                else {
                    failure_count++;
                    std::cerr << "File " << i << " failed" << std::endl;
                }
            }
        }

        std::cout << "\nProcessing complete: "
                  << success_count << " file(s) processed successfully";
        if (failure_count > 0)
            std::cout << ", " << failure_count << " file(s) failed";
        std::cout << std::endl;

        return failure_count > 0 ? 1 : 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
