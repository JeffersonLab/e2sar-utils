#include <stdio.h>
#include <sys/stat.h>
#include "TFile.h"
#include "TTree.h"
#include "TLorentzVector.h"
#include "TH1F.h"
#include "TMath.h"
#include <vector>

const Double_t M_ETA  = 0.547862;
const Double_t M_PIPM = 0.139570;
const Double_t M_PI0  = 0.134977;

// 19 doubles per event: 16 for 4-vectors (4 particles x 4) + 3 kfit scalars
const Int_t EVENT_DOUBLES = 19;

struct EventData {
    TLorentzVector pip;
    TLorentzVector pim;
    TLorentzVector g1;
    TLorentzVector g2;
    Double_t imass_kfit;
    Double_t imassGG_kfit;
    Double_t kfit_prob;
};

struct AnalysisResult {
    Double_t X;
    Double_t Y;
    Bool_t   pass;
};

// Reads one entry from the already-bound tree into an EventData.
EventData read_out_tree(TTree *tree, Int_t i,
                        TLorentzVector *pip_rec, TLorentzVector *pim_rec,
                        TLorentzVector *g1_rec,  TLorentzVector *g2_rec,
                        Double_t &imass_kfit, Double_t &imassGG_kfit, Double_t &kfit_prob)
{
    tree->GetEntry(i);
    EventData ev;
    ev.pip          = *pip_rec;
    ev.pim          = *pim_rec;
    ev.g1           = *g1_rec;
    ev.g2           = *g2_rec;
    ev.imass_kfit   = imass_kfit;
    ev.imassGG_kfit = imassGG_kfit;
    ev.kfit_prob    = kfit_prob;
    return ev;
}

// Pure physics: computes Dalitz coordinates and applies event-selection cuts.
AnalysisResult analysis(const EventData &ev)
{
    TLorentzVector pi0 = ev.g1 + ev.g2;
    Double_t s_pimpi0  = (ev.pim + pi0).M2();
    Double_t s_pippi0  = (ev.pip + pi0).M2();
    Double_t s_pippim  = (ev.pip + ev.pim).M2();

    Double_t Q        = M_ETA - 2*M_PIPM - M_PI0;
    Double_t s_centre = (M_ETA*M_ETA + 2*M_PIPM*M_PIPM + M_PI0*M_PI0) / 3.0;
    Double_t denom    = Q * (Q + 3*M_PI0);

    AnalysisResult res;
    res.X    = TMath::Sqrt(3) * (s_pimpi0 - s_pippi0) / denom;
    res.Y    = 3 * (s_pippim - s_centre) / denom;
    res.pass = (ev.kfit_prob    >  0.0001 &&
                ev.imass_kfit   >= 0.45   && ev.imass_kfit   < 0.58 &&
                ev.imassGG_kfit >  0.1    && ev.imassGG_kfit < 0.15);
    return res;
}

// Appends one EventData as EVENT_DOUBLES doubles to the flat buffer.
// Layout: [pip E px py pz][pim E px py pz][g1 E px py pz][g2 E px py pz]
//         [imass_kfit][imassGG_kfit][kfit_prob]
void append_to_buffer(const EventData &ev, std::vector<Double_t> &buf)
{
    buf.push_back(ev.pip.E());  buf.push_back(ev.pip.Px());
    buf.push_back(ev.pip.Py()); buf.push_back(ev.pip.Pz());

    buf.push_back(ev.pim.E());  buf.push_back(ev.pim.Px());
    buf.push_back(ev.pim.Py()); buf.push_back(ev.pim.Pz());

    buf.push_back(ev.g1.E());   buf.push_back(ev.g1.Px());
    buf.push_back(ev.g1.Py());  buf.push_back(ev.g1.Pz());

    buf.push_back(ev.g2.E());   buf.push_back(ev.g2.Px());
    buf.push_back(ev.g2.Py());  buf.push_back(ev.g2.Pz());

    buf.push_back(ev.imass_kfit);
    buf.push_back(ev.imassGG_kfit);
    buf.push_back(ev.kfit_prob);
}

// Reconstructs one EventData from offset i*EVENT_DOUBLES in the flat buffer.
EventData deserialize_event(const std::vector<Double_t> &buf, Int_t i)
{
    const Double_t *p = buf.data() + i * EVENT_DOUBLES;
    EventData ev;
    ev.pip.SetPxPyPzE(p[1], p[2], p[3], p[0]);
    ev.pim.SetPxPyPzE(p[5], p[6], p[7], p[4]);
    ev.g1.SetPxPyPzE( p[9], p[10],p[11],p[8]);
    ev.g2.SetPxPyPzE( p[13],p[14],p[15],p[12]);
    ev.imass_kfit   = p[16];
    ev.imassGG_kfit = p[17];
    ev.kfit_prob    = p[18];
    return ev;
}

// Phase 1: read all tree entries and serialize into a flat buffer.
std::vector<Double_t> build_buffer(TTree *tree,
                                   TLorentzVector *pip_rec, TLorentzVector *pim_rec,
                                   TLorentzVector *g1_rec,  TLorentzVector *g2_rec,
                                   Double_t &imass_kfit, Double_t &imassGG_kfit,
                                   Double_t &kfit_prob)
{
    Long64_t n = tree->GetEntries();
    std::vector<Double_t> buf;
    buf.reserve(n * EVENT_DOUBLES);

    for (Long64_t i = 0; i < n; i++) {
        EventData ev = read_out_tree(tree, i,
                                     pip_rec, pim_rec, g1_rec, g2_rec,
                                     imass_kfit, imassGG_kfit, kfit_prob);
        append_to_buffer(ev, buf);
    }
    return buf;
}

// Phase 2: walk the flat buffer, run analysis on each event, fill histograms.
void analyze_buffer(const std::vector<Double_t> &buf,
                    TH1F *h_X_pre,    TH1F *h_Y_pre,
                    TH1F *h_im_pre,   TH1F *h_imgg_pre,
                    TH1F *h_X_post,   TH1F *h_Y_post,
                    TH1F *h_im_post,  TH1F *h_imgg_post)
{
    Int_t n = buf.size() / EVENT_DOUBLES;
    for (Int_t i = 0; i < n; i++) {
        EventData      ev  = deserialize_event(buf, i);
        AnalysisResult res = analysis(ev);

        h_X_pre->Fill(res.X);
        h_Y_pre->Fill(res.Y);
        h_im_pre->Fill(ev.imass_kfit);
        h_imgg_pre->Fill(ev.imassGG_kfit);

        if (res.pass) {
            h_X_post->Fill(res.X);
            h_Y_post->Fill(res.Y);
            h_im_post->Fill(ev.imass_kfit);
            h_imgg_post->Fill(ev.imassGG_kfit);
        }
    }
}

void factored_gluex_analysis(const char *filename)
{
    TFile *file = TFile::Open(filename);
    TTree *tree = (TTree*)file->Get("myTree");

    TLorentzVector *pip_rec = 0, *pim_rec = 0, *g1_rec = 0, *g2_rec = 0;
    Double_t imass_kfit = 0.0, imassGG_kfit = 0.0, kfit_prob = 0.0;

    tree->SetBranchAddress("pip_p4_kin",   &pip_rec);
    tree->SetBranchAddress("pim_p4_kin",   &pim_rec);
    tree->SetBranchAddress("g1_p4_kin",    &g1_rec);
    tree->SetBranchAddress("g2_p4_kin",    &g2_rec);
    tree->SetBranchAddress("imass_kfit",   &imass_kfit);
    tree->SetBranchAddress("imassGG_kfit", &imassGG_kfit);
    tree->SetBranchAddress("kfit_prob",    &kfit_prob);

    TCanvas *c1 = new TCanvas("c1", "Pre Event Selection",  1200, 400);
    c1->Divide(2, 2);
    TCanvas *c2 = new TCanvas("c2", "Post Event Selection", 1200, 400);
    c2->Divide(2, 2);

    TH1F *h_X_pre     = new TH1F("h_X_pre",    " ; X; Entries",      100, -1.5, 1.5);
    TH1F *h_Y_pre     = new TH1F("h_Y_pre",    " ; Y; Entries",      100,  0.0, 1.5);
    TH1F *h_im_pre    = new TH1F("h_im_pre",   " ; M [GeV]; Counts", 100,  0.2, 1.0);
    TH1F *h_imgg_pre  = new TH1F("h_imgg_pre", " ; M [GeV]; Counts", 100,  0.0, 0.3);
    TH1F *h_X_post    = new TH1F("h_X_post",   " ; X; Entries",      100, -1.5, 1.5);
    TH1F *h_Y_post    = new TH1F("h_Y_post",   " ; Y; Entries",      100,  0.0, 1.5);
    TH1F *h_im_post   = new TH1F("h_im_post",  " ; M [GeV]; Counts", 100,  0.2, 1.0);
    TH1F *h_imgg_post = new TH1F("h_imgg_post"," ; M [GeV]; Counts", 100,  0.0, 0.3);

    // Phase 1: serialize entire ROOT file into a flat buffer
    std::vector<Double_t> buf = build_buffer(tree,
                                             pip_rec, pim_rec, g1_rec, g2_rec,
                                             imass_kfit, imassGG_kfit, kfit_prob);

    // Phase 2: process the buffer and fill histograms
    analyze_buffer(buf,
                   h_X_pre,  h_Y_pre,  h_im_pre,  h_imgg_pre,
                   h_X_post, h_Y_post, h_im_post, h_imgg_post);

    const char *outdir = "histograms_event_selection";
    mkdir(outdir, 0755);

    c1->cd(1); h_X_pre->Draw();
    c1->cd(2); h_Y_pre->Draw();
    c1->cd(3); h_im_pre->Draw();
    c1->cd(4); h_imgg_pre->Draw();
    c1->Update();
    c1->SaveAs(Form("%s/pre_selection.png", outdir));

    c2->cd(1); h_X_post->Draw();
    c2->cd(2); h_Y_post->Draw();
    c2->cd(3); h_im_post->Draw();
    c2->cd(4); h_imgg_post->Draw();
    c2->Update();
    c2->SaveAs(Form("%s/post_selection.png", outdir));
}
