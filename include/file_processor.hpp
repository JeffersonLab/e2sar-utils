#pragma once
#include "event_data.hpp"
#include <TFile.h>
#include <TTree.h>
#include <e2sar.hpp>
#include <boost/any.hpp>
#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <cstdint>

struct CommandLineArgs {
    std::string tree_name;
    std::vector<std::string> file_paths;
    // E2SAR sending options
    bool send_data = false;
    std::string ejfat_uri;
    uint16_t data_id = 4321;
    uint32_t event_src_id = 1234;
    size_t bufsize_mb = 10;
    uint16_t mtu = 1500;
    // E2SAR receiving options
    bool recv_data = false;
    std::string recv_ip;
    uint16_t recv_port = 19522;
    size_t recv_threads = 1;
    std::string output_pattern = "event_{:08d}.dat";
    int event_timeout_ms = 500;
    bool withCP;
    float rateGbps;
    bool validate;
    // Event schema selection (exactly one required for sender/read-only mode)
    bool use_toy   = false;
    bool use_gluex = false;
};

// Defined in file_processor.cpp; also used by e2sar_root.cpp (receiveEvents, main).
extern std::atomic<size_t> global_buffer_id;
extern std::mutex          cout_mutex;

// Abstract base for per-file ROOT processing (Template Method pattern).
// process() owns the file open, batch allocation, send loop, and statistics.
// Subclasses supply four hooks for their specific tree schema.
class RootFileProcessor {
public:
    RootFileProcessor(const CommandLineArgs& args,
                      e2sar::Segmenter* segmenter,
                      size_t file_index)
        : args_(args), segmenter_(segmenter), file_index_(file_index) {}

    virtual ~RootFileProcessor() = default;

    // Template method: not overridden by subclasses.
    bool process(const std::string& file_path, const std::string& tree_name);

protected:
    // Bind type-specific branch addresses on the already-opened tree.
    virtual void bindBranches(TTree* tree) = 0;
    // Called after tree->GetEntry(i): build one event from loaded branch vars
    // and append it to batch. Save state for printSample() on the first call.
    virtual void appendEntry(std::vector<double>& batch) = 0;
    // Print the first-event summary to stdout (cout_mutex already held by caller).
    virtual void printSample() const = 0;
    // Serialized byte size of one event; used to compute batch capacity.
    virtual size_t eventSize() const = 0;

    const CommandLineArgs& args_;
    e2sar::Segmenter*      segmenter_;
    size_t                 file_index_;
};

// Processes Dalitz toy-MC ROOT files (dalitz_root_tree schema).
// Branches: mag/theta/phi_{plus,neg,neutral1,neutral2}_rec → DalitzEventData
class ToyFileProcessor : public RootFileProcessor {
public:
    using RootFileProcessor::RootFileProcessor;

protected:
    void bindBranches(TTree* tree) override;
    void appendEntry(std::vector<double>& batch) override;
    void printSample() const override;
    size_t eventSize() const override { return DalitzEventData{}.size(); }

private:
    static constexpr Double_t PION_MASS_   = 0.139;
    static constexpr Double_t PHOTON_MASS_ = 0.0;

    Double_t mag_plus_rec_      = 0, theta_plus_rec_      = 0, phi_plus_rec_      = 0;
    Double_t mag_neg_rec_       = 0, theta_neg_rec_       = 0, phi_neg_rec_       = 0;
    Double_t mag_neutral1_rec_  = 0, theta_neutral1_rec_  = 0, phi_neutral1_rec_  = 0;
    Double_t mag_neutral2_rec_  = 0, theta_neutral2_rec_  = 0, phi_neutral2_rec_  = 0;

    DalitzEventData first_;
    bool            saved_first_ = false;
};

// Processes GlueX kinematic-fit ROOT files (myTree schema).
// Branches: pip_p4_kin, pim_p4_kin, g1_p4_kin, g2_p4_kin (TLorentzVector*),
//           imass_kfit, imassGG_kfit, kfit_prob (Double_t) → GluexEventData
class GluexFileProcessor : public RootFileProcessor {
public:
    using RootFileProcessor::RootFileProcessor;

protected:
    void bindBranches(TTree* tree) override;
    void appendEntry(std::vector<double>& batch) override;
    void printSample() const override;
    size_t eventSize() const override { return GluexEventData{}.size(); }

private:
    TLorentzVector *pip_rec_  = nullptr, *pim_rec_ = nullptr;
    TLorentzVector *g1_rec_   = nullptr, *g2_rec_  = nullptr;
    Double_t        imass_kfit_ = 0.0, imassGG_kfit_ = 0.0, kfit_prob_ = 0.0;

    GluexEventData first_;
    bool           saved_first_ = false;
};
