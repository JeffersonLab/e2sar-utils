import ROOT
import numpy as np
import argparse
import tqdm
import time

# Set the LorentzVector, based on the information stored in the root tree:
def set_LorentzVector(mag,theta,phi,mass):
    vector = ROOT.TVector3(0.0,0.0,0.0)
    vector.SetMagThetaPhi(mag,theta,phi)

    lorentz_vector = ROOT.TLorentzVector(0.0,0.0,0.0,0.0)
    lorentz_vector.SetVectM(vector,mass)

    return lorentz_vector

# Analysis: This is where the physicists get excited... We will do a simple analysis that does some basic kinematic checks and
# Computes the observables that we wish to send to SAGIPS:
def analysis(pip,pim,g1,g2):
    # Compute invariant masses: --> Used to enforce kinematic limits and sent to SAGIPS:
    s_pippim = (pip + pim).M2()
    pi0 = g1 + g2
    m_pi0 = pi0.M()
    s_pippi0 = (pip + pi0).M2()
    s_pimpi0 = (pim + pi0).M2()
    pass_kinematic_check = False

    # Enforce kinematic limits:
    if np.sqrt(s_pippim) >= 0.278 and m_pi0 >= 0.08 and m_pi0 <= 0.15:
       pass_kinematic_check = True

    return s_pippim, s_pippi0, s_pimpi0, pass_kinematic_check


def read_out_tree(event):
    # Create particles via their reconstructed four-momentum vectors:
    # Positive pion:
    mag_plus_rec = float(getattr(event, "mag_plus_rec"))
    theta_plus_rec = float(getattr(event, "theta_plus_rec"))
    phi_plus_rec = float(getattr(event, "phi_plus_rec"))
    pi_plus = set_LorentzVector(mag_plus_rec,theta_plus_rec,phi_plus_rec,0.139)
    # Negative pion:
    mag_neg_rec = float(getattr(event, "mag_neg_rec"))
    theta_neg_rec = float(getattr(event, "theta_neg_rec"))
    phi_neg_rec = float(getattr(event, "phi_neg_rec"))
    pi_minus = set_LorentzVector(mag_neg_rec,theta_neg_rec,phi_neg_rec,0.139)
    # Gamma 1:
    mag_neutral1_rec = float(getattr(event, "mag_neutral1_rec"))
    theta_neutral1_rec = float(getattr(event, "theta_neutral1_rec"))
    phi_neutral1_rec = float(getattr(event, "phi_neutral1_rec"))
    gamma1 = set_LorentzVector(mag_neutral1_rec,theta_neutral1_rec,phi_neutral1_rec,0.0)
    # Gamma 2:
    mag_neutral2_rec = float(getattr(event, "mag_neutral2_rec"))
    theta_neutral2_rec = float(getattr(event, "theta_neutral2_rec"))
    phi_neutral2_rec = float(getattr(event, "phi_neutral2_rec"))
    gamma2 = set_LorentzVector(mag_neutral2_rec,theta_neutral2_rec,phi_neutral2_rec,0.0)

    # Run the analysis:
    return analysis(pi_plus,pi_minus,gamma1,gamma2)



def run(file_path,show_progressbar):
    with ROOT.TFile(file_path+".root","READ") as f:
        t = f['dalitz_root_tree']

        data_for_sagips = []
        t_start = time.time()
        for event in tqdm.tqdm(t,disable=not show_progressbar):
            s_pippim, s_pippi0, s_pimpi0, flag = read_out_tree(event)

            if flag:
                data_for_sagips.append(np.array([s_pippim,s_pippi0,s_pimpi0]))

        t_end = time.time()
        t_diff = t_end - t_start
        # Small sanity check:
        npy_sagips_data = np.stack(data_for_sagips)
        print(f"Took {t_diff}s to collect SAGIPS data with shape: {npy_sagips_data.shape}")

        # Now read data into EJFAT...

if __name__ == "__main__":

    # Not a fan of argparse, but here we go:
    parser = argparse.ArgumentParser()
    parser.add_argument("--file_path",type=str,default="dalitz_toy_data_0/dalitz_root_file_0",help="Full path to toy data")
    parser.add_argument("--progressbar",type=bool,default=False,help="Whether to show the progress bar")
    args = parser.parse_args()

    run(args.file_path,args.progressbar)
