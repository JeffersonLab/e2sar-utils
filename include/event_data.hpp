#pragma once
#include <TLorentzVector.h>
#include <TVector3.h>
#include <vector>
#include <cstddef>

// Abstract base for all event types.
// Each subclass knows its serialized size and how to pack/unpack itself.
class EventData {
public:
    virtual ~EventData() = default;
    virtual void appendToBuffer(std::vector<double>& buf) const = 0;
    virtual size_t numDoubles() const = 0;
    // Byte size of the serialized event.
    size_t size() const { return numDoubles() * sizeof(double); }
};

// Four-particle Dalitz decay event: π+π-γγ, stored as 16 doubles.
// Wire layout: [pip E px py pz][pim E px py pz][g1 E px py pz][g2 E px py pz]
class DalitzEventData : public EventData {
public:
    static constexpr size_t NUM_DOUBLES = 16;  // 4 particles × 4-vector

    TLorentzVector pi_plus;
    TLorentzVector pi_minus;
    TLorentzVector gamma1;
    TLorentzVector gamma2;

    void appendToBuffer(std::vector<double>& buf) const override;
    size_t numDoubles() const override { return NUM_DOUBLES; }
    static DalitzEventData fromBuffer(const double* p);
};

// GlueX kinematic-fit event: π+π-γγ + 3 kfit scalars, stored as 19 doubles.
// Wire layout: [pip E px py pz][pim E px py pz][g1 E px py pz][g2 E px py pz]
//              [imass_kfit][imassGG_kfit][kfit_prob]
// Branches: pip_p4_kin, pim_p4_kin, g1_p4_kin, g2_p4_kin, imass_kfit, imassGG_kfit, kfit_prob
class GluexEventData : public EventData {
public:
    static constexpr size_t NUM_DOUBLES = 19;  // 16 four-vector + 3 kfit scalars

    TLorentzVector pip;
    TLorentzVector pim;
    TLorentzVector g1;
    TLorentzVector g2;
    double imass_kfit   = 0.0;
    double imassGG_kfit = 0.0;
    double kfit_prob    = 0.0;

    void appendToBuffer(std::vector<double>& buf) const override;
    size_t numDoubles() const override { return NUM_DOUBLES; }
    static GluexEventData fromBuffer(const double* p);
};

// Build a TLorentzVector from spherical momentum coordinates and a particle mass.
TLorentzVector createLorentzVector(Double_t mag, Double_t theta, Double_t phi, Double_t mass);
