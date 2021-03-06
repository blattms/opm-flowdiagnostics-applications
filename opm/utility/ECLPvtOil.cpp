/*
  Copyright 2017 Statoil ASA.

  This file is part of the Open Porous Media Project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <opm/utility/ECLPvtOil.hpp>

#include <opm/utility/ECLPropTable.hpp>
#include <opm/utility/ECLPvtCommon.hpp>
#include <opm/utility/ECLResultData.hpp>
#include <opm/utility/ECLUnitHandling.hpp>

#include <opm/parser/eclipse/Units/Units.hpp>

#include <cassert>
#include <cmath>
#include <exception>
#include <functional>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <ert/ecl/ecl_kw_magic.h>

namespace {
    ::Opm::ECLPVT::ConvertUnits
    deadOilUnitConverter(const ::Opm::ECLUnits::UnitSystem& usys)
    {
        using ToSI = ::Opm::ECLPVT::CreateUnitConverter::ToSI;

        // [ Po, 1/B, 1/(B*mu), d(1/B)/dPo, d(1/(B*mu))/dPo ]
        return ::Opm::ECLPVT::ConvertUnits {
            ToSI::pressure(usys),
            {
                ToSI::recipFvf(usys),
                ToSI::recipFvfVisc(usys),
                ToSI::recipFvfDerivPress(usys),
                ToSI::recipFvfViscDerivPress(usys)
            }
        };
    }

    ::Opm::ECLPVT::ConvertUnits deadOilUnitConverter(const int usys)
    {
        const auto u = ::Opm::ECLUnits::createUnitSystem(usys);

        return deadOilUnitConverter(*u);
    }

    std::pair< ::Opm::ECLPVT::ConvertUnits::Converter,
               ::Opm::ECLPVT::ConvertUnits>
    liveOilUnitConverter(const int usys)
    {
        using ToSI = ::Opm::ECLPVT::CreateUnitConverter::ToSI;

        const auto u = ::Opm::ECLUnits::createUnitSystem(usys);

        // Key   = Rs
        // Table = [ Po, 1/B, 1/(B*mu), d(1/B)/dPo, d(1/(B*mu))/dPo ]
        //       = dead oil table format.

        return std::make_pair(ToSI::disGas(*u),
                              deadOilUnitConverter(*u));
    }
}

// ---------------------------------------------------------------------

// Enable runtime selection of dead or live oil functions.
class PVxOBase
{
public:
    virtual std::vector<double>
    formationVolumeFactor(const std::vector<double>& rs,
                          const std::vector<double>& po) const = 0;

    virtual std::vector<double>
    viscosity(const std::vector<double>& rs,
              const std::vector<double>& po) const = 0;

    virtual std::vector<Opm::FlowDiagnostics::Graph>
    getPvtCurve(const Opm::ECLPVT::RawCurve curve) const = 0;

    virtual std::unique_ptr<PVxOBase> clone() const = 0;
};

// =====================================================================

class DeadOil : public PVxOBase
{
public:
    using ElemIt = ::Opm::ECLPVT::PVDx::ElemIt;
    using ConvertUnits = ::Opm::ECLPVT::ConvertUnits;

    DeadOil(ElemIt               xBegin,
            ElemIt               xEnd,
            const ConvertUnits&  convert,
            std::vector<ElemIt>& colIt)
        : interpolant_(xBegin, xEnd, convert, colIt)
    {}

    virtual std::vector<double>
    formationVolumeFactor(const std::vector<double>& /* rs */,
                          const std::vector<double>& po) const override
    {
        return this->interpolant_.formationVolumeFactor(po);
    }

    virtual std::vector<double>
    viscosity(const std::vector<double>& /* rs */,
              const std::vector<double>& po) const override
    {
        return this->interpolant_.viscosity(po);
    }

    virtual std::vector<Opm::FlowDiagnostics::Graph>
    getPvtCurve(const Opm::ECLPVT::RawCurve curve) const override
    {
        return { this->interpolant_.getPvtCurve(curve) };
    }

    virtual std::unique_ptr<PVxOBase> clone() const override
    {
        return std::unique_ptr<PVxOBase>(new DeadOil(*this));
    }

private:
    ::Opm::ECLPVT::PVDx interpolant_;
};

// =====================================================================

class LiveOil : public PVxOBase
{
public:
    using Extrap = ::Opm::Interp1D::PiecewisePolynomial::
        ExtrapolationPolicy::Linearly;

    using SubtableInterpolant = ::Opm::Interp1D::PiecewisePolynomial::
        Linear<Extrap, /* IsAscendingRange = */ true>;

    LiveOil(std::vector<double>              key,
            std::vector<SubtableInterpolant> propInterp)
        : interp_(std::move(key), std::move(propInterp))
    {}

    virtual std::vector<double>
    formationVolumeFactor(const std::vector<double>& rs,
                          const std::vector<double>& po) const override
    {
        // PKey   Inner   C0     C1         C2           C3
        // Rs     Po      1/B    1/(B*mu)   d(1/B)/dPo   d(1/(B*mu))/dPo
        //        :       :      :          :            :
        const auto key = TableInterpolant::PrimaryKey { rs };
        const auto x   = TableInterpolant::InnerVariate { po };

        return this->interp_.formationVolumeFactor(key, x);
    }

    virtual std::vector<double>
    viscosity(const std::vector<double>& rs,
              const std::vector<double>& po) const override
    {
        // PKey   Inner   C0     C1         C2           C3
        // Rs     Po      1/B    1/(B*mu)   d(1/B)/dPo   d(1/(B*mu))/dPo
        //        :       :      :          :            :
        const auto key = TableInterpolant::PrimaryKey { rs };
        const auto x   = TableInterpolant::InnerVariate { po };

        return this->interp_.viscosity(key, x);
    }

    virtual std::vector<Opm::FlowDiagnostics::Graph>
    getPvtCurve(const Opm::ECLPVT::RawCurve curve) const override
    {
        return this->interp_.getPvtCurve(curve);
    }

    virtual std::unique_ptr<PVxOBase> clone() const override
    {
        return std::unique_ptr<PVxOBase>(new LiveOil(*this));
    }

private:
    using TableInterpolant = ::Opm::ECLPVT::PVTx<SubtableInterpolant>;

    TableInterpolant interp_;

    std::vector<Opm::FlowDiagnostics::Graph>
    repackagePvtCurve(std::vector<Opm::FlowDiagnostics::Graph>&& graphs,
                      const Opm::ECLPVT::RawCurve                curve) const
    {
        if (curve != Opm::ECLPVT::RawCurve::SaturatedState) {
            // Not saturated state curve.  Nothing to do here.
            return graphs;
        }

        // Saturated state curve for live oil.  Columns are (Rs, Po).  Swap
        // first and second columns to normalised form: (Po, Rs).
        for (auto& graph : graphs) {
            graph.first.swap(graph.second);
        }

        return graphs;
    }
};

// #####################################################################

namespace {
    std::vector<std::unique_ptr<PVxOBase>>
    createDeadOil(const ::Opm::ECLPropTableRawData& raw,
                  const int                         usys)
    {
        using PVTInterp = std::unique_ptr<PVxOBase>;
        using ElmIt     = ::Opm::ECLPropTableRawData::ElementIterator;

        assert ((raw.numPrimary == 1) &&
                "Can't Create Dead Oil Function From Live Oil Table");

        const auto cvrt = deadOilUnitConverter(usys);

        return ::Opm::MakeInterpolants<PVTInterp>::fromRawData(raw,
            [&cvrt](ElmIt xBegin, ElmIt xEnd, std::vector<ElmIt>& colIt)
        {
            return PVTInterp{ new DeadOil(xBegin, xEnd, cvrt, colIt) };
        });
    }

    std::vector<double>
    extractPrimaryKey(const ::Opm::ECLPropTableRawData&             raw,
                      const ::Opm::ECLPropTableRawData::SizeType    t,
                      const ::Opm::ECLPVT::ConvertUnits::Converter& cvrtKey)
    {
        auto key = std::vector<double>{};
        key.reserve(raw.numPrimary);

        for (auto begin = std::begin(raw.primaryKey) + (t + 0)*raw.numPrimary,
                  end   = begin + raw.numPrimary;
             begin != end; ++begin)
        {
            if (std::abs(*begin) < 1.0e20) {
                key.push_back(cvrtKey(*begin));
            }
        }

        return key;
    }

    std::vector<std::unique_ptr<PVxOBase>>
    createLiveOil(const ::Opm::ECLPropTableRawData& raw,
                  const int                         usys)
    {
        auto ret = std::vector<std::unique_ptr<PVxOBase>>{};
        ret.reserve(raw.numTables);

        using Extrap = LiveOil::Extrap;
        using StI    = LiveOil::SubtableInterpolant;
        using ElemIt = ::Opm::ECLPropTableRawData::ElementIterator;

        const auto cvrt = liveOilUnitConverter(usys);

        auto sti = ::Opm::MakeInterpolants<StI>::fromRawData(raw,
            [&cvrt](ElemIt               xBegin,
                    ElemIt               xEnd,
                    std::vector<ElemIt>& colIt) -> StI
        {
            try {
                return StI(Extrap{}, xBegin, xEnd, colIt,
                           cvrt.second.indep,
                           cvrt.second.column);
            }
            catch (const std::invalid_argument&) {
                // No valid nodes.  Return invalid.
                return StI(Extrap{});
            }
        });

        for (auto t = 0*raw.numTables;
                  t <   raw.numTables; ++t)
        {
            auto key = extractPrimaryKey(raw, t, cvrt.first);

            const auto begin = (t + 0)*raw.numPrimary;
            const auto end   = begin + key.size();

            ret.emplace_back(new LiveOil(std::move(key),
                {
                    std::make_move_iterator(std::begin(sti) + begin),
                    std::make_move_iterator(std::begin(sti) + end)
                }));
        }

        return ret;
    }

    std::vector<std::unique_ptr<PVxOBase>>
    createPVTFunction(const ::Opm::ECLPropTableRawData& raw,
                      const int                         usys)
    {
        if (raw.numPrimary == 0) {
            // Malformed Gas PVT table.
            throw std::invalid_argument {
                "Oil PVT Table Without Primary Lookup Key"
            };
        }

        if (raw.numCols != 5) {
            throw std::invalid_argument {
                "PVT Table for Oil Must Have Five Columns"
            };
        }

        if (raw.primaryKey.size() != (raw.numPrimary * raw.numTables)) {
            throw std::invalid_argument {
                "Size Mismatch in RS Nodes of PVT Table for Oil"
            };
        }

        if (raw.data.size() !=
            (raw.numPrimary * raw.numRows * raw.numCols * raw.numTables))
        {
            throw std::invalid_argument {
                "Size Mismatch in Condensed Table Data "
                "of PVT Table for Oil"
            };
        }

        if (raw.numPrimary == 1) {
            return createDeadOil(raw, usys);
        }

        return createLiveOil(raw, usys);
    }

    std::vector<std::unique_ptr<PVxOBase>>
    clone(const std::vector<std::unique_ptr<PVxOBase>>& src)
    {
        auto dest = std::vector<std::unique_ptr<PVxOBase>>{};
        dest.reserve(src.size());

        for (const auto& p : src) {
            dest.push_back(p->clone());
        }

        return dest;
    }
}

// #####################################################################
// #####################################################################

// =====================================================================
// Class ECLPVT::Oil::Impl
// ---------------------------------------------------------------------

class Opm::ECLPVT::Oil::Impl
{
private:
    using EvalPtr = std::unique_ptr<PVxOBase>;

public:
    Impl(const ECLPropTableRawData& raw,
         const int                  usys,
         std::vector<double>        rhoS);

    Impl(const Impl& rhs);
    Impl(Impl&& rhs);

    Impl& operator=(const Impl& rhs);
    Impl& operator=(Impl&& rhs);

    using RegIdx = std::vector<EvalPtr>::size_type;

    std::vector<double>
    formationVolumeFactor(const RegIdx               region,
                          const std::vector<double>& rs,
                          const std::vector<double>& po) const;

    std::vector<double>
    viscosity(const RegIdx               region,
              const std::vector<double>& rs,
              const std::vector<double>& po) const;

    double surfaceMassDensity(const RegIdx region) const
    {
        this->validateRegIdx(region);

        return this->rhoS_[region];
    }

    std::vector<FlowDiagnostics::Graph>
    getPvtCurve(const RegIdx   region,
                const RawCurve curve) const;

private:
    std::vector<EvalPtr> eval_;
    std::vector<double>  rhoS_;

    void validateRegIdx(const RegIdx region) const;
};

Opm::ECLPVT::Oil::Impl::
Impl(const ECLPropTableRawData& raw,
     const int                  usys,
     std::vector<double>        rhoS)
    : eval_(createPVTFunction(raw, usys))
    , rhoS_(std::move(rhoS))
{}

Opm::ECLPVT::Oil::Impl::Impl(const Impl& rhs)
    : eval_(clone(rhs.eval_))
    , rhoS_(rhs.rhoS_)
{}

Opm::ECLPVT::Oil::Impl::Impl(Impl&& rhs)
    : eval_(std::move(rhs.eval_))
    , rhoS_(std::move(rhs.rhoS_))
{}

Opm::ECLPVT::Oil::Impl&
Opm::ECLPVT::Oil::Impl::operator=(const Impl& rhs)
{
    this->eval_ = clone(rhs.eval_);
    this->rhoS_ = rhs.rhoS_;

    return *this;
}

Opm::ECLPVT::Oil::Impl&
Opm::ECLPVT::Oil::Impl::operator=(Impl&& rhs)
{
    this->eval_ = std::move(rhs.eval_);
    this->rhoS_ = std::move(rhs.rhoS_);

    return *this;
}

std::vector<double>
Opm::ECLPVT::Oil::Impl::
formationVolumeFactor(const RegIdx               region,
                      const std::vector<double>& rs,
                      const std::vector<double>& po) const
{
    this->validateRegIdx(region);

    return this->eval_[region]->formationVolumeFactor(rs, po);
}

std::vector<double>
Opm::ECLPVT::Oil::Impl::
viscosity(const RegIdx               region,
          const std::vector<double>& rs,
          const std::vector<double>& po) const
{
    this->validateRegIdx(region);

    return this->eval_[region]->viscosity(rs, po);
}

std::vector<Opm::FlowDiagnostics::Graph>
Opm::ECLPVT::Oil::Impl::
getPvtCurve(const RegIdx   region,
            const RawCurve curve) const
{
    this->validateRegIdx(region);

    return this->eval_[region]->getPvtCurve(curve);
}

void
Opm::ECLPVT::Oil::Impl::validateRegIdx(const RegIdx region) const
{
    if (region >= this->eval_.size()) {
        throw std::invalid_argument {
            "Region Index " +
            std::to_string(region) +
            " Outside Valid Range (0 .. " +
            std::to_string(this->eval_.size() - 1) + ')'
        };
    }
}

// ======================================================================
// Class ECLPVT::Oil
// ----------------------------------------------------------------------

Opm::ECLPVT::Oil::Oil(const ECLPropTableRawData& raw,
                      const int                  usys,
                      std::vector<double>        rhoS)
    : pImpl_(new Impl(raw, usys, std::move(rhoS)))
{}

Opm::ECLPVT::Oil::~Oil()
{}

Opm::ECLPVT::Oil::Oil(const Oil& rhs)
    : pImpl_(new Impl(*rhs.pImpl_))
{}

Opm::ECLPVT::Oil::Oil(Oil&& rhs)
    : pImpl_(std::move(rhs.pImpl_))
{}

Opm::ECLPVT::Oil&
Opm::ECLPVT::Oil::operator=(const Oil& rhs)
{
    this->pImpl_.reset(new Impl(*rhs.pImpl_));

    return *this;
}

Opm::ECLPVT::Oil&
Opm::ECLPVT::Oil::operator=(Oil&& rhs)
{
    this->pImpl_ = std::move(rhs.pImpl_);

    return *this;
}

std::vector<double>
Opm::ECLPVT::Oil::
formationVolumeFactor(const int           region,
                      const DissolvedGas& rs,
                      const OilPressure&  po) const
{
    return this->pImpl_->formationVolumeFactor(region, rs.data, po.data);
}

std::vector<double>
Opm::ECLPVT::Oil::
viscosity(const int           region,
          const DissolvedGas& rs,
          const OilPressure&  po) const
{
    return this->pImpl_->viscosity(region, rs.data, po.data);
}

double Opm::ECLPVT::Oil::surfaceMassDensity(const int region) const
{
    return this->pImpl_->surfaceMassDensity(region);
}

std::vector<Opm::FlowDiagnostics::Graph>
Opm::ECLPVT::Oil::
getPvtCurve(const RawCurve curve,
            const int      region) const
{
    return this->pImpl_->getPvtCurve(region, curve);
}

// =====================================================================

std::unique_ptr<Opm::ECLPVT::Oil>
Opm::ECLPVT::CreateOilPVTInterpolant::
fromECLOutput(const ECLInitFileData& init)
{
    using OPtr = std::unique_ptr<Opm::ECLPVT::Oil>;

    const auto& ih   = init.keywordData<int>(INTEHEAD_KW);
    const auto  iphs = static_cast<unsigned int>(ih[INTEHEAD_PHASE_INDEX]);

    if ((iphs & (1u << 0)) == 0) {
        // Oil is not an active phase (unexpected).
        // Return sentinel (null) pointer.
        return OPtr{};
    }

    auto raw = ::Opm::ECLPropTableRawData{};

    const auto& tabdims = init.keywordData<int>("TABDIMS");
    const auto& tab     = init.keywordData<double>("TAB");

    raw.numPrimary = tabdims[ TABDIMS_NRPVTO_ITEM ]; // #Rs nodes/full table
    raw.numRows    = tabdims[ TABDIMS_NPPVTO_ITEM ]; // #Po nodes/sub-table
    raw.numCols    = 5; // [ Po, 1/B, 1/(B*mu), d(1/B)/dPo, d(1/(B*mu))/dPo ]
    raw.numTables  = tabdims[ TABDIMS_NTPVTO_ITEM ]; // # PVTO tables

    // Extract Primary Key (Rs)
    {
        const auto nTabElem = raw.numPrimary * raw.numTables;

        // Subtract one to account for 1-based indices.
        const auto start = tabdims[ TABDIMS_JBPVTO_OFFSET_ITEM ] - 1;

        raw.primaryKey.assign(&tab[start], &tab[start] + nTabElem);
    }

    // Extract Full Table
    {
        const auto nTabElem =
            raw.numPrimary * raw.numRows * raw.numCols * raw.numTables;

        // Subtract one to account for 1-based indices.
        const auto start = tabdims[ TABDIMS_IBPVTO_OFFSET_ITEM ] - 1;

        raw.data.assign(&tab[start], &tab[start] + nTabElem);
    }

    auto rhoS = surfaceMassDensity(init, ECLPhaseIndex::Liquid);

    return OPtr{
        new Oil(raw, ih[ INTEHEAD_UNIT_INDEX ], std::move(rhoS))
    };
}
