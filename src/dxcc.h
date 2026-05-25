// Lyra — DXCC callsign → country (ISO-2) lookup.
//
// Loads a pre-baked prefix→ISO table (":/data/dxcc_prefix.csv",
// generated offline from country-files.com's cty.dat + a DXCC-name→ISO
// map — the same data old Lyra used).  Used to label DX-cluster spots
// with their country abbreviation, e.g. "US WE7CAT".
//
// Lookup mirrors old Lyra's DxccLookup.country_of: handle portable "/"
// calls, try an exact-call match, then the longest matching prefix.

#pragma once

#include <QHash>
#include <QString>

namespace lyra::ui {

class DxccLookup {
public:
    DxccLookup();                              // loads the table once

    bool    loaded() const { return !prefix_.isEmpty(); }
    QString isoOf(const QString &callsign) const;   // "" if unknown
    // "<ISO> <CALL>" when the country is known, else the bare callsign.
    QString enrich(const QString &callsign) const;

private:
    QHash<QString, QString> prefix_;   // PREFIX -> ISO2
    QHash<QString, QString> exact_;    // =CALL  -> ISO2
};

} // namespace lyra::ui
