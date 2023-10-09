/*++
  Copyright (c) 2020 Microsoft Corporation

  Author:
  Nikolaj Bjorner (nbjorner)
  Lev Nachmanson (levnach)

  --*/
#pragma once

#include "math/lp/nla_common.h"
#include "math/lp/nla_intervals.h"
#include "util/uint_set.h"

namespace nla {
    class core;
    class monomial_bounds : common {
        dep_intervals& dep;

        void var2interval(lpvar v, scoped_dep_interval& i);
        bool is_too_big(mpq const& q) const;
        bool propagate_down(monic const& m, lpvar u);
        bool propagate_value(dep_interval& range, lpvar v);
        bool propagate_value(dep_interval& range, lpvar v, unsigned power);
        void compute_product(unsigned start, monic const& m, scoped_dep_interval& i);
        bool propagate(monic const& m);
        void propagate_fixed(monic const& m, rational const& k);
        void propagate_nonfixed(monic const& m, rational const& k, lpvar w);
        u_dependency* explain_fixed(monic const& m, rational const& k);
        lp::explanation get_explanation(u_dependency* dep);
        bool propagate_down(monic const& m, dep_interval& mi, lpvar v, unsigned power, dep_interval& product);
        void analyze_monomial(monic const& m, unsigned& num_free, lpvar& free_v, unsigned& power) const;
        bool is_free(lpvar v) const;
        bool is_zero(lpvar v) const;

        // monomial propagation
        void unit_propagate(monic & m);
        bool is_linear(monic const& m);
        rational fixed_var_product(monic const& m);
        lpvar non_fixed_var(monic const& m);
    public:
        monomial_bounds(core* core);
        void propagate();
        void unit_propagate();
    }; 
}
