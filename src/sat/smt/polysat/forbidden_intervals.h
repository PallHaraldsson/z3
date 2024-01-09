/*++
Copyright (c) 2021 Microsoft Corporation

Module Name:

    Conflict explanation using forbidden intervals as described in
    "Solving bitvectors with MCSAT: explanations from bits and pieces"
    by S. Graham-Lengrand, D. Jovanovic, B. Dutertre.

Author:

    Nikolaj Bjorner (nbjorner) 2021-03-19
    Jakob Rath 2021-04-06

--*/
#pragma once
#include "sat/smt/polysat/types.h"
#include "sat/smt/polysat/interval.h"
#include "sat/smt/polysat/constraints.h"

namespace polysat {

    class core;

    struct fi_record {
        eval_interval               interval;
        vector<signed_constraint>   side_cond;
        vector<signed_constraint>   src;            // there is either 0 or 1 src.
        vector<dependency>          deps;
        rational                    coeff;
        unsigned                    bit_width = 0;  // number of lower bits; TODO: should move this to viable::entry; where the coeff/bit-width is adapted accordingly

        /** Create invalid fi_record */
        fi_record(): interval(eval_interval::full()) {}

        void reset() {
            interval = eval_interval::full();
            side_cond.reset();
            src.reset();
            coeff.reset();
            deps.reset();
            bit_width = 0;
        }

        struct less {
            bool operator()(fi_record const& a, fi_record const& b) const {
                return a.interval.lo_val() < b.interval.lo_val();
            }
        };
    };

    class forbidden_intervals {

        void push_eq(bool is_trivial, pdd const& p, vector<signed_constraint>& side_cond);
        eval_interval to_interval(signed_constraint const& c, bool is_trivial, rational& coeff,
                                  rational & lo_val, pdd & lo, rational & hi_val, pdd & hi);


        std::tuple<bool, rational, pdd, pdd> linear_decompose(pvar v, pdd const& p, vector<signed_constraint>& out_side_cond);

        bool match_linear1(signed_constraint const& c,
            rational const& a1, pdd const& b1, pdd const& e1,
            rational const& a2, pdd const& b2, pdd const& e2,
            fi_record& fi);

        bool match_linear2(signed_constraint const& c,
            rational const & a1, pdd const& b1, pdd const& e1,
            rational const & a2, pdd const& b2, pdd const& e2,
            fi_record& fi);

        bool match_linear3(signed_constraint const& c,
            rational const & a1, pdd const& b1, pdd const& e1,
            rational const & a2, pdd const& b2, pdd const& e2,
            fi_record& fi);

        bool match_linear4(signed_constraint const& c,
            rational const & a1, pdd const& b1, pdd const& e1,
            rational const & a2, pdd const& b2, pdd const& e2,
            fi_record& fi);

        void add_non_unit_side_conds(fi_record& fi, pdd const& b1, pdd const& e1, pdd const& b2, pdd const& e2);

        bool match_zero(signed_constraint const& c,
            rational const & a1, pdd const& b1, pdd const& e1,
            rational const & a2, pdd const& b2, pdd const& e2,
            fi_record& fi);

        bool match_max(signed_constraint const& c,
            rational const & a1, pdd const& b1, pdd const& e1,
            rational const & a2, pdd const& b2, pdd const& e2,
            fi_record& fi);

        bool match_non_zero(signed_constraint const& c,
            rational const& a1, pdd const& b1, pdd const& e1,
            pdd const& q,
            fi_record& fi);

        bool match_non_max(signed_constraint const& c,
            pdd const& p,
            rational const& a2, pdd const& b2, pdd const& e2,
            fi_record& fi);

        bool get_interval_ule(signed_constraint const& c, pvar v, fi_record& fi);

        bool get_interval_umul_ovfl(signed_constraint const& c, pvar v, fi_record& fi);

        struct backtrack {
            bool released = false;
            vector<signed_constraint>& side_cond;
            unsigned sz;
            backtrack(vector<signed_constraint>& s):side_cond(s), sz(s.size()) {}
            ~backtrack() {
                if (!released)
                    side_cond.shrink(sz);
            }
        };

        core& s;

    public:
        forbidden_intervals(core& s): s(s) {}
        bool get_interval(signed_constraint const& c, pvar v, fi_record& fi);
    };
}
