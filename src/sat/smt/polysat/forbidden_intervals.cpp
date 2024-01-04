/*++
Copyright (c) 2021 Microsoft Corporation

Module Name:

    Conflict explanation using forbidden intervals as described in
    "Solving bitvectors with MCSAT: explanations from bits and pieces"
    by S. Graham-Lengrand, D. Jovanovic, B. Dutertre.

Author:

    Jakob Rath 2021-04-06
    Nikolaj Bjorner (nbjorner) 2021-03-19

--*/
#include "sat/smt/polysat/forbidden_intervals.h"
#include "sat/smt/polysat/interval.h"
#include "sat/smt/polysat/umul_ovfl_constraint.h"
#include "sat/smt/polysat/ule_constraint.h"
#include "sat/smt/polysat/core.h"

namespace polysat {

    /**
     *
     * \param[in] c                 Original constraint
     * \param[in] v                 Variable that is bounded by constraint
     * \param[out] fi               "forbidden interval" record that captures values not allowed for v
     * \returns True iff a forbidden interval exists and the output parameters were set.
     */
    bool forbidden_intervals::get_interval(signed_constraint const& c, pvar v, fi_record& fi) {
        // verbose_stream() << "get_interval for v" << v << "    " << c << "\n";
        SASSERT(fi.side_cond.empty());
        SASSERT(fi.src.empty());
        fi.bit_width = s.size(v);  // TODO: preliminary
        if (c.is_ule())
            return get_interval_ule(c, v, fi);
        if (c.is_umul_ovfl())
            return get_interval_umul_ovfl(c, v, fi);
        return false;
    }

    bool forbidden_intervals::get_interval_umul_ovfl(signed_constraint const& c, pvar v, fi_record& fi) {
        using std::swap;

        backtrack _backtrack(fi.side_cond);

        fi.coeff = 1;
        fi.src.push_back(c);

        // eval(lhs) = a1*v + eval(e1) = a1*v + b1
        // eval(rhs) = a2*v + eval(e2) = a2*v + b2
        // We keep the e1, e2 around in case we need side conditions such as e1=b1, e2=b2.
        auto [ok1, a1, e1, b1] = linear_decompose(v, c.to_umul_ovfl().p(), fi.side_cond);
        auto [ok2, a2, e2, b2] = linear_decompose(v, c.to_umul_ovfl().q(), fi.side_cond);

        auto& m = e1.manager();
        rational bound = m.max_value();

        if (ok2 && !ok1) {
            swap(a1, a2);
            swap(e1, e2);
            swap(b1, b2);
            swap(ok1, ok2);
        }
        if (ok1 && !ok2 && a1.is_one() && b1.is_zero())  {
            if (c.is_positive()) {
                _backtrack.released = true;
                rational lo_val(0);
                rational hi_val(2);
                pdd lo = m.mk_val(lo_val);
                pdd hi = m.mk_val(hi_val);
                fi.interval = eval_interval::proper(lo, lo_val, hi, hi_val);
                return true;
            }
        }

        if (!ok1 || !ok2)
            return false;


        if (a2.is_one() && a1.is_zero()) {
            swap(a1, a2);
            swap(e1, e2);
            swap(b1, b2);
        }

        if (!a1.is_one() || !a2.is_zero())
            return false;

        if (!b1.is_zero())
            return false;

        _backtrack.released = true;

        // Ovfl(v, e2)


        if (c.is_positive()) {
            if (b2.val() <= 1) {
                fi.interval = eval_interval::full();
                fi.side_cond.push_back(s.cs().ule(e2, 1));
            }
            else {
                // A := div(2^N - 1, b2.val())
                //   := hi_val - 1
                // max B such that A*B < 2^N
                //     := ceil(2^N / A) - 1
                //     := div(2^N + A - 1, A) - 1
                //     := div(bound + A, A) - 1
                // [0, div(bound, b2.val()) + 1[
                rational A = div(bound, b2.val());
                rational B = div(bound + A, A) - 1;

                if (A >= 4 && B >= 4) {
                    _backtrack.released = false;
                    return false;
                }
                rational lo_val(0);
                rational hi_val = A + 1;
                pdd lo = m.mk_val(lo_val);
                pdd hi = m.mk_val(hi_val);
                
                SASSERT(b2.val() <= B);
                SASSERT(A * B <= bound);
                SASSERT((A + 1) * B > bound);
                SASSERT(A * (B + 1) > bound);
                fi.interval = eval_interval::proper(lo, lo_val, hi, hi_val);
                fi.side_cond.push_back(s.cs().ule(e2, B));                
            }

        }
        else {
            if (b2.val() <= 1) {
                _backtrack.released = false;
                return false;
            }
            else {
                // [div(bound, b2.val()) + 1, 0[
                // A := div(2^N - 1, b2.val())
                // min B . A*B >= 2^N
                //       := ceil(2^N / A)
                //       := div(2^N + A - 1, A)
                rational A = div(bound, b2.val()) + 1;
                rational B = div(bound + A, A);
                if (A >= 4 && B >= 4) {
                    _backtrack.released = false;
                    return false;
                }
                rational lo_val = A;
                rational hi_val(0);
                SASSERT(A * B > bound);
                SASSERT(A * (B - 1) <= bound);
                SASSERT((A - 1) * B <= bound);
                SASSERT(b2.val() >= B);

                pdd lo = m.mk_val(lo_val);
                pdd hi = m.mk_val(hi_val);
                fi.interval = eval_interval::proper(lo, lo_val, hi, hi_val);
                fi.side_cond.push_back(s.cs().ule(b2.val(), e2));
            }
        }

        // LOG("overflow interval " << fi.interval);

        return true;
    }

    static char const* _last_function = "";

    bool forbidden_intervals::get_interval_ule(signed_constraint const& c, pvar v, fi_record& fi) {

        backtrack _backtrack(fi.side_cond);

        fi.coeff = 1;
        fi.src.push_back(c);

        struct show {
            forbidden_intervals& f;
            signed_constraint const& c;
            pvar v;
            fi_record& fi;
            backtrack& _backtrack;
            show(forbidden_intervals& f,
                 signed_constraint const& c,
                 pvar v,
                 fi_record& fi,
                 backtrack& _backtrack):f(f), c(c), v(v), fi(fi), _backtrack(_backtrack) {}
            ~show() {
                if (!_backtrack.released)
                    return;
                IF_VERBOSE(0, verbose_stream() << _last_function << " " << v << " " << c << " " << fi.interval << " " << fi.side_cond << "\n");
            }
        };
        // uncomment to trace intervals
        // show _show(*this, c, v, fi, _backtrack);

        // eval(lhs) = a1*v + eval(e1) = a1*v + b1
        // eval(rhs) = a2*v + eval(e2) = a2*v + b2
        // We keep the e1, e2 around in case we need side conditions such as e1=b1, e2=b2.
        auto [ok1, a1, e1, b1] = linear_decompose(v, c.to_ule().lhs(), fi.side_cond);
        auto [ok2, a2, e2, b2] = linear_decompose(v, c.to_ule().rhs(), fi.side_cond);

        _backtrack.released = true;

        // v > q
        if (false && ok1 && !ok2 && match_non_zero(c, a1, b1, e1, c.to_ule().rhs(), fi))
            return true;

        // p > v
        if (false && !ok1 && ok2 && match_non_max(c, c.to_ule().lhs(), a2, b2, e2, fi))
            return true;

        if (!ok1 || !ok2 || (a1.is_zero() && a2.is_zero())) {
            _backtrack.released = false;
            return false;
        }
        SASSERT(b1.is_val());
        SASSERT(b2.is_val());

        // a*v + b <= 0, a odd
        // a*v + b > 0, a odd
        if (match_zero(c, a1, b1, e1, a2, b2, e2, fi))
            return true;

        // -1 <= a*v + b, a odd
        // -1 > a*v + b, a odd
        if (match_max(c, a1, b1, e1, a2, b2, e2, fi))
            return true;

        if (match_linear1(c, a1, b1, e1, a2, b2, e2, fi))
            return true;
        if (match_linear2(c, a1, b1, e1, a2, b2, e2, fi))
            return true;
        if (match_linear3(c, a1, b1, e1, a2, b2, e2, fi))
            return true;
        if (match_linear4(c, a1, b1, e1, a2, b2, e2, fi))
            return true;

        _backtrack.released = false;
        return false;
    }

    void forbidden_intervals::push_eq(bool is_zero, pdd const& p, vector<signed_constraint>& side_cond) {
        SASSERT(!p.is_val() || (is_zero == p.is_zero()));
        if (p.is_val())
            return;
        else if (is_zero)
            side_cond.push_back(s.eq(p));
        else
            side_cond.push_back(~s.eq(p));
    }

    std::tuple<bool, rational, pdd, pdd> forbidden_intervals::linear_decompose(pvar v, pdd const& p, vector<signed_constraint>& out_side_cond) {
        auto& m = p.manager();
        pdd q = m.zero();
        pdd e = m.zero();
        unsigned const deg = p.degree(v);
        if (deg == 0)
            // p = 0*v + e
            e = p;
        else if (deg == 1)
            // p = q*v + e
            p.factor(v, 1, q, e);
        else
            return std::tuple(false, rational(0), q, e);

        // r := eval(q)
        // Add side constraint q = r.
        if (!q.is_val()) {
            pdd r = s.subst(q);

            
            if (!r.is_val())
                return std::tuple(false, rational(0), q, e);
            out_side_cond.push_back(s.eq(q, r));
            q = r;
        }
        auto b = s.subst(e);
        return std::tuple(b.is_val(), q.val(), e, b);
    };

    eval_interval forbidden_intervals::to_interval(
        signed_constraint const& c, bool is_trivial, rational & coeff,
        rational & lo_val, pdd & lo,
        rational & hi_val, pdd & hi) {

        dd::pdd_manager& m = lo.manager();

        if (is_trivial) {
            if (c.is_positive())
                // TODO: we cannot use empty intervals for interpolation. So we
                // can remove the empty case (make it represent 'full' instead),
                // and return 'false' here. Then we do not need the proper/full
                // tag on intervals.
                return eval_interval::empty(m);
            else
                return eval_interval::full();
        }

        rational pow2 = m.two_to_N();

        if (coeff > pow2/2) {
            // TODO: if coeff != pow2 - 1, isn't this counterproductive now? considering the gap condition on refine-equal-lin acceleration.

            coeff = pow2 - coeff;
            SASSERT(coeff > 0);
            // Transform according to:  y \in [l;u[  <=>  -y \in [1-u;1-l[
            //      -y \in [1-u;1-l[
            //      <=>  -y - (1 - u) < (1 - l) - (1 - u)    { by: y \in [l;u[  <=>  y - l < u - l }
            //      <=>  u - y - 1 < u - l                   { simplified }
            //      <=>  (u-l) - (u-y-1) - 1 < u-l           { by: a < b  <=>  b - a - 1 < b }
            //      <=>  y - l < u - l                       { simplified }
            //      <=>  y \in [l;u[.
            lo = 1 - lo;
            hi = 1 - hi;
            swap(lo, hi);
            lo_val = mod(1 - lo_val, pow2);
            hi_val = mod(1 - hi_val, pow2);
            lo_val.swap(hi_val);
        }

        if (c.is_positive())
            return eval_interval::proper(lo, lo_val, hi, hi_val);
        else
            return eval_interval::proper(hi, hi_val, lo, lo_val);
    }

    /**
    * Match  e1 + t <= e2, with t = a1*y
    * condition for empty/full: e2 == -1
    */
    bool forbidden_intervals::match_linear1(signed_constraint const& c,
        rational const & a1, pdd const& b1, pdd const& e1,
        rational const & a2, pdd const& b2, pdd const& e2,
        fi_record& fi) {
        _last_function = __func__;
        if (a2.is_zero() && !a1.is_zero()) {
            SASSERT(!a1.is_zero());
            bool is_trivial = (b2 + 1).is_zero();
            push_eq(is_trivial, e2 + 1, fi.side_cond);
            auto lo = e2 - e1 + 1;
            rational lo_val = (b2 - b1 + 1).val();
            auto hi = -e1;
            rational hi_val = (-b1).val();
            fi.coeff = a1;
            fi.interval = to_interval(c, is_trivial, fi.coeff, lo_val, lo, hi_val, hi);
            add_non_unit_side_conds(fi, b1, e1, b2, e2);
            return true;
        }
        return false;
    }

    /**
     * e1 <= e2 + t, with t = a2*y
     * condition for empty/full: e1 == 0
     */
    bool forbidden_intervals::match_linear2(signed_constraint const& c,
        rational const & a1, pdd const& b1, pdd const& e1,
        rational const & a2, pdd const& b2, pdd const& e2,
        fi_record& fi) {
        _last_function = __func__;
        if (a1.is_zero() && !a2.is_zero()) {
            SASSERT(!a2.is_zero());
            bool is_trivial = b1.is_zero();
            push_eq(is_trivial, e1, fi.side_cond);
            auto lo = -e2;
            rational lo_val = (-b2).val();
            auto hi = e1 - e2;
            rational hi_val = (b1 - b2).val();
            fi.coeff = a2;
            fi.interval = to_interval(c, is_trivial, fi.coeff, lo_val, lo, hi_val, hi);
            add_non_unit_side_conds(fi, b1, e1, b2, e2);
            return true;
        }
        return false;
    }

    /**
     * e1 + t <= e2 + t, with t = a1*y = a2*y
     * condition for empty/full: e1 == e2
     */
    bool forbidden_intervals::match_linear3(signed_constraint const& c,
        rational const & a1, pdd const& b1, pdd const& e1,
        rational const & a2, pdd const& b2, pdd const& e2,
        fi_record& fi) {
        _last_function = __func__;
        if (a1 == a2 && !a1.is_zero()) {
            bool is_trivial = b1.val() == b2.val();
            push_eq(is_trivial, e1 - e2, fi.side_cond);
            auto lo = -e2;
            rational lo_val = (-b2).val();
            auto hi = -e1;
            rational hi_val = (-b1).val();
            fi.coeff = a1;
            fi.interval = to_interval(c, is_trivial, fi.coeff, lo_val, lo, hi_val, hi);
            add_non_unit_side_conds(fi, b1, e1, b2, e2);
            return true;
        }
        return false;
    }

    /**
     * e1 + t <= e2 + t', with t = a1*y, t' = a2*y, a1 != a2, a1, a2 non-zero
     */
    bool forbidden_intervals::match_linear4(signed_constraint const& c,
        rational const & a1, pdd const& b1, pdd const& e1,
        rational const & a2, pdd const& b2, pdd const& e2,
        fi_record& fi) {
        _last_function = __func__;
        if (a1 != a2 && !a1.is_zero() && !a2.is_zero()) {
            // NOTE: we don't have an interval here in the same sense as in the other cases.
            // We use the interval to smuggle out the values a1,b1,a2,b2 without adding additional fields.
            // to_interval flips a1,b1 with a2,b2 for negative constraints, which we also need for this case.
            auto lo = b1;
            rational lo_val = a1;
            auto hi = b2;
            rational hi_val = a2;
            // We use fi.coeff = -1 to tell the caller to treat it as a diseq_lin.
            fi.coeff = -1;
            fi.interval = to_interval(c, false, fi.coeff, lo_val, lo, hi_val, hi);
            add_non_unit_side_conds(fi, b1, e1, b2, e2);
            SASSERT(!fi.interval.is_currently_empty());
            return true;
        }
        return false;
    }

    /**
     * a*v <= 0, a odd
     * forbidden interval for v is [1;0[
     *
     * a*v + b <= 0, a odd
     * forbidden interval for v is [n+1;n[ where n = -b * a^-1
     *
     * TODO: extend to
     * 2^k*a*v <= 0, a odd
     * (using intervals for the lower bits of v)
     */
    bool forbidden_intervals::match_zero(
        signed_constraint const& c,
        rational const & a1, pdd const& b1, pdd const& e1,
        rational const & a2, pdd const& b2, pdd const& e2,
        fi_record& fi) {
        _last_function = __func__;
        if (a1.is_odd() && a2.is_zero() && b2.is_zero()) {
            auto& m = e1.manager();
            rational const& mod_value = m.two_to_N();
            rational a1_inv;
            VERIFY(a1.mult_inverse(m.power_of_2(), a1_inv));

            // interval for a*v + b > 0 is [n;n+1[ where n = -b * a^-1
            rational lo_val = mod(-b1.val() * a1_inv, mod_value);
            pdd lo          = -e1 * a1_inv;
            rational hi_val = mod(lo_val + 1, mod_value);
            pdd hi          = lo + 1;

            // interval for a*v + b <= 0 is the complement
            if (c.is_positive()) {
                std::swap(lo_val, hi_val);
                std::swap(lo, hi);
            }

            fi.coeff = 1;
            fi.interval = eval_interval::proper(lo, lo_val, hi, hi_val);
            // RHS == 0 is a precondition because we can only multiply with a^-1 in equations, not inequalities
            if (b2 != e2)
                fi.side_cond.push_back(s.eq(b2, e2));
            return true;
        }
        return false;
    }

    /**
     * -1 <= a*v + b, a odd
     * forbidden interval for v is [n+1;n[ where n = (-b-1) * a^-1
     */
    bool forbidden_intervals::match_max(
        signed_constraint const& c,
        rational const & a1, pdd const& b1, pdd const& e1,
        rational const & a2, pdd const& b2, pdd const& e2,
        fi_record& fi) {
        _last_function = __func__;
        if (a1.is_zero() && b1.is_max() && a2.is_odd()) {
            auto& m = e2.manager();
            rational const& mod_value = m.two_to_N();
            rational a2_inv;
            VERIFY(a2.mult_inverse(m.power_of_2(), a2_inv));

            // interval for -1 > a*v + b is [n;n+1[ where n = (-b-1) * a^-1
            rational lo_val = mod((-1 - b2.val()) * a2_inv, mod_value);
            pdd lo          = (-1 - e2) * a2_inv;
            rational hi_val = mod(lo_val + 1, mod_value);
            pdd hi          = lo + 1;

            // interval for -1 <= a*v + b is the complement
            if (c.is_positive()) {
                std::swap(lo_val, hi_val);
                std::swap(lo, hi);
            }

            fi.coeff = 1;
            fi.interval = eval_interval::proper(lo, lo_val, hi, hi_val);
            // LHS == -1 is a precondition because we can only multiply with a^-1 in equations, not inequalities
            if (b1 != e1)
                fi.side_cond.push_back(s.eq(b1, e1));
            return true;
        }
        return false;
    }

    /**
     * v > q
     * forbidden interval for v is [0,1[
     *
     * v - k > q
     * forbidden interval for v is [k,k+1[
     *
     * v > q
     * forbidden interval for v is [0;q+1[ but at least [0;1[
     *
     * The following cases are implemented, and subsume the simple ones above.
     *
     * v - k > q
     * forbidden interval for v is [k;k+q+1[ but at least [k;k+1[
     *
     * a*v - k > q, a odd
     * forbidden interval for v is [a^-1*k, a^-1*k + 1[
     */
    bool forbidden_intervals::match_non_zero(
        signed_constraint const& c,
        rational const& a1, pdd const& b1, pdd const& e1,
        pdd const& q,
        fi_record& fi) {
        _last_function = __func__;
        SASSERT(b1.is_val());
        if (a1.is_one() && c.is_negative()) {
            // v - k > q
            auto& m = e1.manager();
            rational const& mod_value = m.two_to_N();
            rational lo_val = (-b1).val();
            pdd lo = -e1;
            rational hi_val = mod(lo_val + 1, mod_value);
            pdd hi = lo + q + 1;
            fi.coeff = 1;
            fi.interval = eval_interval::proper(lo, lo_val, hi, hi_val);
            return true;
        }
        if (a1.is_odd() && c.is_negative()) {
            // a*v - k > q, a odd
            auto& m = e1.manager();
            rational const& mod_value = m.two_to_N();
            rational a1_inv;
            VERIFY(a1.mult_inverse(m.power_of_2(), a1_inv));
            rational lo_val(mod(-b1.val() * a1_inv, mod_value));
            auto lo = -e1 * a1_inv;
            rational hi_val(mod(lo_val + 1, mod_value));
            auto hi = lo + 1;
            fi.coeff = 1;
            fi.interval = eval_interval::proper(lo, lo_val, hi, hi_val);
            return true;
        }
        return false;
    }

    /**
     * p > v
     * forbidden interval for v is [p;0[ but at least [-1,0[
     *
     * p > v + k
     * forbidden interval for v is [p-k;-k[ but at least [-1-k,-k[
     *
     * p > a*v + k, a odd
     * forbidden interval for v is [ a^-1*(-1-k) ; a^-1*(-1-k) + 1 [
     */
    bool forbidden_intervals::match_non_max(
        signed_constraint const& c,
        pdd const& p,
        rational const& a2, pdd const& b2, pdd const& e2,
        fi_record& fi) {
        _last_function = __func__;
        SASSERT(b2.is_val());
        if (a2.is_one() && c.is_negative()) {
            // p > v + k
            auto& m = e2.manager();
            rational const& mod_value = m.two_to_N();
            rational hi_val = (-b2).val();
            pdd hi = -e2;
            rational lo_val = mod(hi_val - 1, mod_value);
            pdd lo = p - e2;
            fi.coeff = 1;
            fi.interval = eval_interval::proper(lo, lo_val, hi, hi_val);
            return true;
        }
        if (a2.is_odd() && c.is_negative()) {
            // p > a*v + k, a odd
            auto& m = e2.manager();
            rational const& mod_value = m.two_to_N();
            rational a2_inv;
            VERIFY(a2.mult_inverse(m.power_of_2(), a2_inv));
            rational lo_val = mod(a2_inv * (-1 - b2.val()), mod_value);
            pdd lo = a2_inv * (-1 - e2);
            rational hi_val = mod(lo_val + 1, mod_value);
            pdd hi = lo + 1;
            fi.coeff = 1;
            fi.interval = eval_interval::proper(lo, lo_val, hi, hi_val);
            return true;
        }
        return false;
    }


    void forbidden_intervals::add_non_unit_side_conds(fi_record& fi, pdd const& b1, pdd const& e1, pdd const& b2, pdd const& e2) {
        if (fi.coeff == 1)
            return;
        if (b1 != e1)
            fi.side_cond.push_back(s.eq(b1, e1));
        if (b2 != e2)
            fi.side_cond.push_back(s.eq(b2, e2));
    }
}
