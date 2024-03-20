/*++
Copyright (c) 2020 Microsoft Corporation

Module Name:

    polysat_core.h

Abstract:

    Core solver for polysat

Author:

    Nikolaj Bjorner (nbjorner) 2020-08-30
    Jakob Rath 2021-04-06

--*/
#pragma once

#include "util/var_queue.h"
#include "util/dependency.h"
#include "math/dd/dd_pdd.h"
#include "sat/sat_extension.h"
#include "sat/smt/polysat/types.h"
#include "sat/smt/polysat/constraints.h"
#include "sat/smt/polysat/viable.h"
#include "sat/smt/polysat/assignment.h"
#include "sat/smt/polysat/monomials.h"

namespace polysat {

    class core;
    class solver_interface;



    class core {
        struct var_activity {
            unsigned sz;
            unsigned act;
            bool operator<(var_activity const& other) const {
                if (other.sz != sz)
                    return sz > other.sz;
                return act < other.act;
            };
            bool operator>(var_activity const& other) const {
                return other < *this;
            }
        };
        class mk_add_var;
        class mk_dqueue_var;
        class mk_assign_var;
        class mk_add_watch;
        typedef svector<var_activity> activity;
        friend class viable;
        friend class constraints;
        friend class assignment;

        struct constraint_info {
            signed_constraint sc;        // signed constraint representation
            dependency d;                // justification for constraint
            lbool      value;            // value assigned by solver
        };
        solver_interface& s;
        mutable scoped_ptr_vector<dd::pdd_manager> m_pdd;
        viable m_viable;
        constraints m_constraints;
        assignment m_assignment;
        monomials  m_monomials;
        unsigned m_qhead = 0;
        constraint_id_vector m_prop_queue;
        svector<constraint_info> m_constraint_index;  // index of constraints
        dependency_vector m_unsat_core;
        random_gen           m_rand;


        // attributes associated with variables
        vector<pdd>             m_vars;                       // for each variable a pdd
        vector<rational>        m_values;                     // current value of assigned variable
        dependency_vector       m_justification;              // justification for assignment
        activity                m_activity;                   // activity of variables
        var_queue<activity>     m_var_queue;                  // priority queue of variables to assign
        vector<unsigned_vector> m_watch;                      // watch lists for variables for constraints on m_prop_queue where they occur

        // values to split on
        pvar        m_var = 0;

        dd::pdd_manager& sz2pdd(unsigned sz) const;
        dd::pdd_manager& var2pdd(pvar v) const;

        void del_var();

        void viable_conflict(pvar v);
        void viable_propagate(pvar v, rational const& var_value);

        bool is_assigned(pvar v) { return !m_justification[v].is_null(); }
        void propagate_assignment(constraint_id idx);
        void propagate_eval(constraint_id idx);
        void propagate_assignment(pvar v, rational const& value, dependency dep);
        void propagate_activation(constraint_id idx, signed_constraint& sc, dependency dep);
        dependency_vector explain_weak_eval(unsigned_vector const& vars);

        void add_watch(unsigned idx, unsigned var);

        lbool assign_variable();
        lbool assign_variable(pvar v);
        
        void add_opdef(signed_constraint sc);

        unsigned m_activity_inc = 128;
        void inc_activity(pvar v);
        void rescale_activity();
        void decay_activity();

    public:
        core(solver_interface& s);

        sat::check_result check();        
        constraint_id register_constraint(signed_constraint& sc, dependency d);
        bool propagate();
        void assign_eh(constraint_id idx, bool sign);
        pvar next_var() { return m_var_queue.next_var(); }

        pdd value(rational const& v, unsigned sz);
        pdd subst(pdd const&);
        bool try_eval(pdd const& p, rational& r);

        void collect_statistics(statistics& st) const;

        signed_constraint eq(pdd const& p) { return m_constraints.eq(p); }
        signed_constraint eq(pdd const& p, pdd const& q) { return m_constraints.eq(p - q); }
        signed_constraint ule(pdd const& p, pdd const& q) { return m_constraints.ule(p, q); }
        signed_constraint sle(pdd const& p, pdd const& q) { return m_constraints.sle(p, q); }
        signed_constraint umul_ovfl(pdd const& p, pdd const& q) { return m_constraints.umul_ovfl(p, q); }
        signed_constraint bit(pdd const& p, unsigned i) { return m_constraints.bit(p, i); }


        void lshr(pdd const& a, pdd const& b, pdd const& r) { add_opdef(m_constraints.lshr(a, b, r)); }
        void ashr(pdd const& a, pdd const& b, pdd const& r) { add_opdef(m_constraints.ashr(a, b, r)); }
        void shl(pdd const& a, pdd const& b, pdd const& r) { add_opdef(m_constraints.shl(a, b, r)); }
        void band(pdd const& a, pdd const& b, pdd const& r) { add_opdef(m_constraints.band(a, b, r)); }
        void bor(pdd const& a, pdd const& b, pdd const& r) { add_opdef(m_constraints.bor(a, b, r)); }

        pdd bnot(pdd p) { return -p - 1; }
        pvar mul(unsigned n, pdd const* args) { return m_monomials.mk(n, args); }


        /*
        * Add a named clause. Dependencies are assumed, signed constraints are guaranteed.
        * In other words, the clause represents the formula /\ d_i -> \/ sc_j
        * Where d_i are logical interpretations of dependencies and sc_j are signed constraints.
        */
        bool add_axiom(char const* name, constraint_or_dependency_list const& cs, bool is_redundant);
        bool add_axiom(char const* name, constraint_or_dependency const* begin, constraint_or_dependency const* end, bool is_redundant);
        bool add_axiom(char const* name, constraint_or_dependency_vector const& cs, bool is_redundant);
        
        pvar add_var(unsigned sz);
        pdd var(pvar p) { return m_vars[p]; }
        unsigned size(pvar v) const { return m_vars[v].power_of_2(); }

        constraints& cs() { return m_constraints; }
        monomials& ms() { return m_monomials; }
        trail_stack& trail();

        unsigned level(dependency const& d) { return s.level(d); }

        std::ostream& display(std::ostream& out) const;

        /*
        * Viable 
        */
        void get_bitvector_suffixes(pvar v, offset_slices& out);
        void get_fixed_bits(pvar v, fixed_bits_vector& fixed_slices);
        void get_subslices(pvar v, offset_slices& out);
        void get_fixed_subslices(pvar v, fixed_bits_vector& fixed_subslices);
        pdd  mk_zero_extend(unsigned sz, pdd const& p);
        pdd  mk_extract(unsigned hi, unsigned lo, pdd const& p);

        /*
        * Saturation
        */
        signed_constraint get_constraint(constraint_id id);
        dependency_vector const& unsat_core() const { return m_unsat_core; }
        constraint_id_vector const& assigned_constraints() const { return m_prop_queue; }
        dependency get_dependency(constraint_id idx) const;
        // dependency_vector get_dependencies(constraint_id_vector const& ids) const;
        lbool weak_eval(constraint_id id);
        lbool strong_eval(constraint_id id);
        dependency propagate(signed_constraint const& sc, dependency_vector const& deps, char const* hint = nullptr) { return s.propagate(sc, deps, hint); }
        lbool weak_eval(signed_constraint const& sc);
        lbool strong_eval(signed_constraint const& sc);
        dependency_vector explain_weak_eval(signed_constraint const& sc);
        dependency_vector explain_strong_eval(signed_constraint const& sc);
        svector<pvar> find_conflict_variables(constraint_id idx);
        bool inconsistent() const;

        /*
        * Constraints
        */
        assignment& get_assignment() { return m_assignment; }

        random_gen& rand() { return m_rand; }

        pdd mk_ite(signed_constraint const& sc, pdd const& p, pdd const& q) { return s.mk_ite(sc, p, q); }
    };

}
