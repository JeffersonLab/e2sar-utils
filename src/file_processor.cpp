#include "file_processor.hpp"
#include <iostream>
#include <sstream>
#include <chrono>
#include <thread>

// ── Globals ──────────────────────────────────────────────────────────────────

std::atomic<size_t> global_buffer_id{0};
std::mutex          cout_mutex;

// ── File-local helpers ───────────────────────────────────────────────────────

namespace {

struct StreamingStats {
    size_t total_events_processed = 0;
    size_t total_batches_sent     = 0;
    size_t total_buffers_sent     = 0;
    size_t total_bytes_sent       = 0;

    void addBatch(size_t events, size_t buffers, size_t bytes) {
        total_events_processed += events;
        total_batches_sent++;
        total_buffers_sent += buffers;
        total_bytes_sent   += bytes;
    }

    void printProgress() const {
        std::cout << "  Batches: " << total_batches_sent
                  << " | Events: " << total_events_processed
                  << " | Buffers: " << total_buffers_sent
                  << " | MB sent: " << (total_bytes_sent / (1024.0 * 1024.0))
                  << std::endl;
    }
};

void thread_print(size_t file_idx, const std::ostringstream& oss) {
    std::lock_guard<std::mutex> lock(cout_mutex);
    std::cout << "[File " << file_idx << "] " << oss.str() << std::endl;
}

void freeBuffer(boost::any a) {
    delete boost::any_cast<std::vector<double>*>(a);
}

} // namespace

// ── RootFileProcessor::process() ─────────────────────────────────────────────

bool RootFileProcessor::process(const std::string& file_path,
                                const std::string& tree_name) {
    auto file = std::unique_ptr<TFile>(TFile::Open(file_path.c_str(), "READ"));
    if (!file || file->IsZombie()) {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cerr << "[File " << file_index_ << "] Error: Cannot open file " << file_path << std::endl;
        return false;
    }

    TTree* tree = file->Get<TTree>(tree_name.c_str());
    if (!tree) {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cerr << "[File " << file_index_ << "] Error: Tree '" << tree_name
                  << "' not found in file " << file_path << std::endl;
        return false;
    }

    Long64_t nEntries = tree->GetEntries();
    {
        std::ostringstream oss;
        oss << "Found tree '" << tree_name << "' with " << nEntries << " entries";
        thread_print(file_index_, oss);
    }

    bindBranches(tree);

    const size_t EVENT_SIZE        = eventSize();
    const size_t BATCH_SIZE_BYTES  = args_.bufsize_mb * 1024 * 1024;
    const size_t BATCH_SIZE_EVENTS = BATCH_SIZE_BYTES / EVENT_SIZE;
    const size_t BATCH_DOUBLES     = BATCH_SIZE_BYTES / sizeof(double);

    {
        std::ostringstream oss;
        oss << "Batch size: " << args_.bufsize_mb << " MB (" << BATCH_SIZE_EVENTS << " events)";
        thread_print(file_index_, oss);
    }

    StreamingStats stats;

    {
        std::ostringstream oss;
        oss << "Streaming " << nEntries << " events...";
        thread_print(file_index_, oss);
    }

    auto* batch = new std::vector<double>();
    batch->reserve(BATCH_DOUBLES);
    size_t events_in_batch = 0;

    for (Long64_t i = 0; i < nEntries; ++i) {
        tree->GetEntry(i);
        appendEntry(*batch);
        events_in_batch++;

        if (events_in_batch >= BATCH_SIZE_EVENTS || i == nEntries - 1) {
            if (args_.send_data && segmenter_) {
                uint8_t* buffer_ptr    = reinterpret_cast<uint8_t*>(batch->data());
                size_t   buffer_size   = batch->size() * sizeof(double);
                size_t   cur_buffer_id = global_buffer_id.fetch_add(1);

                bool sent        = false;
                int  retry_count = 0;
                const int MAX_RETRIES = 10000;

                while (!sent && retry_count < MAX_RETRIES) {
                    auto send_result = segmenter_->addToSendQueue(buffer_ptr, buffer_size,
                        cur_buffer_id, 0, 0, &freeBuffer, batch);

                    if (send_result.has_error()) {
                        if (send_result.error().code() == e2sar::E2SARErrorc::MemoryError) {
                            std::this_thread::sleep_for(std::chrono::microseconds(100));
                            retry_count++;
                            continue;
                        } else {
                            std::lock_guard<std::mutex> lock(cout_mutex);
                            std::cerr << "[File " << file_index_ << "] Send error: "
                                      << send_result.error().message() << std::endl;
                            delete batch;
                            return false;
                        }
                    }
                    sent = true;
                }

                if (!sent) {
                    std::lock_guard<std::mutex> lock(cout_mutex);
                    std::cerr << "[File " << file_index_ << "] Failed to send buffer after "
                              << MAX_RETRIES << " retries" << std::endl;
                    delete batch;
                    return false;
                }

                stats.addBatch(events_in_batch, 1, buffer_size);

                if (stats.total_batches_sent % 10 == 0) {
                    std::lock_guard<std::mutex> lock(cout_mutex);
                    std::cout << "[File " << file_index_ << "] ";
                    stats.printProgress();
                }
            } else if (!args_.send_data) {
                delete batch;
            }

            if (i < nEntries - 1) {
                batch = new std::vector<double>();
                batch->reserve(BATCH_DOUBLES);
                events_in_batch = 0;
            }
        }

        if ((i + 1) % 500000 == 0) {
            std::ostringstream oss;
            oss << "Read " << (i + 1) << " / " << nEntries << " events";
            thread_print(file_index_, oss);
        }
    }

    {
        std::ostringstream oss;
        oss << "Successfully processed " << nEntries << " events from " << file_path;
        thread_print(file_index_, oss);
    }

    if (nEntries > 0) {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "[File " << file_index_ << "] Sample (first event):" << std::endl;
        printSample();
    }

    if (args_.send_data && segmenter_) {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "[File " << file_index_ << "] ========== File Processing Complete ==========" << std::endl;
        std::cout << "[File " << file_index_ << "] Events processed: " << stats.total_events_processed << std::endl;
        std::cout << "[File " << file_index_ << "] Batches sent: "     << stats.total_batches_sent      << std::endl;
        std::cout << "[File " << file_index_ << "] Data volume: "      << (stats.total_bytes_sent / (1024.0 * 1024.0)) << " MB" << std::endl;
    }

    return true;
}

// ── ToyFileProcessor ─────────────────────────────────────────────────────────

void ToyFileProcessor::bindBranches(TTree* tree) {
    tree->SetBranchAddress("mag_plus_rec",       &mag_plus_rec_);
    tree->SetBranchAddress("theta_plus_rec",     &theta_plus_rec_);
    tree->SetBranchAddress("phi_plus_rec",       &phi_plus_rec_);
    tree->SetBranchAddress("mag_neg_rec",        &mag_neg_rec_);
    tree->SetBranchAddress("theta_neg_rec",      &theta_neg_rec_);
    tree->SetBranchAddress("phi_neg_rec",        &phi_neg_rec_);
    tree->SetBranchAddress("mag_neutral1_rec",   &mag_neutral1_rec_);
    tree->SetBranchAddress("theta_neutral1_rec", &theta_neutral1_rec_);
    tree->SetBranchAddress("phi_neutral1_rec",   &phi_neutral1_rec_);
    tree->SetBranchAddress("mag_neutral2_rec",   &mag_neutral2_rec_);
    tree->SetBranchAddress("theta_neutral2_rec", &theta_neutral2_rec_);
    tree->SetBranchAddress("phi_neutral2_rec",   &phi_neutral2_rec_);
}

void ToyFileProcessor::appendEntry(std::vector<double>& batch) {
    DalitzEventData event;
    event.pi_plus  = createLorentzVector(mag_plus_rec_,     theta_plus_rec_,     phi_plus_rec_,     PION_MASS_);
    event.pi_minus = createLorentzVector(mag_neg_rec_,      theta_neg_rec_,      phi_neg_rec_,      PION_MASS_);
    event.gamma1   = createLorentzVector(mag_neutral1_rec_, theta_neutral1_rec_, phi_neutral1_rec_, PHOTON_MASS_);
    event.gamma2   = createLorentzVector(mag_neutral2_rec_, theta_neutral2_rec_, phi_neutral2_rec_, PHOTON_MASS_);
    if (!saved_first_) { first_ = event; saved_first_ = true; }
    event.appendToBuffer(batch);
}

void ToyFileProcessor::printSample() const {
    std::cout << "[File " << file_index_ << "]   π+ : E=" << first_.pi_plus.E()
              << " GeV, p=(" << first_.pi_plus.Px()  << ", " << first_.pi_plus.Py()  << ", " << first_.pi_plus.Pz()  << ") GeV/c" << std::endl;
    std::cout << "[File " << file_index_ << "]   π- : E=" << first_.pi_minus.E()
              << " GeV, p=(" << first_.pi_minus.Px() << ", " << first_.pi_minus.Py() << ", " << first_.pi_minus.Pz() << ") GeV/c" << std::endl;
    std::cout << "[File " << file_index_ << "]   γ1 : E=" << first_.gamma1.E()
              << " GeV, p=(" << first_.gamma1.Px()   << ", " << first_.gamma1.Py()   << ", " << first_.gamma1.Pz()   << ") GeV/c" << std::endl;
    std::cout << "[File " << file_index_ << "]   γ2 : E=" << first_.gamma2.E()
              << " GeV, p=(" << first_.gamma2.Px()   << ", " << first_.gamma2.Py()   << ", " << first_.gamma2.Pz()   << ") GeV/c" << std::endl;
}

// ── GluexFileProcessor ───────────────────────────────────────────────────────

void GluexFileProcessor::bindBranches(TTree* tree) {
    tree->SetBranchAddress("pip_p4_kin",   &pip_rec_);
    tree->SetBranchAddress("pim_p4_kin",   &pim_rec_);
    tree->SetBranchAddress("g1_p4_kin",    &g1_rec_);
    tree->SetBranchAddress("g2_p4_kin",    &g2_rec_);
    tree->SetBranchAddress("imass_kfit",   &imass_kfit_);
    tree->SetBranchAddress("imassGG_kfit", &imassGG_kfit_);
    tree->SetBranchAddress("kfit_prob",    &kfit_prob_);
}

void GluexFileProcessor::appendEntry(std::vector<double>& batch) {
    GluexEventData event;
    event.pip          = *pip_rec_;
    event.pim          = *pim_rec_;
    event.g1           = *g1_rec_;
    event.g2           = *g2_rec_;
    event.imass_kfit   = imass_kfit_;
    event.imassGG_kfit = imassGG_kfit_;
    event.kfit_prob    = kfit_prob_;
    if (!saved_first_) { first_ = event; saved_first_ = true; }
    event.appendToBuffer(batch);
}

void GluexFileProcessor::printSample() const {
    std::cout << "[File " << file_index_ << "]   π+ : E=" << first_.pip.E()
              << " GeV, p=(" << first_.pip.Px() << ", " << first_.pip.Py() << ", " << first_.pip.Pz() << ") GeV/c" << std::endl;
    std::cout << "[File " << file_index_ << "]   π- : E=" << first_.pim.E()
              << " GeV, p=(" << first_.pim.Px() << ", " << first_.pim.Py() << ", " << first_.pim.Pz() << ") GeV/c" << std::endl;
    std::cout << "[File " << file_index_ << "]   γ1 : E=" << first_.g1.E()
              << " GeV, p=(" << first_.g1.Px()  << ", " << first_.g1.Py()  << ", " << first_.g1.Pz()  << ") GeV/c" << std::endl;
    std::cout << "[File " << file_index_ << "]   γ2 : E=" << first_.g2.E()
              << " GeV, p=(" << first_.g2.Px()  << ", " << first_.g2.Py()  << ", " << first_.g2.Pz()  << ") GeV/c" << std::endl;
    std::cout << "[File " << file_index_ << "]   imass_kfit=" << first_.imass_kfit
              << "  imassGG_kfit=" << first_.imassGG_kfit
              << "  kfit_prob="    << first_.kfit_prob << std::endl;
}
