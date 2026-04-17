void compare_histos(const char *datafile) {
    const char *names[] = {"h_X_pre","h_Y_pre","h_im_pre","h_imgg_pre",
                           "h_X_post","h_Y_post","h_im_post","h_imgg_post"};
    const int N = 8;

    // Run original macro — histograms land in memory
    gROOT->ProcessLine(Form(".x gluex_event_selection.C(\"%s\")", datafile));

    TH1F *orig[N];
    for (int i = 0; i < N; i++) {
        orig[i] = (TH1F*)gDirectory->FindObject(names[i]);
        orig[i]->SetName(Form("%s_orig", names[i]));
    }

    // Run factored macro
    gROOT->ProcessLine(Form(".x factored_gluex_analysis.C(\"%s\")", datafile));

    TH1F *fact[N];
    for (int i = 0; i < N; i++)
        fact[i] = (TH1F*)gDirectory->FindObject(names[i]);

    bool all_ok = true;
    for (int i = 0; i < N; i++) {
        if (!orig[i] || !fact[i]) {
            printf("MISSING histogram: %s\n", names[i]);
            all_ok = false;
            continue;
        }
        int nbins = orig[i]->GetNbinsX();
        bool match = (orig[i]->GetEntries() == fact[i]->GetEntries());
        for (int b = 0; b <= nbins+1; b++) {
            if (orig[i]->GetBinContent(b) != fact[i]->GetBinContent(b)) {
                match = false;
                printf("  BIN MISMATCH %s bin %d: orig=%.1f fact=%.1f\n",
                       names[i], b,
                       orig[i]->GetBinContent(b),
                       fact[i]->GetBinContent(b));
            }
        }
        printf("%s: entries orig=%.0f fact=%.0f  -> %s\n",
               names[i],
               orig[i]->GetEntries(), fact[i]->GetEntries(),
               match ? "OK" : "DIFFER");
        if (!match) all_ok = false;
    }
    printf("\n%s\n", all_ok ? "ALL HISTOGRAMS IDENTICAL" : "DIFFERENCES FOUND");
}
