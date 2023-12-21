/*++
Copyright (c) 2021 Microsoft Corporation

Module Name:

    Polysat core saturation

Author:

    Nikolaj Bjorner (nbjorner) 2021-03-19
    Jakob Rath 2021-04-6


TODO: preserve falsification
- each rule selects a certain premises that are problematic.
  If the problematic premise is false under the current assignment, the newly inferred
  literal should also be false in the assignment in order to preserve conflicts.


TODO: when we check that 'x' is "unary":
- in principle, 'x' could be any polynomial. However, we need to divide the lhs by x, and we don't have general polynomial division yet.
  so for now we just allow the form 'value*variable'.
   (extension to arbitrary monomials for 'x' should be fairly easy too)

--*/
#include "sat/smt/polysat/core.h"
#include "sat/smt/polysat/saturation.h"
#include "sat/smt/polysat/umul_ovfl_constraint.h"
#include "sat/smt/polysat/ule_constraint.h"
#include "util/log.h"


namespace polysat {

    saturation::saturation(core& c) : c(c), C(c.cs()) {}

    void saturation::propagate(pvar v) {
        for (auto id : c.unsat_core())
            propagate(v, id);
    }

    bool saturation::propagate(pvar v, constraint_id id) {
        if (c.eval(id) == l_true)
            return false;
        auto sc = c.get_constraint(id);
        m_propagated = false;
        if (sc.is_ule())
            propagate(v, inequality::from_ule(c, id));
        else
            ;

        return m_propagated;
    }

    void saturation::propagate(pvar v, inequality const& i) {
        if (c.size(v) != i.lhs().power_of_2())
            return;
        propagate_infer_equality(v, i);
        try_ugt_x(v, i);
        return;

    }

    void saturation::propagate(signed_constraint const& sc, std::initializer_list<constraint_id> const& premises) {
        if (c.propagate(sc, premises))
            m_propagated = true;
    }

    void saturation::add_clause(char const* name, core_vector const& cs, bool is_redundant) {
        if (c.add_clause(name, cs, is_redundant))
            m_propagated = true;
    }

    bool saturation::match_core(std::function<bool(signed_constraint const& sc)> const& p, constraint_id& id_out) {
        for (auto id : c.unsat_core()) {
            auto sc = c.get_constraint(id);
            if (p(sc)) {
                id_out = id;
                return true;
            }
        }
        return false;
    }

    bool saturation::match_constraints(std::function<bool(signed_constraint const& sc)> const& p, constraint_id& id_out) {
        for (auto id : c.assigned_constraints()) {
            auto sc = c.get_constraint(id);
            if (p(sc)) {
                id_out = id;
                return true;
            }
        }
        return false;
    }

    signed_constraint saturation::ineq(bool is_strict, pdd const& x, pdd const& y) {
        return is_strict ? c.cs().ult(x, y) : c.cs().ule(x, y);
    }

    /**
     * p <= q, q <= p => p = q
    */
    void saturation::propagate_infer_equality(pvar x, inequality const& i) {
        set_rule("[x] p <= q, q <= p => p - q = 0");
        if (i.is_strict())
            return;
        if (i.lhs().degree(x) == 0 && i.rhs().degree(x) == 0)
            return;
        constraint_id id;
        if (!match_core([&](auto const& sc) { return sc.is_ule() && !sc.sign() && sc.to_ule().lhs() == i.rhs() && sc.to_ule().rhs() == i.lhs(); }, id))
            return;
        c.propagate(c.eq(i.lhs(), i.rhs()), { id, i.id() });     
    }

    /**
     * Implement the inferences
     *  [x] yx < zx   ==>  Ω*(x,y) \/ y < z
     *  [x] yx <= zx  ==>  Ω*(x,y) \/ y <= z \/ x = 0
     */
    void saturation::try_ugt_x(pvar v, inequality const& i) {
        pdd x = c.var(v);
        pdd y = x;
        pdd z = x;       
        auto& C = c.cs();

        if (!i.is_xY_l_xZ(v, y, z))
            return;

        auto ovfl = C.umul_ovfl(x, y);
        if (i.is_strict()) 
            add_clause("[x] yx < zx ==>  Ω*(x,y) \\/ y < z", { i.dep(), ovfl, C.ult(y, z)}, false);
        else 
            add_clause("[x] yx <= zx  ==>  Ω*(x,y) \\/ y <= z \\/ x = 0", { i.dep(), ovfl, C.eq(x), C.ule(y, z) }, false);
    }

    /**
     * [y] z' <= y /\ yx <= zx  ==>  Ω*(x,y) \/ z'x <= zx
     * [y] z' <= y /\ yx <  zx  ==>  Ω*(x,y) \/ z'x <  zx
     * [y] z' <  y /\ yx <= zx  ==>  Ω*(x,y) \/ z'x <= zx
     * [y] z' <  y /\ yx <  zx  ==>  Ω*(x,y) \/ z'x <  zx
     * [y] z' <  y /\ yx <  zx  ==>  Ω*(x,y) \/ z'x + 1 < zx     (TODO?)
     * [y] z' <  y /\ yx <  zx  ==>  Ω*(x,y) \/ (z' + 1)x < zx   (TODO?)
     */
    void saturation::try_ugt_y(pvar v, inequality const& i) {
        auto y = c.var(v);
        pdd x = y;
        pdd z = y;
        auto& C = c.cs();
        constraint_id id;
        if (!i.is_Xy_l_XZ(v, x, z))
            return;
        if (!match_constraints([&](auto const& sc) { return inequality::is_l_v(y, sc); }, id))
            return;

        auto j = inequality::from_ule(c, id);
        pdd const& z_prime = i.lhs();
        bool is_strict = i.is_strict() || j.is_strict();
        add_clause("[y] z' <= y & yx <= zx", { i, j, C.umul_ovfl(x, y), ineq(is_strict, z_prime * x, z * x) }, false);
    }

    /**
     * [z] z <= y' /\ yx <= zx  ==>  Ω*(x,y') \/ yx <= y'x
     * [z] z <= y' /\ yx <  zx  ==>  Ω*(x,y') \/ yx <  y'x
     * [z] z <  y' /\ yx <= zx  ==>  Ω*(x,y') \/ yx <= y'x
     * [z] z <  y' /\ yx <  zx  ==>  Ω*(x,y') \/ yx <  y'x
     * [z] z <  y' /\ yx <  zx  ==>  Ω*(x,y') \/ yx+1 < y'x     (TODO?)
     * [z] z <  y' /\ yx <  zx  ==>  Ω*(x,y') \/ (y+1)x < y'x   (TODO?)
     */
    void saturation::try_ugt_z(pvar v, inequality const& i) {
        auto z = c.var(v);
        pdd y = z;
        pdd x = z;
        constraint_id id;
        if (!i.is_YX_l_zX(v, x, y))
            return;
        if (!match_constraints([&](auto const& sc) { return inequality::is_g_v(z, sc); }, id))
            return;
        auto j = inequality::from_ule(c, id);
        auto y_prime = j.rhs();
        bool is_strict = i.is_strict() || j.is_strict();
        add_clause("[z] z <= y' && yx <= zx", { i, j, c.umul_ovfl(x, y_prime), ineq(is_strict, y * x, y_prime * x) }, false);
    }


    /**
     * Determine whether values of x * y is non-overflowing.
     */
    bool saturation::is_non_overflow(pdd const& x, pdd const& y) {
        rational x_val, y_val;
        rational bound = x.manager().two_to_N();
        return c.try_eval(x, x_val) && c.try_eval(y, y_val) && x_val * y_val < bound;
    }

    bool saturation::is_non_overflow(pdd const& x, pdd const& y, signed_constraint& sc) {
        return is_non_overflow(x, y) && (sc = c.umul_ovfl(x, y), true);
    }

#if 0


    bool saturation::try_inequality(pvar v, inequality const& i, conflict& core) {
        bool prop = false;
        if (s.size(v) != i.lhs().power_of_2())
            return false;
        if (try_nonzero_upper_extract(v, core, i))
            prop = true;
        if (try_congruence(v, core, i))
            prop = true;
        if (try_mul_bounds(v, core, i))
            prop = true;
        if (try_parity(v, core, i))
            prop = true;
        if (try_parity_diseq(v, core, i))
            prop = true;
        if (try_transitivity(v, core, i))
            prop = true;
        if (try_factor_equality(v, core, i))
            prop = true;
        if (try_infer_equality(v, core, i))
            prop = true;
        if (try_add_overflow_bound(v, core, i))
            prop = true;
        if (try_add_mul_bound(v, core, i))
            prop = true;
        if (try_infer_parity_equality(v, core, i))
            prop = true;
        if (try_mul_eq_bound(v, core, i))
            prop = true;
        if (try_y_l_ax_and_x_l_z(v, core, i))            
            prop = true;
        if (false && try_tangent(v, core, i))
            prop = true;
        return prop;
    }


    bool saturation::try_nonzero_upper_extract(pvar y, conflict& core, inequality const& i) {
        set_rule("y = x[h:l] & y != 0 ==> x >= 2^l");
        if (!s.m_justification[y].is_propagation_by_slicing())
            return false;
        if (!s.get_value(y).is_zero())
            return false;
        if (!is_nonzero_by(y, i))
            return false;
        for (pvar x : core.vars()) {
            if (!s.get_value(x).is_zero())
                continue;
            unsigned hi, lo;
            if (!s.m_slicing.is_extract(y, x, hi, lo))  // TODO: generalize; use is_equal to check this and if yes, extract the explanation. otherwise it will only work in very limited cases.
                continue;
            if (propagate(y, core, i, s.ule(rational::power_of_two(lo), s.var(x))))
                return true;
        }
        return false;
    }

    // TODO: can be generalized
    bool saturation::is_nonzero_by(pvar x, inequality const& i) {
        if (i.is_strict() && i.lhs().is_zero()) {
            // 0 < p
            pdd const& p = i.rhs();
            if (p.is_val())
                return false;
            if (!p.lo().is_zero())
                return false;
            if (!p.hi().is_val())
                return false;
            SASSERT(!p.hi().is_zero());
            // 0 < a*x for a != 0
            return true;
        }
        return false;
    }

    bool saturation::try_umul_ovfl(pvar v, signed_constraint c, conflict& core) {
        SASSERT(c->is_umul_ovfl());
        bool prop = false;
        if (try_umul_ovfl_noovfl(v, c, core))
            prop = true;
#if 0
        if (c.is_positive()) {
            prop = try_umul_ovfl_bounds(v, c, core);
        }
        else {
            prop = try_umul_noovfl_bounds(v, c, core);
            if (false && try_umul_noovfl_lo(v, c, core))
                prop = true;
        }
#endif
        return prop;
    }

    // Ovfl(x, y) & ~Ovfl(y, z) ==> x > z
    // TODO: Ovfl(x, y1) & ~Ovfl(y2, z) & y1 <= y2 ==> x > z
    bool saturation::try_umul_ovfl_noovfl(pvar v, signed_constraint c1, conflict& core) {
        set_rule("[y] ovfl(x, y) & ~ovfl(y, z) ==> x > z");
        SASSERT(c1->is_umul_ovfl());
        if (!c1.is_positive())  // since we search for both premises in the core, break symmetry by requiring c1 to be positive
            return false;
        pdd p = c1->to_umul_ovfl().p();
        pdd q = c1->to_umul_ovfl().q();
        for (auto c2 : core) {
            if (!c2.is_negative())
                continue;
            if (!c2->is_umul_ovfl())
                continue;
            pdd r = c2->to_umul_ovfl().p();
            pdd u = c2->to_umul_ovfl().q();
            if (p == u || q == u)
                swap(r, u);
            if (q == r || q == u)
                swap(p, q);
            if (p != r)
                continue;
            m_lemma.reset();
            m_lemma.insert(~c1);
            m_lemma.insert(~c2);
            if (propagate(v, core, s.ult(u, q)))
                return true;
        }
        return false;
    }

    bool saturation::try_umul_noovfl_lo(pvar v, signed_constraint c, conflict& core) {
        set_rule("[x] ~ovfl(x, y) => y = 0 or x <= x * y");
        SASSERT(c->is_umul_ovfl());
        if (!c.is_negative())
            return false;
        auto const& ovfl = c->to_umul_ovfl();
        auto V = s.var(v);
        auto p = ovfl.p(), q = ovfl.q();
        // TODO could relax condition to be that V occurs in p
        if (q == V) 
            std::swap(p, q);
        signed_constraint q_eq_0;
        if (p == V && is_forced_diseq(q, 0, q_eq_0)) {
            // ~ovfl(V,q) => q = 0 or V <= V*q
            m_lemma.reset();
            m_lemma.insert_eval(q_eq_0);
            if (propagate(v, core, c, s.ule(p, p * q)))
                return true;
        }
        return false;
    }

    /**
     * ~ovfl(p, q) & p >= k => q < 2^N/k
     * TODO: subsumed by narrowing inferences?
     */
    bool saturation::try_umul_noovfl_bounds(pvar x, signed_constraint c, conflict& core) {
        set_rule("[x] ~ovfl(x, q) & x >= k => q <= (2^N-1)/k");
        SASSERT(c->is_umul_ovfl());
        SASSERT(c.is_negative());
        auto const& ovfl = c->to_umul_ovfl();        
        auto p = ovfl.p(), q = ovfl.q();
        auto X = s.var(x);
        auto& m = p.manager();
        rational p_val, q_val;
        if (q == X) 
            std::swap(p, q);
        if (p == X) {
            vector<signed_constraint> x_ge_bound;
            if (!s.try_eval(q, q_val))
                return false;
            if (!has_lower_bound(x, core, p_val, x_ge_bound))
                return false;
            if (p_val * q_val <= m.max_value())
                return false;
            m_lemma.reset();
            m_lemma.insert_eval(~s.uge(X, p_val));
            signed_constraint conseq = s.ule(q, floor(m.max_value()/p_val));
            return propagate(x, core, c, conseq);
        }
        if (!s.try_eval(p, p_val) || !s.try_eval(q, q_val))
            return false;
        if (p_val * q_val <= m.max_value())
            return false;
        m_lemma.reset();
        m_lemma.insert_eval(~s.uge(q, q_val));
        signed_constraint conseq = s.ule(p, floor(m.max_value()/q_val));
        return propagate(x, core, c, conseq);
    }

   /**
    * ovfl(p, q) & p <= k => q > 2^N/k
    */
    bool saturation::try_umul_ovfl_bounds(pvar v, signed_constraint c, conflict& core) {
        SASSERT(c->is_umul_ovfl());
        SASSERT(c.is_positive());
        auto const& ovfl = c->to_umul_ovfl();
        auto p = ovfl.p(), q = ovfl.q();
        rational p_val, q_val;
        return false;
    }

    bool saturation::try_op(pvar v, signed_constraint c, conflict& core) {
        set_rule("try_op");
        SASSERT(c->is_op());
        SASSERT(c.is_positive());
        clause_ref correction = c.produce_lemma(s, s.get_assignment());
        if (correction) {
            IF_LOGGING(
                LOG("correcting op_constraint: " << *correction);
                for (sat::literal lit : *correction) {
                    LOG("\t" << lit_pp(s, lit));
                }
            );

            for (sat::literal lit : *correction)
                if (!s.m_bvars.is_assigned(lit) && s.lit2cnstr(lit).is_currently_false(s))
                    s.assign_eval(~lit);
            core.add_lemma(correction);
            log_lemma(v, core);
            return true;
        }
        return false;
    }

    bool saturation::is_non_overflow(pdd const& x, pdd const& y, signed_constraint& c) {
        
        if (is_non_overflow(x, y)) {
            c = ~s.umul_ovfl(x, y);
            return true;
        }

        // TODO: do we really search the stack or can we just create the literal s.umul_ovfl(x, y)
        // and check if it is assigned, or not even create the literal but look up whether it is assigned?
        // constraint_manager uses m_dedup, alloc
        // but to probe whether a literal occurs these are not needed.
        // m_dedup.constraints.contains(&c);
        
        for (auto si : s.m_search) {
            if (!si.is_boolean())
                continue;
            if (si.is_resolved())
                continue;
            auto d = s.lit2cnstr(si.lit());
            if (!d->is_umul_ovfl() || !d.is_negative())
                continue;
            auto const& ovfl = d->to_umul_ovfl();
            if (x != ovfl.p() && x != ovfl.q())
                continue;
            if (y != ovfl.p() && y != ovfl.q())
                continue;
            c = d;
            return true;
        }
        return false;
    }

 

    // 
    // overall comment: we use value propagation to check if p is val
    // but we could also use literal propagation and establish there is a literal p = 0 that is true.
    // in this way the value of p doesn't have to be fixed.
    // 
    // is_forced_diseq already creates a literal.
    // is_non_overflow also creates a literal
    // 
    // The condition that p = val may be indirect.
    // it could be a literal
    // it could be by propagation of literals
    // Example:
    //  -35: v90 + v89*v43 + -1*v87 != 0     [ l_false bprop@0 pwatched ]
    //   36: ovfl*(v43, v89)                 [ l_false bprop@0 pwatched ]
    // -218: v90 + -1*v87 + -1 != 0          [ l_false eval@6 pwatched ]
    // 
    // what should we "pay" to establish this condition?
    // or do we just afford us to add this lemma?
    // 

    bool saturation::is_forced_eq(pdd const& p, rational const& val) {
        rational pv;
        if (s.try_eval(p, pv) && pv == val)
            return true;
        return false;
    }

    bool saturation::is_forced_diseq(pdd const& p, rational const& val, signed_constraint& c) {
        c = s.eq(p, val);
        return is_forced_false(c);
    }

    bool saturation::is_forced_odd(pdd const& p, signed_constraint& c) {
        c = s.odd(p);
        return is_forced_true(c);
    }

    bool saturation::is_forced_false(signed_constraint const& c) {
        return c.bvalue(s) == l_false || c.is_currently_false(s);
    }

    bool saturation::is_forced_true(signed_constraint const& c) {
        return c.bvalue(s) == l_true || c.is_currently_true(s);
    }








    /**
     * [x]  y <= ax /\ x <= z  (non-overflow case)
     *     ==>   Ω*(a, z)  \/  y <= az
     * ... (other combinations of <, <=)
     */
    bool saturation::try_y_l_ax_and_x_l_z(pvar x, conflict& core, inequality const& y_l_ax) {
        set_rule("[x] y <= ax & x <= z");
        auto& m = s.var2pdd(x);
        pdd y = m.zero();
        pdd a = m.zero();
        if (!is_Y_l_Ax(x, y_l_ax, a, y))
            return false;
        if (a.is_one())
            return false;
        for (auto si : s.m_search) {
            if (!si.is_boolean())
                continue;
            if (si.is_resolved())
                continue;
            auto d = s.lit2cnstr(si.lit());
            if (!d->is_ule())
                continue;
            auto x_l_z = inequality::from_ule(d);
            if (is_g_v(x, x_l_z) && try_y_l_ax_and_x_l_z(x, core, y_l_ax, x_l_z, a, y))
                return true;
        }
        return false;
    }

    bool saturation::try_y_l_ax_and_x_l_z(pvar x, conflict& core, inequality const& y_l_ax, inequality const& x_l_z, pdd const& a, pdd const& y) {
        SASSERT(is_g_v(x, x_l_z));
        SASSERT(verify_Y_l_Ax(x, y_l_ax, a, y));
        pdd const& z = x_l_z.rhs();
        signed_constraint non_ovfl;
        if (!is_non_overflow(a, z, non_ovfl))
            return false;
        m_lemma.reset();
        m_lemma.insert_eval(~non_ovfl);
        return add_conflict(x, core, y_l_ax, x_l_z, ineq(x_l_z.is_strict() || y_l_ax.is_strict(), y, a * z));
    }

    /**
     * [x] a <= k & a*x + b = 0 & b = 0 => a = 0 or x = 0 or x >= 2^K/k
     * [x] x <= k & a*x + b = 0 & b = 0 => x = 0 or a = 0 or a >= 2^K/k
     * Better?
     * [x] a*x + b = 0 & b = 0 => a = 0 or x = 0 or Ω*(a, x)     
     * We need up to four versions of this for all sign combinations of a, x
     */
    bool saturation::try_mul_bounds(pvar x, conflict& core, inequality const& axb_l_y) {
        set_rule("[x] a*x + b = 0 & b = 0 => a = 0 or x = 0 or ovfl(a, x)");
        auto& m = s.var2pdd(x);
        pdd y = m.zero();
        pdd a = m.zero();
        pdd b = m.zero();
        pdd k = m.zero();
        pdd X = s.var(x);
        rational k_val;
        if (!is_AxB_eq_0(x, axb_l_y, a, b, y))
            return false;
        if (a.is_val())
            return false;        
        if (!is_forced_eq(b, 0))
            return false;

        signed_constraint x_eq_0, a_eq_0;
        if (!is_forced_diseq(X, 0, x_eq_0))
            return false;
        if (!is_forced_diseq(a, 0, a_eq_0))
            return false;

        auto prop1 = [&](signed_constraint c) {
            m_lemma.reset();
            m_lemma.insert_eval(~s.eq(b));
            m_lemma.insert_eval(~s.eq(y));
            m_lemma.insert_eval(x_eq_0);
            m_lemma.insert_eval(a_eq_0);
            return propagate(x, core, axb_l_y, c);
        };

        auto prop2 = [&](signed_constraint ante, signed_constraint c) {
            m_lemma.reset();
            m_lemma.insert_eval(~s.eq(b));
            m_lemma.insert_eval(~s.eq(y));
            m_lemma.insert_eval(x_eq_0);
            m_lemma.insert_eval(a_eq_0);
            m_lemma.insert_eval(~ante);
            return propagate(x, core, axb_l_y, c);
        };

        pdd minus_a = -a;
        pdd minus_X = -X;
        pdd Y = X;
        for (auto si : s.m_search) {
            if (!si.is_boolean())
                continue;
            if (si.is_resolved())
                continue;
            auto d = s.lit2cnstr(si.lit());
            if (!d->is_ule())
                continue;
            auto u_l_k = inequality::from_ule(d);
            if (u_l_k.rhs().power_of_2() != m.power_of_2())
                continue;
            // a <= k or x <= k
            k = u_l_k.rhs();
            if (!k.is_val())
                continue;
            k_val = k.val();
            if (u_l_k.is_strict())
                k_val -= 1;
            if (k_val <= 1)
                continue;
            if (u_l_k.lhs() == a || u_l_k.lhs() == minus_a) 
                Y = X;
            else if (u_l_k.lhs() == X || u_l_k.lhs() == minus_X) 
                Y = a;
            else
                continue;
            //
            // NSB review: should we handle cases where k_val >= 2^{K-1}, but exploit that x*y = 0 iff -x*y = 0?
            // 
            // IF_VERBOSE(0, verbose_stream() << "mult-bounds2 " << Y << " " << axb_l_y << " " << u_l_k<< " \n");
            rational bound = ceil(rational::power_of_two(m.power_of_2()) / k_val);
            if (prop2(d, s.uge(Y, bound)))
                return true;
            if (prop2(d, s.uge(-Y, bound)))
                return true;                     
        }

        // IF_VERBOSE(0, verbose_stream() << "mult-bounds1 " << a << " " << axb_l_y << " \n");
        // IF_VERBOSE(0, verbose_stream() << core << "\n"); 
        if (prop1(s.umul_ovfl(a, X)))
            return true;
        if (prop1(s.umul_ovfl(a, -X)))
            return true;
        if (prop1(s.umul_ovfl(-a, X)))
            return true;
        if (prop1(s.umul_ovfl(-a, -X)))
            return true;

        return false;
    }


    // bench 5
    // fairly ad-hoc rule derived from bench 5.
    // The clause could also be added whenever narrowing the literal 2^k*x = 2^k*y
    // It can be expected to be relatively common because these equalities come from
    // bit-masking.
    // 
    // A bigger hammer for detecting such propagations may be through LIA or a variant
    // 
    // a*x - a*y + b*z = 0 0 <= x < b/a, 0 <= y < b/a => z = 0
    // and then => x = y
    // 
    // a general lemma is that the linear term a*p = 0 is such that a*p does not overflow
    // and therefore p = 0
    //
    // the rule would also be subsumed by equality rewriting modulo parity
    // 
    // TBD: encode the general lemma instead of this special case.
    // 
    bool saturation::try_mul_eq_bound(pvar x, conflict& core, inequality const& axb_l_y) {
        set_rule("[x] 2^k*x = 2^k*y & x < 2^N-k => y = x or y >= 2^{N-k}");
        auto& m = s.var2pdd(x);
        unsigned const N = m.power_of_2();
        pdd y = m.zero();
        pdd a = y, b = y, a2 = y;
        pdd const X = s.var(x);
        // Match ax + b <= y with eval(y) = 0
        if (!is_AxB_eq_0(x, axb_l_y, a, b, y))
            return false;
        if (!a.is_val())
            return false;
        if (!a.val().is_power_of_two())
            return false;
        unsigned const k = a.val().trailing_zeros();
        if (k == 0)
            return false;
        // x*2^k = b, x <= a2 < 2^{N-k}
        rational const bound = rational::power_of_two(N - k);
        b = -b;
        if (b.leading_coefficient() != a.val())
            return false;
        pdd Y = m.zero();
        if (!b.try_div(b.leading_coefficient(), Y))
            return false;
        rational Y_val;
        if (!s.try_eval(Y, Y_val) || Y_val >= bound)
            return false;
        for (auto c : core) {
            if (!c->is_ule())
                continue;
            auto i = inequality::from_ule(c);
            if (!is_x_l_Y(x, i, a2))
                continue;
            if (!a2.is_val())
                continue;
            if (i.is_strict() && a2.val() >= bound)
                continue;
            if (!i.is_strict() && a2.val() > bound)
                continue;
            signed_constraint le = s.ule(Y, bound - 1);
            m_lemma.reset();
            m_lemma.insert_eval(~le);
            m_lemma.insert_eval(~s.eq(y));
            m_lemma.insert(~c);
            if (propagate(x, core, axb_l_y, s.eq(X, Y)))
                return true;
        }
        return false;
    }

    /*
     * x*y = 1 & ~ovfl(x,y) => x = 1 
     * x*y = -1 & ~ovfl(-x,y) => -x = 1 
     */
    bool saturation::try_mul_eq_1(pvar x, conflict& core, inequality const& axb_l_y) {
        set_rule("[x] ax + b <= y & y = 0 & b = -1 & ~ovfl(a,x) => x = 1");
        auto& m = s.var2pdd(x);
        pdd y = m.zero();
        pdd a = m.zero();
        pdd b = m.zero();
        pdd X = s.var(x);
        signed_constraint non_ovfl;
        if (!is_AxB_eq_0(x, axb_l_y, a, b, y))
            return false;
        if (!is_forced_eq(b, -1))
            return false;
        if (!is_non_overflow(a, X, non_ovfl)) 
            return false;
        m_lemma.reset();
        m_lemma.insert_eval(~s.eq(b, rational(-1)));
        m_lemma.insert_eval(~s.eq(y));
        m_lemma.insert_eval(~non_ovfl);
        if (propagate(x, core, axb_l_y, s.eq(X, 1)))
            return true;
        if (propagate(x, core, axb_l_y, s.eq(a, 1)))
            return true;
        return false;
    }

    /**
     * odd(x*y) => odd(x) 
     * even(x) => even(x*y)
     *
     * parity(x) <= parity(x*y)
     * parity(x) = k & parity(x*y) = k + j => parity(y) = j
     * parity(x) = k & parity(y) = j => parity(x*y) = k + j
     *
     * odd(x) & even(y) => x + y != 0
     *
     * Special case rule: a*x + y = 0 => (odd(y) <=> odd(a) & odd(x))
     *
     * General rule:
     * 
     * a*x + y = 0 => min(K, parity(a) + parity(x)) = parity(y)
     * 
     * using inequalities:
     *
     * parity(x) <= i, parity(a) <= j => parity(y) <= i + j
     * parity(x) >= i, parity(a) >= j => parity(y) >= i + j
     * parity(x) <= i, parity(y) >= j => parity(a) >= j - i
     * parity(x) >= i, parity(y) <= j => parity(a) <= j - i 
     * symmetric rules for swapping x, a
     *
     * min_parity(x) = N if x = 0
     * min_parity(x) = number of trailing bits of x if x is a non-zero value
     * min_parity(x) = k if 2^{N-k}*x == 0 is forced for max k
     * min_parity(x1*x2) = min_parity(x1) + min_parity(x2)
     * min_parity(x) = 0, otherwise
     *
     * max_parity(x) = N if x = 0
     * max_parity(x) = number of trailing bits of x if x is a non-zero value
     * max_parity(x) = k if 2^{N-k-1}*x != 0 for min k
     * max_parity(x1*x2) = max_parity(x1) + max_parity(x2)
     * max_parity(x) = N, otherwise
     *
     */
    unsigned saturation::min_parity(pdd const& p, vector<signed_constraint>& explain) {
        auto& m = p.manager();
        unsigned const N = m.power_of_2();
        if (p.is_val())
            return p.val().parity(N);

        rational val;
        if (s.try_eval(p, val)) {
            unsigned k = val.parity(N);
            if (k > 0)
                explain.push_back(s.parity_at_least(p, k));
            return k;
        }
        
        unsigned min = 0;
        unsigned const sz = explain.size();
        if (!p.is_var()) {
            // parity of a product => sum of parities
            // parity of sum => minimum of monomial's minimal parities
            min = N;
            for (auto const& monomial : p) {
                SASSERT(!monomial.coeff.is_zero());
                unsigned parity_sum = monomial.coeff.trailing_zeros();
                for (pvar v : monomial.vars)
                    parity_sum += min_parity(m.mk_var(v), explain);
                min = std::min(min, parity_sum);
            }
        }
        SASSERT(min <= N);

        for (unsigned j = N; j > min; --j)
            if (is_forced_true(s.parity_at_least(p, j))) {
                explain.shrink(sz);
                explain.push_back(s.parity_at_least(p, j));
                return j;
            }
        return min;
    }

    unsigned saturation::max_parity(pdd const& p, vector<signed_constraint>& explain) {
        auto& m = p.manager();
        unsigned N = m.power_of_2();
        rational val;
        if (p.is_val())
            return p.val().parity(N);
        
        if (s.try_eval(p, val)) {
            unsigned k = val.parity(N);
            if (k != N)
                explain.push_back(s.parity_at_most(p, k));
            return k;
        }

        unsigned max = N;
        unsigned sz = explain.size();
        if (!p.is_var() && p.is_monomial()) {
            // it's just a product => sum them up
            // the case of a sum is harder as the lower bound (because of carry bits)
            // ==> restricted for now to monomials
            dd::pdd_monomial monomial = *p.begin();
            max = monomial.coeff.trailing_zeros();
            for (pvar c : monomial.vars)
                max += max_parity(m.mk_var(c), explain);
        }

        for (unsigned j = 0; j < max; ++j) 
            if (is_forced_true(s.parity_at_most(p, j))) {
                explain.shrink(sz);
                explain.push_back(s.parity_at_most(p, j));
                return j;
            }
        return max;
    }

    bool saturation::try_parity(pvar x, conflict& core, inequality const& axb_l_y) {
        auto& m = s.var2pdd(x);
        unsigned N = m.power_of_2();
        pdd y = m.zero();
        pdd a = y, b = y;
        pdd X = s.var(x);
        if (!is_AxB_eq_0(x, axb_l_y, a, b, y))
            return false;
        if (a.is_max() && b.is_var())  // x == y, we propagate values in each direction and don't need a lemma
            return false;
        if (a.is_one() && (-b).is_var())  // y == x
            return false;
        if (a.is_one()) // TODO: Sure this is correct?
            return false;
        if (a.is_val() && b.is_zero())
            return false;

        auto propagate1 = [&](vector<signed_constraint> const& premise, signed_constraint conseq) {
            IF_VERBOSE(1, verbose_stream() << "propagate " << axb_l_y << " " << premise << " => " << conseq << "\n");
            m_lemma.reset();
            m_lemma.insert_eval(~s.eq(y));
            for (auto const& c : premise) {
                if (is_forced_false(c))
                    return false;
                m_lemma.insert_eval(~c);
            }
            return propagate(x, core, axb_l_y, conseq);
        };

        auto propagate2 = [&](vector<signed_constraint> const& premise1, vector<signed_constraint> const& premise2, signed_constraint conseq) {
            IF_VERBOSE(1, verbose_stream() << "propagate " << axb_l_y << " " << premise1 << " " << premise2 << " => " << conseq << "\n");
            m_lemma.reset();
            m_lemma.insert_eval(~s.eq(y));
            for (auto const& c : premise1) {
                if (is_forced_false(c))
                    return false;
                m_lemma.insert_eval(~c);
            }
            for (auto const& c : premise2) {
                if (is_forced_false(c))
                    return false;
                m_lemma.insert_eval(~c);
            }
            return propagate(x, core, axb_l_y, conseq);
        };

        auto correct_parity = [&](vector<signed_constraint> const& at_least, vector<signed_constraint> const& at_most) {
            m_lemma.reset();
            m_lemma.insert_eval(~s.eq(y));
            for (auto const& c : at_least) {
                if (is_forced_false(c))
                    return false;
                m_lemma.insert_eval(~c);
            }
            for (auto const& c : at_most) {
                if (is_forced_false(c))
                    return false;
                m_lemma.insert_eval(~c);
            }
            return propagate(x, core, axb_l_y, s.f()); // TODO: Conflict overload
        };

        vector<signed_constraint> at_least_x, at_most_x, at_least_b, at_most_b, at_least_a, at_most_a;

        set_rule("[x] min_parity(t[x], j1) > max_parity(t[x], j2) => (!j1 || !j2)");

        bool failed = false;
        unsigned min_x = min_parity(X, at_least_x), max_x = max_parity(X, at_most_x);
        unsigned min_b = min_parity(b, at_least_b), max_b = max_parity(b, at_most_b);
        unsigned min_a = min_parity(a, at_least_a), max_a = max_parity(a, at_most_a);

        // correct min_parity(x) > max_parity(x)
        if (min_x > max_x) {
            failed = true;
            correct_parity(at_least_x, at_most_x);
        }
        if (min_b > max_b) {
            failed = true;
            correct_parity(at_least_b, at_most_b);
        }
        if (min_a > max_a) {
            failed = true;
            correct_parity(at_least_a, at_most_a);
        }

        if (failed)
            // we propagated at least one parity correction lemma but there is no reason to proceed
            return true;

        SASSERT(max_x <= N);
        SASSERT(max_b <= N);
        SASSERT(max_a <= N);

        IF_VERBOSE(2, 
                   verbose_stream() << "try parity v" << x << " " << axb_l_y << "\n";
                   verbose_stream() << "x " << X << " " << min_x << " " << max_x << "\n";
                   verbose_stream() << "a " << a << " " << min_a << " " << max_a << "\n";
                   verbose_stream() << "b " << b << " " << min_b << " " << max_b << "\n");                    

        if (min_x >= N || min_a >= N)
            return false;

        auto at_most = [&](pdd const& p, unsigned k) {
            VERIFY(k < N);
            return s.parity_at_most(p, k);
        };

        auto at_least = [&](pdd const& p, unsigned k) {
            VERIFY(k != 0);
            VERIFY(k <= N);
            return s.parity_at_least(p, k);
        };
 
        set_rule("[x] a*x + b = 0 => (odd(a) & odd(x) <=> odd(b)) 1");
        if (!b.is_val() && max_b > max_a + max_x && propagate2(at_most_a, at_most_x, at_most(b, max_x + max_a)))
            return true;
        set_rule("[x] a*x + b = 0 => (odd(a) & odd(x) <=> odd(b)) 2");
        if (!b.is_val() && min_x > min_b && propagate1(at_least_x, at_least(b, min_x)))
            return true;
        set_rule("[x] a*x + b = 0 => (odd(a) & odd(x) <=> odd(b)) 3");
        if (!b.is_val() && min_a > min_b && propagate1(at_least_a, at_least(b, min_a)))
            return true;
        set_rule("[x] a*x + b = 0 => parity(b) >= parity(a) + parity(x)");
        if (!b.is_val() && min_x > 0 && min_a > 0 && min_x + min_a > min_b && N > min_b && propagate2(at_least_a, at_least_x, at_least(b, std::min(N, min_a + min_x))))
            return true;
        set_rule("[x] a*x + b = 0 => (odd(a) & odd(x) <=> odd(b)) 5");
        if (!a.is_val() && max_x <= min_b && min_a < min_b - max_x && propagate2(at_most_x, at_least_b, at_least(a, min_b - max_x)))
            return true;
        set_rule("[x] a*x + b = 0 => (odd(a) & odd(x) <=> odd(b)) 6");
        if (max_a <= min_b && min_x < min_b - max_a && propagate2(at_most_a, at_least_b, at_least(X, min_b - max_a)))
            return true;
        set_rule("[x] a*x + b = 0 => (odd(a) & odd(x) <=> odd(b)) 7");
        if (max_b < N && !a.is_val() && min_x > 0 && min_x <= max_b && max_a > max_b - min_x && propagate2(at_least_x, at_most_b, at_most(a, max_b - min_x)))
            return true;
        set_rule("[x] a*x + b = 0 => (odd(a) & odd(x) <=> odd(b)) 8");
        if (max_b < N && min_a > 0 && min_a <= max_b && max_x > max_b - min_a && propagate2(at_least_a, at_most_b, at_most(X, max_b - min_a)))
            return true;

        return false;        
    }

    /**
     *  2^{N-1}*x*y != 0 => odd(x) & odd(y)
     *  2^k*x != 0 => parity(x) < N - k
     *  2^k*x*y != 0 => parity(x) + parity(y) < N - k
     * 
     *  2^k*x + b != 0 & parity(x) >= N - k => b != 0 & 2^k*x = 0  (rewriting constraints modulo parity is more powerful and subsumes this)
     */
    bool saturation::try_parity_diseq(pvar x, conflict& core, inequality const& axb_l_y) {
        set_rule("[x] p(x,y) != 0 => constraints on parity(x), parity(y)");
        auto& m = s.var2pdd(x);
        unsigned N = m.power_of_2();
        pdd y = m.zero();
        pdd a = y, b = y;
        pdd X = s.var(x);
        if (!is_AxB_diseq_0(x, axb_l_y, a, b, y))
            return false;
        if (is_forced_eq(b, 0)) {
            auto coeff = a.leading_coefficient();
            if (coeff.is_odd())
                return false;
            SASSERT(coeff != 0);
            unsigned k = coeff.trailing_zeros();
            m_lemma.reset();
            m_lemma.insert_eval(~s.eq(y));
            m_lemma.insert_eval(~s.eq(b));
            if (propagate(x, core, axb_l_y, ~s.parity_at_least(X, N - k)))
                return true;
            // TODO parity on a (without leading coefficient?)
        }
        if (a.is_val()) {
            auto coeff = a.val();
            unsigned k = coeff.trailing_zeros();
            vector<signed_constraint> at_least_x;
            unsigned p_x = min_parity(X, at_least_x);
            if (k + p_x >= N) {
                // ax + b != 0
                m_lemma.reset();
                m_lemma.insert_eval(~s.eq(y));
                for (auto c : at_least_x)
                    m_lemma.insert_eval(~c);
                if (propagate(x, core, axb_l_y, ~s.eq(b))) 
                    return true;
            }
        }

        return false;
    }

    /**
     * a*x = 0 => a = 0 or even(x)
     * a*x = 0 => a = 0 or x = 0 or even(a)
     */
    bool saturation::try_mul_odd(pvar x, conflict& core, inequality const& axb_l_y) {
        set_rule("[x] ax = 0 => a = 0 or even(x)");
        auto& m = s.var2pdd(x);
        pdd y = m.zero();
        pdd a = m.zero();
        pdd b = m.zero();
        pdd X = s.var(x);
        signed_constraint a_eq_0, x_eq_0;
        if (!is_AxB_eq_0(x, axb_l_y, a, b, y))
            return false;
        if (!is_forced_eq(b, 0))
            return false;
        if (!is_forced_diseq(a, 0, a_eq_0))
            return false;
        m_lemma.reset();
        m_lemma.insert_eval(s.eq(y));
        m_lemma.insert_eval(~s.eq(b));
        m_lemma.insert_eval(a_eq_0);
        if (propagate(x, core, axb_l_y, s.even(X)))
            return true;
        if (!is_forced_diseq(X, 0, x_eq_0))
            return false;
        m_lemma.insert_eval(x_eq_0);
        if (propagate(x, core, axb_l_y, s.even(a)))
            return true;
        return false;
    }

    /**
     * TODO If both inequalities are strict, then the implied inequality has a gap of 2
     * a < b, b < c => a + 1 < c & a + 1 != 0
     */
    bool saturation::try_transitivity(pvar x, conflict& core, inequality const& a_l_b) {
        set_rule("[x] q < x & x <= p => q < p");
        auto& m = s.var2pdd(x);
        pdd p = m.zero();
        pdd a = p, b = p, q = p;
        // x <= p
        if (!is_Ax_l_Y(x, a_l_b, a, p))
            return false;
        if (!is_forced_eq(a, 1))
            return false;
        for (auto c : core) {
            if (!c->is_ule())
                continue;
            if (c->to_ule().power_of_2() != m.power_of_2())
                continue;
            auto i = inequality::from_ule(c);
            if (c == a_l_b.as_signed_constraint())
                continue;
            if (!is_Y_l_Ax(x, i, b, q))
                continue;
            if (!is_forced_eq(b, 1))
                continue;
            m_lemma.reset();
            m_lemma.insert_eval(~s.eq(a, 1));
            m_lemma.insert_eval(~s.eq(b, 1));
            m_lemma.insert(~c);
            auto ineq = i.is_strict() || a_l_b.is_strict() ? (p.is_val() ? s.ule(q, p - 1) : s.ult(q, p)) : s.ule(q, p);
            if (propagate(x, core, a_l_b, ineq))
                return true;
        }

        return false;
    }



    lbool saturation::get_multiple(const pdd& p1, const pdd& p2, pdd& out) {
        LOG("Check if " << p2 << " can be multiplied with something to get " << p1);
        if (p1.is_zero()) { // TODO: use the evaluated parity (max_parity) instead?
            out = p1.manager().zero();
            return l_true;
        }
        if (p2.is_one()) {
            out = p1;
            return l_true;
        }
        if (!p1.is_monomial() || !p2.is_monomial())
            // TODO: Actually, this could work as well. (4a*d + 6b*c*d) is a multiple of (2a + 3b*c) although none of them is a monomial
            return l_undef;

        vector<signed_constraint> maxp1, minp2;
        unsigned max_parity_p1 = max_parity(p1, maxp1);
        unsigned min_parity_p2 = min_parity(p2, minp2);
        
        if (min_parity_p2 > max_parity_p1) 
            return l_false;
        
        dd::pdd_monomial p1m = *p1.begin();
        dd::pdd_monomial p2m = *p2.begin();
        
        m_occ_cnt.reserve(s.m_vars.size(), (unsigned)0); // TODO: Are there duplicates in the list (e.g., v1 * v1)?)
        
        for (const auto& v1 : p1m.vars) {
            if (m_occ_cnt[v1] == 0)
                m_occ.push_back(v1);
            m_occ_cnt[v1]++;
        }
        for (const auto& v2 : p2m.vars) {
            if (m_occ_cnt[v2] == 0) {
                for (const auto& occ : m_occ)
                    m_occ_cnt[occ] = 0;
                m_occ.clear();
                return l_undef; // p2 contains more v2 than p1; we need more information (assignments)
            }
            m_occ_cnt[v2]--;
        }
        
        unsigned tz1 = p1m.coeff.trailing_zeros();
        unsigned tz2 = p2m.coeff.trailing_zeros();
        if (tz2 > tz1) 
            return l_undef;
        
        rational odd = div(p2m.coeff, rational::power_of_two(tz2));
        rational inv;
        VERIFY(odd.mult_inverse(p1.power_of_2() - tz2, inv)); // we divided by the even part, so it has to be odd/invertible now
        inv *= div(p1m.coeff, rational::power_of_two(tz2));
        
        out = p1.manager().mk_val(inv);
        for (const auto& occ : m_occ) {
            for (unsigned i = 0; i < m_occ_cnt[occ]; i++)
                out *= s.var(occ);
            m_occ_cnt[occ] = 0;
        }
        m_occ.clear();
        LOG("Found multiple: " << out);
        return l_true;
    }

    bool saturation::try_factor_equality(pvar x, conflict& core, inequality const& a_l_b) {
        set_rule("[x] ax + b = 0 & C[x] => C[-inv(a)*b]");
        auto& m = s.var2pdd(x);       
        pdd y  = m.zero();
        pdd a = y, b = y, a1 = y, b1 = y, mul_fac = y;        
        if (!is_AxB_eq_0(x, a_l_b, a, b, y)) // TODO: Is the restriction to linear "x" too restrictive?
            return false;
        
        bool prop = false;
        
        for (auto c : core) {
            if (c == a_l_b)
                continue;
            LOG("Trying to eliminate v" << x << " in " << c << " by using equation " << a_l_b.as_signed_constraint());
            if (c->is_ule()) {
                set_rule("[x] ax + b = 0 & C[x] => C[-inv(a)*b] ule");
                // If both are equalities this boils down to polynomial superposition => Might generate the same lemma twice
                auto const& ule = c->to_ule();
                m_lemma.reset();
                auto [lhs_new, changed_lhs] = m_parity_tracker.eliminate_variable(*this, x, a, b, ule.lhs(), m_lemma);
                auto [rhs_new, changed_rhs] = m_parity_tracker.eliminate_variable(*this, x, a, b, ule.rhs(), m_lemma);
                if (!changed_lhs && !changed_rhs)
                    continue; // nothing changed - no reason for propagating lemmas
                m_lemma.insert(~c);
                m_lemma.insert_eval(~s.eq(y));
                
                if (propagate(x, core, a_l_b, c.is_positive() ? s.ule(lhs_new, rhs_new) : ~s.ule(lhs_new, rhs_new)))
                    prop = true;
            }
            else if (c->is_umul_ovfl()) {
                set_rule("[x] ax + b = 0 & C[x] => C[-inv(a)*b] umul_ovfl");
                auto const& ovf = c->to_umul_ovfl();
                m_lemma.reset();
                auto [lhs_new, changed_lhs] = m_parity_tracker.eliminate_variable(*this, x, a, b, ovf.p(), m_lemma);
                auto [rhs_new, changed_rhs] = m_parity_tracker.eliminate_variable(*this, x, a, b, ovf.q(), m_lemma);
                if (!changed_lhs && !changed_rhs)
                    continue;
                m_lemma.insert(~c);
                m_lemma.insert_eval(~s.eq(y));
                
                if (propagate(x, core, a_l_b, c.is_positive() ? s.umul_ovfl(lhs_new, rhs_new) : ~s.umul_ovfl(lhs_new, rhs_new)))
                    prop = true;
            }
        }
        return prop;
    }


    /**
     *  x >=  x + y  &  x <= n  ==>  y >= M - n or y = 0
     *  x >   x + y  &  x <= n  ==>  y >= M - n
     * -x <= -x - y  &  x <= n  ==>  y >= M - n or y = 0 or x = 0
     * -x <  -x - y  &  x <= n  ==>  y >= M - n          or x = 0
     *
     * NOTE:  x + y <= x   <=>  -y <= x     <=>   -x-1 <= y-1
     *        x <= x + y   <=>  x <= -y-1   <=>   y <= -x-1
     *        (see notes on equivalent forms in ule_constraint.cpp)
     *
     *        p <= q  ==>  p = 0  or  -q <= -p
     */
    bool saturation::try_add_overflow_bound(pvar x, conflict& core, inequality const& i) {
        set_rule("[x] x >= x + y & x <= n => y = 0 or y >= 2^N - n");
        signed_constraint y_eq_0, x_eq_0;
        vector<signed_constraint> x_le_bound;
        auto& m = s.var2pdd(x);
        pdd y = m.zero();
        bool is_minus;
        if (!is_add_overflow(x, i, y, is_minus))
            return false;
        if (!i.is_strict() && !is_forced_diseq(y, 0, y_eq_0))
            return false;
        if (is_minus && !is_forced_diseq(s.var(x), 0, x_eq_0))
            return false;
        rational bound;
        if (!has_upper_bound(x, core, bound, x_le_bound))
            return false;
        SASSERT(bound != 0);
        m_lemma.reset();
        if (!i.is_strict())
            m_lemma.insert_eval(y_eq_0);
        if (is_minus)
            m_lemma.insert_eval(x_eq_0);
        for (auto c : x_le_bound)
            m_lemma.insert_eval(~c);
        return propagate(x, core, i, s.uge(y, m.two_to_N() - bound));
    }

    /**
     * Match one of the patterns:
     *  x >=  x + y
     *  x >   x + y
     * -x <= -x - y
     * -x <  -x - y
     */
    bool saturation::is_add_overflow(pvar x, inequality const& i, pdd& y, bool& is_minus) {
        pdd const X = s.var(x);
        pdd a = X;
        if (i.lhs().degree(x) != 1 || i.rhs().degree(x) != 1)
            return false;
        if (i.rhs() == X) {
            i.lhs().factor(x, 1, a, y);
            if (a.is_one()) {
                is_minus = false;
                return true;
            }
        }
        if (i.lhs() == -X) {
            i.rhs().factor(x, 1, a, y);
            if ((-a).is_one()) {
                is_minus = true;
                y = -y;
                return true;
            }
        }
        return false;
    }

    bool saturation::has_upper_bound(pvar x, conflict& core, rational& bound, vector<signed_constraint>& x_le_bound) {
        return s.m_viable.has_upper_bound(x, bound, x_le_bound);
    }

    bool saturation::has_lower_bound(pvar x, conflict& core, rational& bound, vector<signed_constraint>& x_ge_bound) {
        return s.m_viable.has_lower_bound(x, bound, x_ge_bound);
    }

    rational saturation::round(rational const& M, rational const& x) {
        SASSERT(0 <= x && x < M);
        if (x + M/2 > M)
            return x - M;
        else
            return x;
    }

    bool saturation::eval_round(rational const& M, pdd const& p, rational& r) {
        if (!s.try_eval(p, r))
            return false;
        r = round(M, r);
        return true;
    }

    /**
     * Write as q := a*y + b
     *
     * If y == null_var, chooses some variable y from q (if one exists).
     */
    bool saturation::extract_linear_form(pdd const& q, pvar& y, rational& a, rational& b) {
        auto& m = q.manager();
        rational const& M = m.two_to_N();
        
        if (q.is_val()) {
            a = 0;
            b = round(M, q.val());
            return true;
        }
        if (y == null_var) {
            // choose the top variable
            y = q.var();
            if (!q.hi().is_val() && q.hi().var() == y)
                return false;
            if (!eval_round(M, q.hi(), a))
                return false;
            if (!eval_round(M, q.lo(), b))
                return false;
            return true;
        }
        else {
            // factor according to given variable
            SASSERT(y != null_var);
            switch (q.degree(y)) {
            case 0:
                if (!eval_round(M, q, b))
                    return false;
                a = 0;
                return true;
            case 1: {
                pdd a1(m), b1(m);
                q.factor(y, 1, a1, b1);
                if (!eval_round(M, a1, a))
                    return false;
                if (!eval_round(M, b1, b))
                    return false;
                return true;
            }
            default:
                return false;
            }
        }
    }
    
    /**
     * Write as p := a*x*y + b*x + c*y + d
     * 
     * If y == null_var, chooses some variable y != x from p (if one exists).
     */
    bool saturation::extract_bilinear_form(pvar x, pdd const& p, pvar& y, bilinear& b) {
        auto& m = s.var2pdd(x);
        rational const& M = m.two_to_N();
        switch (p.degree(x)) {
        case 0:
            if (!s.try_eval(p, b.d))
                return false;
            b.a = b.b = b.c = 0;
            return true;
        case 1: {
            pdd q = p, r = p, u = p, v = p;
            p.factor(x, 1, q, r);
            if (!extract_linear_form(q, y, b.a, b.b))
                return false;
            if (b.a == 0) {
                b.c = 0;
                return eval_round(M, r, b.d);
            }
            SASSERT(y != null_var);
            switch (r.degree(y)) {
            case 0:
                if (!eval_round(M, r, b.d))
                    return false;
                b.c = 0;
                return true;
            case 1:
                r.factor(y, 1, u, v);
                if (!eval_round(M, u, b.c))
                    return false;
                if (!eval_round(M, v, b.d))
                    return false;
                return true;
            default:
                return false;
            }
            return false;
        }
        default:
            return false;
        }        
    }

    /**
     * Update d such that -M < a*x*y0 + b*x + c*y0 + d < M for every value x_min <= x <= x_max, return x_split such that [x_min,x_split[ and [x_split,x_max] can fit into [0,M[
     * return false if there is no such d.
     */
    bool saturation::adjust_bound(rational const& x_min, rational const& x_max, rational const& y0,
                                  rational const& M, bilinear& b, rational& x) {
        SASSERT(x_min <= x_max);
        rational A = b.a*y0 + b.b;
        rational B = b.c*y0 + b.d;
        rational max = A >= 0 ? x_max * A + B : x_min * A + B;
        rational min = A >= 0 ? x_min * A + B : x_max * A + B;
        VERIFY(min <= max);
        if (max - min >= M) {
            IF_VERBOSE(10, verbose_stream() << "adjust_bound: abort because max - min >= M\n");
            return false;
        }

        // k0 = min k. val + kM >= 0
        //    = min k. k >= -val/M
        //    = ceil(-val/M) = -floor(val/M)
        rational offset = rational::zero();
        if (max < 0 || max >= M)
            offset = -M * floor(max / M);
        b.d += offset;

        // If min + offset < 0, then [min,max] contains a multiple of M.
        if (min + offset < 0) {
            // A*x_split + B + offset = 0
            // x_split = -(B+offset)/A
            if (A >= 0) {
                x = ceil((-offset - B) / A);
                // [x_min; x_split-1] maps to interval < 0
                // [x_split; x_max] maps to interval >= 0
                VERIFY(b.eval(x, y0) >= 0);
                VERIFY(b.eval(x-1, y0) < 0);
                VERIFY(x_min <= x && x <= x_max);
            }
            else {
                x = floor((-offset - B) / A) + 1;
                // [x_min; x_split-1] maps to interval >= 0
                // [x_split; x_max] maps to interval < 0
                VERIFY(b.eval(x, y0) < 0);
                VERIFY(b.eval(x-1, y0) >= 0);
                VERIFY(x_min <= x && x <= x_max);
            }
        }

        VERIFY(-M < b.eval(x_min, y0));
        VERIFY(b.eval(x_min, y0) < M);
        VERIFY(-M < b.eval(x_max, y0));
        VERIFY(b.eval(x_max, y0) < M);
        return true;
    }
    
    /**
     * Based on a*x*y + b*x + c*y + d >= 0
     * update lower bound for y
     */
    bool saturation::update_min(rational& y_min, rational const& x_min, rational const& x_max,
                                bilinear const& b) {
        if (b.a == 0 && b.c == 0)
            return true;

        rational x_bound;
        if (b.a >= 0 && b.b >= 0) 
            x_bound = x_min;
        else if (b.a <= 0 && b.b <= 0)
            x_bound = x_max;
        else
            return false;
        
        // a*x_bound*y + b*x_bound + c*y + d >= 0
        // (a*x_bound + c)*y >= -d - b*x_bound
        // if a*x_bound + c > 0
        rational A = b.a*x_bound + b.c;
        if (A <= 0)
            return true;
        rational y1 = ceil((- b.d - b.b*x_bound)/A);
        if (y1 > y_min)
            y_min = y1;
        return true;
    }

    bool saturation::update_max(rational& y_max, rational const& x_min, rational const& x_max,
                                bilinear const& b) {
        if (b.a == 0 && b.c == 0)
            return true;

        rational x_bound;
        if (b.a >= 0 && b.b >= 0) 
            x_bound = x_min;
        else if (b.a <= 0 && b.b <= 0)
            x_bound = x_max;
        else
            return false;
        
        // a*x_bound*y + b*x_bound + c*y + d >= 0
        // (a*x_bound + c)*y >= -d - b*x_bound
        // if a*x_bound + c < 0
        rational A = b.a*x_bound + b.c;
        if (A >= 0)
            return true;
        rational y1 = floor((- b.d - b.b*x_bound)/A);
        if (y1 < y_max)
            y_max = y1;
        return true;
    }

    void saturation::fix_values(pvar y, pdd const& p) {
        if (p.degree(y) == 0) {
            rational p_val;
            VERIFY(s.try_eval(p, p_val));
            m_lemma.insert_eval(~s.eq(p, p_val));
        }
        else {
            pdd q = p, r = p;
            p.factor(y, 1, q, r);
            fix_values(y, q);
            fix_values(y, r);
        }
    }

    void saturation::fix_values(pvar x, pvar y, pdd const& p) {
        if (p.degree(x) == 0) 
            fix_values(y, p);
        else {
            pdd q = p, r = p;
            p.factor(x, 1, q, r);
            fix_values(x, y, q);
            fix_values(x, y, r);
        }
    }

    bool saturation::update_bounds_for_xs(rational const& x_min, rational const& x_max, rational& y_min, rational& y_max, rational const& y0, bilinear b1, bilinear b2, rational const& M, inequality const& a_l_b) {

        VERIFY(x_min <= x_max);

        if (b1.eval(x_min, y0) < 0)
            b1.d += M;
        if (b2.eval(x_min, y0) < 0)
            b2.d += M;

        IF_VERBOSE(2,
                   verbose_stream() << "Adjusted for x in [" << x_min << "; " << x_max << "]\n";
                   verbose_stream() << "p ... " << b1 << " " << b1.eval(x_min, y0) << "\n";
                   verbose_stream() << "q ... " << b2 << " " << b2.eval(x_min, y0) << "\n";
                   );

        // Precondition: forall x . x_min <= x <= x_max ==> p(x,y0) > q(x,y0)
        // check the endpoints
        //
        // the pre-condition could be false if the interval x_min..x_max 
        // is not defined by a_l_b, but different constraints.
        //
        if (b1.eval(x_min, y0) < b2.eval(x_min, y0) + (a_l_b.is_strict() ? 0 : 1))
            return false;
        if (b1.eval(x_max, y0) < b2.eval(x_max, y0) + (a_l_b.is_strict() ? 0 : 1))
            return false;
        
        if (!update_min(y_min, x_min, x_max, b1))
            return false;
        if (!update_min(y_min, x_min, x_max, b2))
            return false;
        //verbose_stream() << "min-max: x := v" << x << " [" << x_min << "," << x_max << "] y := v" << y << " [" << y_min << ", " << y_max << "] y0 " << y0 << "\n";
        VERIFY(y_min <= y0 && y0 <= y_max);
        if (!update_max(y_max, x_min, x_max, b1))
            return false;
        if (!update_max(y_max, x_min, x_max, b2))
            return false;
        //verbose_stream() << "min-max: x := v" << x << " [" << x_min << "," << x_max << "] y := v" << y << " [" << y_min << ", " << y_max << "] y0 " << y0 << "\n";
        VERIFY(y_min <= y0 && y0 <= y_max);
        // p < M iff -p > -M iff -p + M - 1 >= 0
        if (!update_min(y_min, x_min, x_max, -b1 + (M - 1)))
            return false;
        if (!update_min(y_min, x_min, x_max, -b2 + (M - 1)))
            return false;
        if (!update_max(y_max, x_min, x_max, -b1 + (M - 1)))
            return false;
        if (!update_max(y_max, x_min, x_max, -b2 + (M - 1)))
            return false;
        VERIFY(y_min <= y0 && y0 <= y_max);
        // p <= q or p < q is false
        // so p > q or p >= q
        // p - q - 1 >= 0 or p - q >= 0
        // min-max for p - q - 1 or p - q are non-negative
        if (!update_min(y_min, x_min, x_max, b1 - b2 - (a_l_b.is_strict() ? 0 : 1)))
            return false;
        if (!update_max(y_max, x_min, x_max, b1 - b2 - (a_l_b.is_strict() ? 0 : 1)))
            return false;
        return true;
    }

    // wip - outline of what should be a more general approach
    bool saturation::try_add_mul_bound(pvar x, conflict& core, inequality const& a_l_b) {
        set_rule("[x] mul-bound2 ax + b <= y, ... => a >= u_a");

        // comment out for dev
        return false;

        auto& m = s.var2pdd(x);    
        pdd p = a_l_b.lhs(), q = a_l_b.rhs();
        // add this filter to remove useless bounds
        if (q.is_zero())
            return false;
        if (p.degree(x) > 1 || q.degree(x) > 1)
            return false;
        if (p.degree(x) == 0 && q.degree(x) == 0)
            return false;

        pvar y = null_var;
        bilinear b1, b2;
        if (!extract_bilinear_form(x, p, y, b1))
            return false;
        if (!extract_bilinear_form(x, q, y, b2))
            return false;
        if (y == null_var)
            return false;
        if (!s.is_assigned(y))
            return false;
        rational y0 = s.get_value(y);

        vector<signed_constraint> bounds;
        rational x_min, x_max;
        if (!s.m_viable.has_max_forbidden(x, a_l_b, x_max, x_min, bounds))
            return false;

        // retrieved maximal forbidden interval [x_max, x_min[.
        // [x_min, x_max[ is the allowed interval.
        // compute [x_min, x_max - 1]
        VERIFY(x_min != x_max);
        SASSERT(0 <= x_min && x_min <= m.max_value());
        SASSERT(0 <= x_max && x_max <= m.max_value());
        rational const& M = m.two_to_N();
        x_max = x_max == 0 ? m.max_value() : x_max - 1;
        if (x_min == x_max)
            return false;
        if (x_min > x_max)
            x_min -= M;
        // else if (m.max_value() - x_max < x_min) {
        //     TODO: deal with large x values like this?
        //     x_min -= M;
        //     x_max -= M;
        // }
        SASSERT(x_min <= x_max);

        IF_VERBOSE(2,
                   verbose_stream() << "\n---\n\n";
                   verbose_stream() << "constraint " << lit_pp(s, a_l_b) << "\n";
                   verbose_stream() << "x = v" << x << "\n";
                   verbose_stream() << "y = v" << y << "\n";
                   s.m_viable.display(verbose_stream() << "\nx-intervals:\n", x, "\n") << "\n";
                   verbose_stream() << "\n";
                   verbose_stream() << "x_min " << x_min << " x_max " << x_max << "\n";
                   verbose_stream() << "v" << y << " " << y0 << "\n";
                   verbose_stream() << p << " ... " << b1 << "\n";
                   verbose_stream() << q << " ... " << b2 << "\n");

        rational x_sp1 = x_min;
        rational x_sp2 = x_min;

        if (!adjust_bound(x_min, x_max, y0, M, b1, x_sp1))
            return false;
        if (!adjust_bound(x_min, x_max, y0, M, b2, x_sp2))
            return false;

        if (x_sp1 > x_sp2)
            std::swap(x_sp1, x_sp2);
        SASSERT(x_min <= x_sp1 && x_sp1 <= x_sp2 && x_sp2 <= x_max);

        IF_VERBOSE(2,
                   verbose_stream() << "Adjusted\n";
                   verbose_stream() << p << " ... " << b1 << "\n";
                   verbose_stream() << q << " ... " << b2 << "\n";
                   // verbose_stream() << "p(x_min,y0) = " << b1.eval(x_min, y0) << "\n";
                   // verbose_stream() << "q(x_min,y0) = " << b2.eval(x_min, y0) << "\n";
                   // verbose_stream() << "p(x_max,y0) = " << b1.eval(x_max, y0) << "\n";
                   // verbose_stream() << "q(x_max,y0) = " << b2.eval(x_max, y0) << "\n";
                   );

        rational y_min(0), y_max(M-1);
        if (x_min != x_sp1 && !update_bounds_for_xs(x_min, x_sp1-1, y_min, y_max, y0, b1, b2, M, a_l_b))
            return false;
        IF_VERBOSE(11, verbose_stream() << "min-max: x := v" << x << " [" << x_min << "," << x_max << "] y := v" << y << " [" << y_min << ", " << y_max << "] y0 " << y0 << "\n");
        if (x_sp1 != x_sp2 && !update_bounds_for_xs(x_sp1, x_sp2-1, y_min, y_max, y0, b1, b2, M, a_l_b))
            return false;
        IF_VERBOSE(11, verbose_stream() << "min-max: x := v" << x << " [" << x_min << "," << x_max << "] y := v" << y << " [" << y_min << ", " << y_max << "] y0 " << y0 << "\n");
        if (!update_bounds_for_xs(x_sp2, x_max,   y_min, y_max, y0, b1, b2, M, a_l_b))
            return false;
        IF_VERBOSE(11, verbose_stream() << "min-max: x := v" << x << " [" << x_min << "," << x_max << "] y := v" << y << " [" << y_min << ", " << y_max << "] y0 " << y0 << "\n");

        SASSERT(y_min <= y0 && y0 <= y_max);
        VERIFY(y_min <= y0 && y0 <= y_max);
        if (y_min == y_max)
            return false;

        m_lemma.reset();
        for (auto const& c : bounds)
            m_lemma.insert_eval(~c);
        fix_values(x, y, p);
        fix_values(x, y, q);
        if (y_max != M - 1) {
            if (y_min != 0)
                m_lemma.insert_eval(s.ult(s.var(y), y_min));
            return propagate(x, core, a_l_b, s.ult(y_max, s.var(y)));
        }
        if (y_min != 0)
            return propagate(x, core, a_l_b, s.ult(s.var(y), y_min));
        else 
            return false;
    }

    /**
     * p >= q & q*2^k = 0 & p < 2^{K-k} => q = 0
     * More generally
     * p >= q + r & q*2^k = 0 & p < 2^{K-k} & r < 2^{K-k} => q = 0 & p >= r
     * 
     * The parity constraint on q entails that the low K-k bits of q must be 0
     * and therefore q is either 0 or at or above 2^{K-k}.
     * Since p is blow 2^{K-k} the only intersection between the viable 
     * intervals imposed by p and possible for q is 0.
     * 
     */
    bool saturation::try_infer_parity_equality(pvar x, conflict& core, inequality const& a_l_b) {
        return false;
        set_rule("[x] p > q & 2^k*q = 0 & p < 2^{K-k} => q = 0");
        auto& m = s.var2pdd(x);
        auto p = a_l_b.rhs(), q = a_l_b.lhs();
        if (q.is_val())
            return false;
        if (p.is_val() && p.val() == 0)
            return false;
        rational p_val;
        if (!s.try_eval(p, p_val))
            return false;
        vector<signed_constraint> at_least_k;
        unsigned k = min_parity(q, at_least_k);
        unsigned N = m.power_of_2();
        if (k == N)
            return false;
        if (rational::power_of_two(k) > p_val) {
            // verbose_stream() << k << " " << p_val << " " << a_l_b << "\n";
            m_lemma.reset();
            for (auto const& c : at_least_k)
                m_lemma.insert_eval(~c);
            m_lemma.insert_eval(~s.ult(p, rational::power_of_two(k)));
            return propagate(x, core, a_l_b, s.eq(q));
        }
        return false;
    }


    /**
     * let q1 = x1 / y1, q2 = x2 / y2
     * x1 <= x2  & y1 >= y2 => q1 <= q2
     * y1 <= y2 & q1 < q2 => (x2 - x1) >= (q2 - q1 - 1) * y1
     * 
     * Limitation/assumption:
     * Values of x1, y1, q1 have to be available for the rule to apply.
     * If not all values are present, the rule isn't going to be used.
     * The arithmetic solver uses complete assignments because it 
     * builds on top of an integer feasible state (or feasible over rationals)
     * Lemmas are false under that assignment. They don't necessarily propagate, though.
     * PolySAT isn't (yet) set up to work with complete assignments and thereforce misses such lemmas.
     * - should we force complete assignments by computing first a model that is feasible modulo linear constraints 
     *   (ignore non-linear constraints in linear mode)?
     * - should we detect forcing relations x1 <= x2, y2 <= y1 based on the constraints (not on assignments)?
     *   other saturation rules already do this, but it is highly syntactic whether they apply.
     * 
     * 
     * Other rules:
     *  x < y div z => x * z < y
     * 
     * Or just:
     *  (y div z) * z <= y, 
     *  ~overfl((y div z) * z)
     * 
     * ~overfl(x * y), z <= y => x * z <= x * y
     * 
     */
    bool saturation::try_div_monotonicity(conflict& core) {
        bool propagated = false;

        auto log = [&](auto& x1, auto& x1_val, auto& y1, auto& y1_val, auto& q1, auto& q1_val,
                                 auto& x2, auto& x2_val, auto& y2, auto& y2_val, auto& q2, auto& q2_val) {
            IF_VERBOSE(1, verbose_stream() << "Division monotonicity: [" << x1 << "] (" << x1_val << ") / [" << y1 << "] (" << y1_val << ") = " 
                       << s.var(q1) << "\n");
        };

#if 0
        // monotonicity0 lemma should be asserted eagerly.
        auto monotonicity0 = [&](auto& x1, auto& x1_val, auto& y1, auto& y1_val, auto& q1, auto& q1_val) {
            if (q1_val * y1_val <= x1_val)
                return;
            // q1*y1 + r1 = x1, q1*y1 <= -r1 - 1, q1*y1 <= x1
            propagated = true;
            set_rule("[x1, y1] (x1 / y1) * y1 <= x1");            
            m_lemma.reset();
            propagate(q1, core, s.ule(s.var(q1) * y1, x1));            
        };
#endif

        auto monotonicity1 = [&](auto& x1, auto& x1_val, auto& y1, auto& y1_val, auto& q1, auto& q1_val,
                                 auto& x2, auto& x2_val, auto& y2, auto& y2_val, auto& q2, auto& q2_val) {
            if (!(x1_val <= x2_val && y1_val >= y2_val && q1_val > q2_val))
                return;
            propagated = true;
            set_rule("[x1, y1, x2, y2] x1 <= x2 & y2 <= y1 => x1 / y1 <= x2 / y2");
            log(x1, x1_val, y1, y1_val, q1, q1_val, x2, x2_val, y2, y2_val, q2, q2_val);
            m_lemma.reset();
            m_lemma.insert_eval(~s.ule(x1, x2));
            m_lemma.insert_eval(~s.ule(y2, y1));
            propagate(q1, core, s.ule(s.var(q1), s.var(q2)));
        };

        auto monotonicity2 = [&](auto& x1, auto& x1_val, auto& y1, auto& y1_val, auto& q1, auto& q1_val,
                                 auto& x2, auto& x2_val, auto& y2, auto& y2_val, auto& q2, auto& q2_val) {
            if (!(y1_val <= y2_val && q1_val < q2_val && (x2_val - x1_val < (q2_val - q1_val - 1) * y1_val)))
                return;
            propagated = true;
            set_rule("[x1, y1, x2, y2] y2 >= y1 & q2 > q1 => x2 - x1 >= ((x2 / y2) - (x1 / y1) - 1) * y1");
            log(x1, x1_val, y1, y1_val, q1, q1_val, x2, x2_val, y2, y2_val, q2, q2_val);            
            m_lemma.reset();
            m_lemma.insert_eval(~s.uge(y2, y1));
            m_lemma.insert_eval(~s.ult(s.var(q1), s.var(q2)));
            propagate(q1, core, s.uge(x2 - x1, (s.var(q2) - s.var(q1) - 1) * y1));
        };
                                 
                                       
        for (auto const& [x1, y1, q1, r1] : s.m_constraints.div_constraints()) {
            rational x1_val, y1_val, q1_val;
            if (!s.try_eval(x1, x1_val) || !s.try_eval(y1, y1_val) || !s.is_assigned(q1))
                continue;
            q1_val = s.get_value(q1);
            rational expected1 = y1_val.is_zero() ? y1.manager().max_value() : div(x1_val, y1_val);

            if (q1_val == expected1)
                continue;

            // force that q1 * y1 <= x1 if it isn't the case.
            // monotonicity0(x1, x1_val, y1, y1_val, q1, q1_val);

            for (auto const& [x2, y2, q2, r2] : s.m_constraints.div_constraints()) {            
                if (x1 == x2 && y1 == y2)
                    continue;
                if (x1.power_of_2() != x2.power_of_2())
                    continue;
                rational x2_val, y2_val, q2_val;
                if (!s.try_eval(x2, x2_val) || !s.try_eval(y2, y2_val) || !s.is_assigned(q2))
                    continue;
                q2_val = s.get_value(q2);
                monotonicity1(x1, x1_val, y1, y1_val, q1, q1_val, x2, x2_val, y2, y2_val, q2, q2_val);
                monotonicity1(x2, x2_val, y2, y2_val, q2, q2_val, x1, x1_val, y1, y1_val, q1, q1_val);
                monotonicity2(x1, x1_val, y1, y1_val, q1, q1_val, x2, x2_val, y2, y2_val, q2, q2_val);
                monotonicity2(x2, x2_val, y2, y2_val, q2, q2_val, x1, x1_val, y1, y1_val, q1, q1_val);                
            }
        }
        return propagated;
    }

    /*
     * TODO
     *
     * Maybe also
     * x*y = k => \/_{j is such that there is j', j*j' = k} x = j
     * x*y = k & ~ovfl(x,y) & x = j => y = k/j where j is a divisor of k
     */


    /**
     * [x] p(x) <= q(x) where value(p) > value(q)
     *     ==> q <= value(q) => p <= value(q)
     *
     * for strict?
     *     p(x) < q(x) where value(p) >= value(q)
     *     ==> value(p) <= p => value(p) < q
     */
    bool saturation::try_tangent(pvar v, conflict& core, inequality const& c) {
        set_rule("[x] p(x) <= q(x) where value(p) > value(q)");
        // if (c.is_strict())
        //     return false;
        if (!c.as_signed_constraint()->contains_var(v))
            return false;
        if (c.lhs().is_val() || c.rhs().is_val())
            return false;

        auto& m = s.var2pdd(v);
        pdd q_l(m), e_l(m), q_r(m), e_r(m);
        bool is_linear = true;
        is_linear &= c.lhs().degree(v) <= 1;
        is_linear &= c.rhs().degree(v) <= 1;
        if (c.lhs().degree(v) == 1) {
            c.lhs().factor(v, 1, q_l, e_l);
            is_linear &= q_l.is_val();
        }
        if (c.rhs().degree(v) == 1) {
            c.rhs().factor(v, 1, q_r, e_r);
            is_linear &= q_r.is_val();
        }
        if (is_linear)
            return false;

        if (!c.as_signed_constraint().is_currently_false(s))
            return false;
        rational l_val, r_val;
        if (!s.try_eval(c.lhs(), l_val))
            return false;
        if (!s.try_eval(c.rhs(), r_val))
            return false;
        SASSERT(c.is_strict() || l_val > r_val);
        SASSERT(!c.is_strict() || l_val >= r_val);
        m_lemma.reset();
        if (c.is_strict()) {
            auto d = s.ule(l_val, c.lhs());
            if (d.bvalue(s) == l_false) // it is a different value conflict that contains v
                return false;
            m_lemma.insert_eval(~d);
            auto conseq = s.ult(r_val, c.rhs());
            return add_conflict(v, core, c, conseq);
        }
        else {
            auto d = s.ule(c.rhs(), r_val);
            if (d.bvalue(s) == l_false) // it is a different value conflict that contains v
                return false;
            m_lemma.insert_eval(~d);
            auto conseq = s.ule(c.lhs(), r_val);
            return add_conflict(v, core, c, conseq);
        }
    }

#endif

}
