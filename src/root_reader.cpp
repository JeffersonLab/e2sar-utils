#include <TFile.h>
#include <TTree.h>
#include <TLorentzVector.h>
#include <TVector3.h>
#include <boost/program_options.hpp>
#include <e2sar.hpp>
#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <cstdlib>
#include <chrono>
#include <thread>

namespace po = boost::program_options;

struct CommandLineArgs {
    std::string tree_name;
    std::vector<std::string> file_paths;
    // E2SAR sending options
    bool send_data = false;
    std::string ejfat_uri;
    uint16_t data_id = 4321;
    uint32_t event_src_id = 1234;
    size_t bufsize_mb = 10;      // NEW: Batch size in MB
    uint16_t mtu = 1500;         // NEW: MTU for Segmenter
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

// Initialize and start E2SAR Segmenter
// Returns unique_ptr for RAII-based cleanup
std::unique_ptr<e2sar::Segmenter> initializeSegmenter(
    const std::string& uri_str,
    uint16_t data_id,
    uint32_t event_src_id,
    uint16_t mtu) {

    std::cout << "\nInitializing E2SAR Segmenter..." << std::endl;

    // Parse EJFAT URI
    auto uri_result = e2sar::EjfatURI::getFromString(uri_str,
        e2sar::EjfatURI::TokenType::admin, false);

    if (uri_result.has_error()) {
        std::cerr << "Error parsing URI: " << uri_result.error().message() << std::endl;
        return nullptr;
    }

    e2sar::EjfatURI uri = uri_result.value();

    // Create Segmenter with configurable MTU
    e2sar::Segmenter::SegmenterFlags sflags;
    sflags.mtu = mtu;
    sflags.useCP = false;
    sflags.numSendSockets = 4;

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

    return segmenter;
}

CommandLineArgs parseArgs(int argc, char* argv[]) {
    CommandLineArgs args;

    po::options_description desc("ROOT File Reader - Extract named trees from ROOT files");
    desc.add_options()
        ("help,h", "Show this help message")
        ("tree,t", po::value<std::string>(&args.tree_name)->required(),
         "Name of the tree to extract (required)")
        ("send,s", po::bool_switch(&args.send_data)->default_value(false),
         "Enable E2SAR network sending")
        ("uri,u", po::value<std::string>(&args.ejfat_uri),
         "EJFAT URI for E2SAR sending (required if --send)")
        ("dataid", po::value<uint16_t>(&args.data_id)->default_value(1),
         "Data ID for E2SAR (default: 1)")
        ("eventsrcid", po::value<uint32_t>(&args.event_src_id)->default_value(1),
         "Event source ID for E2SAR (default: 1)")
        ("bufsize-mb", po::value<size_t>(&args.bufsize_mb)->default_value(10),
         "Batch size in MB for streaming (default: 10)")
        ("mtu", po::value<uint16_t>(&args.mtu)->default_value(1500),
         "MTU size in bytes for E2SAR segmenter (default: 1500)")
        ("files", po::value<std::vector<std::string>>(&args.file_paths)->required(),
         "ROOT files to process");

    po::positional_options_description pos;
    pos.add("files", -1);

    po::variables_map vm;

    try {
        po::store(po::command_line_parser(argc, argv)
                  .options(desc)
                  .positional(pos)
                  .run(), vm);

        if (vm.count("help")) {
            std::cout << "Usage: " << argv[0]
                      << " --tree <tree_name> [--send --uri <ejfat_uri>] <file1.root> ...\n\n"
                      << desc << "\n"
                      << "Examples:\n"
                      << "  " << argv[0] << " --tree dalitz_root_tree data/file.root\n"
                      << "  " << argv[0] << " -t my_tree --send -u ejfat://... --bufsize-mb 5 file.root\n"
                      << "  " << argv[0] << " -t my_tree --send -u ejfat://... --bufsize-mb 20 --mtu 9000 file.root\n";
            std::exit(0);
        }

        po::notify(vm);  // Throws if required options missing

        // Validate E2SAR options
        if (args.send_data && args.ejfat_uri.empty()) {
            throw std::runtime_error("--uri is required when --send is enabled");
        }

        // Validate bufsize_mb
        if (args.bufsize_mb == 0) {
            throw std::runtime_error("--bufsize-mb must be greater than 0");
        }

        // Validate MTU range
        if (args.mtu < 576 || args.mtu > 9000) {
            throw std::runtime_error("--mtu must be between 576 and 9000 bytes");
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

    return args;
}

bool processRootFile(const std::string& file_path, const std::string& tree_name, const CommandLineArgs& args) {
    // Open ROOT file
    auto file = std::unique_ptr<TFile>(TFile::Open(file_path.c_str(), "READ"));

    // Check if file opened successfully
    if (!file || file->IsZombie()) {
        std::cerr << "Error: Cannot open file " << file_path << std::endl;
        return false;
    }

    // Get the tree
    TTree* tree = file->Get<TTree>(tree_name.c_str());

    // Check if tree exists
    if (!tree) {
        std::cerr << "Error: Tree '" << tree_name
                  << "' not found in file " << file_path << std::endl;
        return false;
    }

    // Get number of entries
    Long64_t nEntries = tree->GetEntries();
    std::cout << "Found tree '" << tree_name << "' with " << nEntries << " entries" << std::endl;

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

    std::cout << "Streaming configuration:" << std::endl;
    std::cout << "  Batch size: " << args.bufsize_mb << " MB ("
              << BATCH_SIZE_EVENTS << " events)" << std::endl;

    // Initialize E2SAR if sending
    std::unique_ptr<e2sar::Segmenter> segmenter;
    size_t buffer_id = 0;          // Running buffer ID counter
    StreamingStats stats;

    if (args.send_data) {
        segmenter = initializeSegmenter(args.ejfat_uri, args.data_id,
                                       args.event_src_id, args.mtu);
        if (!segmenter) {
            std::cerr << "Failed to initialize E2SAR segmenter" << std::endl;
            return false;
        }

        std::cout << "  Max payload: " << segmenter->getMaxPldLen() << " bytes" << std::endl;
    }

    // ========== Streaming read-send loop ==========

    std::cout << "\nStreaming " << nEntries << " events..." << std::endl;

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

            // Send batch if E2SAR enabled
            if (args.send_data) {
                uint8_t* buffer_ptr = reinterpret_cast<uint8_t*>(batch->data());
                size_t buffer_size = batch->size() * sizeof(double);

                // Send buffer via E2SAR with retry on queue full
                bool sent = false;
                int retry_count = 0;
                const int MAX_RETRIES = 10000;

                while (!sent && retry_count < MAX_RETRIES) {
                    auto send_result = segmenter->addToSendQueue(buffer_ptr, buffer_size,
                        buffer_id, 0, 0, &freeBuffer, batch);

                    if (send_result.has_error()) {
                        if (send_result.error().code() == e2sar::E2SARErrorc::MemoryError) {
                            // Queue full, retry after short sleep
                            std::this_thread::sleep_for(std::chrono::microseconds(100));
                            retry_count++;
                            continue;
                        } else {
                            std::cerr << "Send error: " << send_result.error().message() << std::endl;
                            delete batch;
                            return false;
                        }
                    }
                    sent = true;
                }

                if (!sent) {
                    std::cerr << "Failed to send buffer after " << MAX_RETRIES << " retries" << std::endl;
                    delete batch;
                    return false;
                }

                buffer_id++;
                stats.addBatch(events_in_batch, 1, buffer_size);

                // Progress update every 10 batches
                if (stats.total_batches_sent % 10 == 0) {
                    stats.printProgress();
                }
            } else {
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
            std::cout << "  Read " << (i + 1) << " / " << nEntries << " events" << std::endl;
        }
    }

    std::cout << "\nSuccessfully processed " << nEntries << " events from " << file_path << std::endl;

    // ========== Sample output (use saved first_event) ==========

    if (saved_first) {
        std::cout << "\nSample (first event):" << std::endl;
        std::cout << "  π+ : E=" << first_event.pi_plus.E()
                  << " GeV, p=(" << first_event.pi_plus.Px()
                  << ", " << first_event.pi_plus.Py()
                  << ", " << first_event.pi_plus.Pz() << ") GeV/c" << std::endl;
        std::cout << "  π- : E=" << first_event.pi_minus.E()
                  << " GeV, p=(" << first_event.pi_minus.Px()
                  << ", " << first_event.pi_minus.Py()
                  << ", " << first_event.pi_minus.Pz() << ") GeV/c" << std::endl;
        std::cout << "  γ1 : E=" << first_event.gamma1.E()
                  << " GeV, p=(" << first_event.gamma1.Px()
                  << ", " << first_event.gamma1.Py()
                  << ", " << first_event.gamma1.Pz() << ") GeV/c" << std::endl;
        std::cout << "  γ2 : E=" << first_event.gamma2.E()
                  << " GeV, p=(" << first_event.gamma2.Px()
                  << ", " << first_event.gamma2.Py()
                  << ", " << first_event.gamma2.Pz() << ") GeV/c" << std::endl;
    }

    // ========== Wait for completion and final stats ==========

    if (args.send_data && segmenter) {
        std::cout << "\nAll batches queued. Waiting for send completion..." << std::endl;

        // Calculate expected frame count based on total bytes sent
        size_t max_payload = segmenter->getMaxPldLen();
        size_t expected_frames = (stats.total_bytes_sent + max_payload - 1) / max_payload;

        // Wait for completion
        auto start = std::chrono::steady_clock::now();
        while (true) {
            auto send_stats = segmenter->getSendStats();

            if (send_stats.msgCnt >= expected_frames || send_stats.errCnt > 0) {
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        auto send_stats = segmenter->getSendStats();
        auto duration = std::chrono::steady_clock::now() - start;
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();

        std::cout << "\n========== E2SAR Sending Complete ==========" << std::endl;
        std::cout << "Events processed: " << stats.total_events_processed << std::endl;
        std::cout << "Batches sent: " << stats.total_batches_sent << std::endl;
        std::cout << "E2SAR buffers: " << stats.total_buffers_sent << std::endl;
        std::cout << "Data volume: " << (stats.total_bytes_sent / (1024.0 * 1024.0)) << " MB" << std::endl;
        std::cout << "Network frames: " << send_stats.msgCnt << " / " << expected_frames << std::endl;
        std::cout << "Errors: " << send_stats.errCnt << std::endl;
        std::cout << "Duration: " << duration_ms << " ms" << std::endl;

        if (send_stats.errCnt > 0) {
            std::cerr << "WARNING: Errors occurred during sending" << std::endl;
            return false;
        }
    }

    // Segmenter destructor called automatically (stops threads)

    // File closes automatically when unique_ptr goes out of scope
    return true;
}

int main(int argc, char* argv[]) {
    try {
        // Parse arguments
        auto args = parseArgs(argc, argv);

        // Process each file
        int success_count = 0;
        int failure_count = 0;

        for (const auto& file_path : args.file_paths) {
            std::cout << "Opening ROOT file: " << file_path << std::endl;

            if (processRootFile(file_path, args.tree_name, args)) {
                success_count++;
            } else {
                failure_count++;
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
