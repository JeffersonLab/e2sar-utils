#include "event_data.hpp"

void DalitzEventData::appendToBuffer(std::vector<double>& buf) const {
    buf.reserve(buf.size() + NUM_DOUBLES);
    buf.push_back(pi_plus.E());   buf.push_back(pi_plus.Px());
    buf.push_back(pi_plus.Py());  buf.push_back(pi_plus.Pz());
    buf.push_back(pi_minus.E());  buf.push_back(pi_minus.Px());
    buf.push_back(pi_minus.Py()); buf.push_back(pi_minus.Pz());
    buf.push_back(gamma1.E());    buf.push_back(gamma1.Px());
    buf.push_back(gamma1.Py());   buf.push_back(gamma1.Pz());
    buf.push_back(gamma2.E());    buf.push_back(gamma2.Px());
    buf.push_back(gamma2.Py());   buf.push_back(gamma2.Pz());
}

DalitzEventData DalitzEventData::fromBuffer(const double* p) {
    DalitzEventData ev;
    ev.pi_plus.SetPxPyPzE( p[1],  p[2],  p[3],  p[0]);
    ev.pi_minus.SetPxPyPzE(p[5],  p[6],  p[7],  p[4]);
    ev.gamma1.SetPxPyPzE(  p[9],  p[10], p[11], p[8]);
    ev.gamma2.SetPxPyPzE(  p[13], p[14], p[15], p[12]);
    return ev;
}

void GluexEventData::appendToBuffer(std::vector<double>& buf) const {
    buf.reserve(buf.size() + NUM_DOUBLES);
    buf.push_back(pip.E());  buf.push_back(pip.Px());
    buf.push_back(pip.Py()); buf.push_back(pip.Pz());
    buf.push_back(pim.E());  buf.push_back(pim.Px());
    buf.push_back(pim.Py()); buf.push_back(pim.Pz());
    buf.push_back(g1.E());   buf.push_back(g1.Px());
    buf.push_back(g1.Py());  buf.push_back(g1.Pz());
    buf.push_back(g2.E());   buf.push_back(g2.Px());
    buf.push_back(g2.Py());  buf.push_back(g2.Pz());
    buf.push_back(imass_kfit);
    buf.push_back(imassGG_kfit);
    buf.push_back(kfit_prob);
}

GluexEventData GluexEventData::fromBuffer(const double* p) {
    GluexEventData ev;
    ev.pip.SetPxPyPzE(p[1],  p[2],  p[3],  p[0]);
    ev.pim.SetPxPyPzE(p[5],  p[6],  p[7],  p[4]);
    ev.g1.SetPxPyPzE( p[9],  p[10], p[11], p[8]);
    ev.g2.SetPxPyPzE( p[13], p[14], p[15], p[12]);
    ev.imass_kfit   = p[16];
    ev.imassGG_kfit = p[17];
    ev.kfit_prob    = p[18];
    return ev;
}

TLorentzVector createLorentzVector(Double_t mag, Double_t theta, Double_t phi, Double_t mass) {
    TVector3 v;
    v.SetMagThetaPhi(mag, theta, phi);
    TLorentzVector lv;
    lv.SetVectM(v, mass);
    return lv;
}
