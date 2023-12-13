/*++
Copyright (c) 2021 Microsoft Corporation

Module Name:

    Polysat core saturation

Author:

    Nikolaj Bjorner (nbjorner) 2021-03-19
    Jakob Rath 2021-04-6

--*/
#pragma once

#include "math/polysat/constraints.h"

namespace polysat {

    struct bilinear {
        rational a, b, c, d;

        rational eval(rational const& x, rational const& y) const {
            return a*x*y + b*x + c*y + d;
        }

        bilinear operator-() const {
            bilinear r(*this);
            r.a = -r.a;
            r.b = -r.b;
            r.c = -r.c;
            r.d = -r.d;
            return r;
        }

        bilinear operator-(bilinear const& other) const {
            bilinear r(*this);
            r.a -= other.a;
            r.b -= other.b;
            r.c -= other.c;
            r.d -= other.d;
            return r;
        }

        bilinear operator+(rational const& d) const {
            bilinear r(*this);
            r.d += d;
            return r;
        }
        
        bilinear operator-(rational const& d) const {
            bilinear r(*this);
            r.d -= d;
            return r;
        }

        bilinear operator-(int d) const {
            bilinear r(*this);
            r.d -= d;
            return r;
        }
};

    inline std::ostream& operator<<(std::ostream& out, bilinear const& b) {
        return out << b.a << "*x*y + " << b.b << "*x + " << b.c << "*y + " << b.d;
    }

    /**
     * Introduce lemmas that derive new (simpler) constraints from the current conflict and partial model.
     */
    class saturation {

        core& c;
        constraints& C;
        char const* m_rule = nullptr;
        
#if 0
        parity_tracker m_parity_tracker;
        unsigned_vector m_occ;
        unsigned_vector m_occ_cnt;

        void set_rule(char const* r) { m_rule = r; }

        bool is_non_overflow(pdd const& x, pdd const& y, signed_constraint& c);
        signed_constraint ineq(bool strict, pdd const& lhs, pdd const& rhs);

        void log_lemma(pvar v, conflict& core);
        bool propagate(pvar v, conflict& core, signed_constraint crit1, signed_constraint c);
        bool propagate(pvar v, conflict& core, inequality const& crit1, signed_constraint c);
        bool propagate(pvar v, conflict& core, signed_constraint c);
        bool add_conflict(pvar v, conflict& core, inequality const& crit1, signed_constraint c);
        bool add_conflict(pvar v, conflict& core, inequality const& crit1, inequality const& crit2, signed_constraint c);

        bool try_ugt_x(pvar v, conflict& core, inequality const& c);

        bool try_ugt_y(pvar v, conflict& core, inequality const& c);
        bool try_ugt_y(pvar v, conflict& core, inequality const& l_y, inequality const& yx_l_zx, pdd const& x, pdd const& z);

        bool try_y_l_ax_and_x_l_z(pvar x, conflict& core, inequality const& c);
        bool try_y_l_ax_and_x_l_z(pvar x, conflict& core, inequality const& x_l_z, inequality const& y_l_ax, pdd const& a, pdd const& y);

        bool try_ugt_z(pvar z, conflict& core, inequality const& c);
        bool try_ugt_z(pvar z, conflict& core, inequality const& x_l_z0, inequality const& yz_l_xz, pdd const& y, pdd const& x);

        bool try_parity(pvar x, conflict& core, inequality const& axb_l_y);
        bool try_parity_diseq(pvar x, conflict& core, inequality const& axb_l_y);
        bool try_mul_bounds(pvar x, conflict& core, inequality const& axb_l_y);
        bool try_factor_equality(pvar x, conflict& core, inequality const& a_l_b);
        bool try_infer_equality(pvar x, conflict& core, inequality const& a_l_b);
        bool try_mul_eq_1(pvar x, conflict& core, inequality const& axb_l_y);
        bool try_mul_odd(pvar x, conflict& core, inequality const& axb_l_y);
        bool try_mul_eq_bound(pvar x, conflict& core, inequality const& axb_l_y);
        bool try_transitivity(pvar x, conflict& core, inequality const& axb_l_y);
        bool try_tangent(pvar v, conflict& core, inequality const& c);
        bool try_add_overflow_bound(pvar x, conflict& core, inequality const& axb_l_y);
        bool try_add_mul_bound(pvar x, conflict& core, inequality const& axb_l_y);
        bool try_infer_parity_equality(pvar x, conflict& core, inequality const& a_l_b);
        bool try_div_monotonicity(conflict& core);

        bool try_nonzero_upper_extract(pvar v, conflict& core, inequality const& i);
        bool try_congruence(pvar v, conflict& core, inequality const& i);


        rational round(rational const& M, rational const& x);
        bool eval_round(rational const& M, pdd const& p, rational& r);
        bool extract_linear_form(pdd const& q, pvar& y, rational& a, rational& b);
        bool extract_bilinear_form(pvar x, pdd const& p, pvar& y, bilinear& b);
        bool adjust_bound(rational const& x_min, rational const& x_max, rational const& y0, rational const& M,
                          bilinear& b, rational& x_split);
        bool update_min(rational& y_min, rational const& x_min, rational const& x_max,
                        bilinear const& b);
        bool update_max(rational& y_max, rational const& x_min, rational const& x_max,
                        bilinear const& b);
        bool update_bounds_for_xs(rational const& x_min, rational const& x_max, rational& y_min, rational& y_max,
                                  rational const& y0, bilinear b1, bilinear b2,
                                  rational const& M, inequality const& a_l_b);
        void fix_values(pvar x, pvar y, pdd const& p);
        void fix_values(pvar y, pdd const& p);
        
        // c := lhs ~ v
        //  where ~ is < or <=
        bool is_l_v(pvar v, inequality const& c);

        // c := v ~ rhs
        bool is_g_v(pvar v, inequality const& c);

        // c := x ~ Y
        bool is_x_l_Y(pvar x, inequality const& i, pdd& y);

        // c := Y ~ x
        bool is_Y_l_x(pvar x, inequality const& i, pdd& y);

        // c := X*y ~ X*Z
        bool is_Xy_l_XZ(pvar y, inequality const& c, pdd& x, pdd& z);
        bool verify_Xy_l_XZ(pvar y, inequality const& c, pdd const& x, pdd const& z);

        // c := Y ~ Ax
        bool is_Y_l_Ax(pvar x, inequality const& c, pdd& a, pdd& y);
        bool verify_Y_l_Ax(pvar x, inequality const& c, pdd const& a, pdd const& y);

        // c := Ax ~ Y
        bool is_Ax_l_Y(pvar x, inequality const& c, pdd& a, pdd& y);
        bool verify_Ax_l_Y(pvar x, inequality const& c, pdd const& a, pdd const& y);

        // c := Ax + B ~ Y
        bool is_AxB_l_Y(pvar x, inequality const& c, pdd& a, pdd& b, pdd& y);
        bool verify_AxB_l_Y(pvar x, inequality const& c, pdd const& a, pdd const& b, pdd const& y);

        // c := Y ~ Ax + B
        bool is_Y_l_AxB(pvar x, inequality const& c, pdd& y, pdd& a, pdd& b);
        bool verify_Y_l_AxB(pvar x, inequality const& c, pdd const& y, pdd const& a, pdd& b);

        // c := Ax + B ~ Y, val(Y) = 0
        bool is_AxB_eq_0(pvar x, inequality const& c, pdd& a, pdd& b, pdd& y);
        bool verify_AxB_eq_0(pvar x, inequality const& c, pdd const& a, pdd const& b, pdd const& y);

        // c := Ax + B != Y, val(Y) = 0
        bool is_AxB_diseq_0(pvar x, inequality const& c, pdd& a, pdd& b, pdd& y);

        // c := Y*X ~ z*X
        bool is_YX_l_zX(pvar z, inequality const& c, pdd& x, pdd& y);
        bool verify_YX_l_zX(pvar z, inequality const& c, pdd const& x, pdd const& y);

        // c := xY <= xZ
        bool is_xY_l_xZ(pvar x, inequality const& c, pdd& y, pdd& z);

        // xy := x * Y
        bool is_xY(pvar x, pdd const& xy, pdd& y);

        // a * b does not overflow
        bool is_non_overflow(pdd const& a, pdd const& b);

        // p := coeff*x*y where coeff_x = coeff*x, x a variable
        bool is_coeffxY(pdd const& coeff_x, pdd const& p, pdd& y);

        bool is_add_overflow(pvar x, inequality const& i, pdd& y, bool& is_minus);

        bool has_upper_bound(pvar x, conflict& core, rational& bound, vector<signed_constraint>& x_ge_bound);

        bool has_lower_bound(pvar x, conflict& core, rational& bound, vector<signed_constraint>& x_le_bound);

        // inequality i implies x != 0
        bool is_nonzero_by(pvar x, inequality const& i);

        // determine min/max parity of polynomial
        unsigned min_parity(pdd const& p, vector<signed_constraint>& explain);
        unsigned max_parity(pdd const& p, vector<signed_constraint>& explain);
        unsigned min_parity(pdd const& p) { vector<signed_constraint> ex; return min_parity(p, ex); }
        unsigned max_parity(pdd const& p) { vector<signed_constraint> ex; return max_parity(p, ex); }

        lbool get_multiple(const pdd& p1, const pdd& p2, pdd& out);
        
        bool is_forced_eq(pdd const& p, rational const& val);
        bool is_forced_eq(pdd const& p, int i) { return is_forced_eq(p, rational(i)); }
        
        bool is_forced_diseq(pdd const& p, rational const& val, signed_constraint& c);
        bool is_forced_diseq(pdd const& p, int i, signed_constraint& c) { return is_forced_diseq(p, rational(i), c); }

        bool is_forced_odd(pdd const& p, signed_constraint& c);

        bool is_forced_false(signed_constraint const& sc);

        bool is_forced_true(signed_constraint const& sc);

        bool try_inequality(pvar v, inequality const& i);

        bool try_umul_ovfl(pvar v, signed_constraint c);
        bool try_umul_ovfl_noovfl(pvar v, signed_constraint c);
        bool try_umul_noovfl_lo(pvar v, signed_constraint c);
        bool try_umul_noovfl_bounds(pvar v, signed_constraint c);
        bool try_umul_ovfl_bounds(pvar v, signed_constraint c);

        bool try_op(pvar v, signed_constraint c);
#endif

    public:
        saturation(core& c);
        void perform(pvar v);
        bool perform(pvar v, signed_constraint sc);
    };
}
