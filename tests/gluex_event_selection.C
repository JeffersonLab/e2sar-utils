#include <stdio.h>
#include <sys/stat.h> 
#include "TFile.h"
#include "TTree.h"
#include "TLorentzVector.h"
#include "TH1F.h"
#include "TMath.h"

void gluex_event_selection(const char *filename) {
   TFile *file = TFile::Open(filename);
   TTree *tree = (TTree*)file->Get("myTree");

   TLorentzVector *pip_rec = 0;
   TLorentzVector *pim_rec = 0;
   TLorentzVector *g1_rec = 0;
   TLorentzVector *g2_rec = 0;
   Double_t imassGG_kfit = 0.0;
   Double_t imass_kfit = 0.0;
   Double_t kfit_prob = 0.0;
   
   tree->SetBranchAddress("pip_p4_kin", &pip_rec);
   tree->SetBranchAddress("pim_p4_kin", &pim_rec);
   tree->SetBranchAddress("g1_p4_kin", &g1_rec);
   tree->SetBranchAddress("g2_p4_kin", &g2_rec);
   tree->SetBranchAddress("imass_kfit", &imass_kfit);
   tree->SetBranchAddress("imassGG_kfit", &imassGG_kfit);
   tree->SetBranchAddress("kfit_prob", &kfit_prob);

   TCanvas *c1 = new TCanvas("c1", "Pre Event Selection", 1200, 400);
   c1->Divide(2, 2);  // 2 columns, 2 rows

   TCanvas *c2 = new TCanvas("c2", "Post Event Selection", 1200, 400);
   c2->Divide(2, 2);  // 2 columns, 2 rows

   // Create histograms
   // Before event selection:
   TH1F *h_X_pre = new TH1F("h_X_pre"," ; X; Entries", 100, -1.5, 1.5);
   TH1F *h_Y_pre = new TH1F("h_Y_pre"," ; Y; Entries", 100, 0.0, 1.5);
   TH1F *h_im_pre = new TH1F("h_im_pre"," ; M [GeV]; Counts",100, 0.2, 1.0);
   TH1F *h_imgg_pre = new TH1F("h_imgg_pre"," ; M [GeV] ; Counts",100, 0.0, 0.3);
   // After event selection:
   TH1F *h_X_post = new TH1F("h_X_post"," ; X; Entries", 100, -1.5, 1.5);
   TH1F *h_Y_post = new TH1F("h_Y_post"," ; Y; Entries", 100, 0.0, 1.5);
   TH1F *h_im_post = new TH1F("h_im_post"," ; M [GeV]; Counts",100, 0.2, 1.0);
   TH1F *h_imgg_post = new TH1F("h_imgg_post"," ; M [GeV] ; Counts",100, 0.0, 0.3);

   Double_t M_ETA   = 0.547862;
   Double_t M_PIPM  = 0.139570; 
   Double_t M_PI0   = 0.134977; 

   for (Int_t i=0;i<tree->GetEntries(); i++){
      tree->GetEntry(i);

      

      TLorentzVector pi0 = *g1_rec + *g2_rec;
      Double_t s_pimpi0 = (*pim_rec + pi0).M2();
      Double_t s_pippi0 = (*pip_rec + pi0).M2();
      Double_t s_pippim = (*pip_rec + *pim_rec).M2();

      Double_t Q = M_ETA - 2*M_PIPM - M_PI0;

      // Centre of Dalitz plot
      Double_t s_centre = (M_ETA*M_ETA + 2*M_PIPM*M_PIPM + M_PI0*M_PI0) / 3.0;

      // Denominator (same for X and Y)
      Double_t denom = Q * (Q + 3*M_PI0);

      // Dalitz coordinates
      Double_t X = TMath::Sqrt(3) * (s_pimpi0 - s_pippi0) / denom;
      Double_t Y = 3 * (s_pippim - s_centre) / denom;
      
      h_X_pre->Fill(X);
      h_Y_pre->Fill(Y);
      h_im_pre->Fill(imass_kfit);
      h_imgg_pre->Fill(imassGG_kfit);

      // Event selection criteria:
      if (kfit_prob > 0.0001 && imass_kfit >= 0.45 && imass_kfit < 0.58 && imassGG_kfit > 0.1 && imassGG_kfit < 0.15){
        h_X_post->Fill(X);
        h_Y_post->Fill(Y);
        h_im_post->Fill(imass_kfit);
        h_imgg_post->Fill(imassGG_kfit);
      }

   }

   // Create folder to store histograms:
   const char *outdir = "histograms_event_selection";
   mkdir(outdir, 0755);

   c1->cd(1);
   h_X_pre->Draw();
   c1->cd(2);
   h_Y_pre->Draw();
   c1->cd(3);
   h_im_pre->Draw();
   c1->cd(4);
   h_imgg_pre->Draw();
   c1->Update();
   c1->SaveAs(Form("%s/pre_selection.png", outdir));

   c2->cd(1);
   h_X_post->Draw();
   c2->cd(2);
   h_Y_post->Draw();
   c2->cd(3);
   h_im_post->Draw();
   c2->cd(4);
   h_imgg_post->Draw();
   c2->Update();
   c2->SaveAs(Form("%s/post_selection.png", outdir));

}


