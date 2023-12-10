/*++
Copyright (c) 2021 Microsoft Corporation

Module Name:

    polysat substitution and assignment

Author:

    Nikolaj Bjorner (nbjorner) 2021-03-19
    Jakob Rath 2021-04-06

--*/
#pragma once
#include "util/scoped_ptr_vector.h"
#include "sat/smt/polysat/polysat_types.h"

namespace polysat {

    class core;

    using assignment_item_t = std::pair<pvar, rational>;

    class substitution_iterator {
        pdd m_current;
        substitution_iterator(pdd current) : m_current(std::move(current)) {}
        friend class substitution;

    public:
        using value_type = assignment_item_t;
        using difference_type = std::ptrdiff_t;
        using pointer = value_type const*;
        using reference = value_type const&;
        using iterator_category = std::input_iterator_tag;

        substitution_iterator& operator++() {
            SASSERT(!m_current.is_val());
            m_current = m_current.hi();
            return *this;
        }

        value_type operator*() const {
            SASSERT(!m_current.is_val());
            return { m_current.var(), m_current.lo().val() };
        }

        bool operator==(substitution_iterator const& other) const { return m_current == other.m_current; }
        bool operator!=(substitution_iterator const& other) const { return !operator==(other); }
    };

    /** Substitution for a single bit width. */
    class substitution {
        pdd m_subst;

        substitution(pdd p);

    public:
        substitution(dd::pdd_manager& m);
        [[nodiscard]] substitution add(pvar var, rational const& value) const;
        [[nodiscard]] pdd apply_to(pdd const& p) const;

        [[nodiscard]] bool contains(pvar var) const;
        [[nodiscard]] bool value(pvar var, rational& out_value) const;

        [[nodiscard]] bool empty() const { return m_subst.is_one(); }

        pdd const& to_pdd() const { return m_subst; }
        unsigned bit_width() const { return to_pdd().power_of_2(); }

        bool operator==(substitution const& other) const { return &m_subst.manager() == &other.m_subst.manager() && m_subst == other.m_subst; }
        bool operator!=(substitution const& other) const { return !operator==(other); }

        std::ostream& display(std::ostream& out) const;

        using const_iterator = substitution_iterator;
        const_iterator begin() const { return {m_subst}; }
        const_iterator end() const { return {m_subst.manager().one()}; }
    };

    /** Full variable assignment, may include variables of varying bit widths. */
    class assignment {
        core& m_core;
        vector<assignment_item_t>               m_pairs;
        mutable scoped_ptr_vector<substitution> m_subst;
        vector<substitution>                    m_subst_trail;

        substitution& subst(unsigned sz);
        core& s() const { return m_core; }
    public:
        assignment(core& s);
        // prevent implicit copy, use clone() if you do need a copy
        assignment(assignment const&) = delete;
        assignment& operator=(assignment const&) = delete;

        void push(pvar var, rational const& value);
        void pop();

        pdd apply_to(pdd const& p) const;

        bool contains(pvar var) const;
        bool value(pvar var, rational& out_value) const;
        rational value(pvar var) const { rational val; VERIFY(value(var, val)); return val; }
        bool empty() const { return pairs().empty(); }
        substitution const& subst(unsigned sz) const;
        vector<assignment_item_t> const& pairs() const { return m_pairs; }
        using const_iterator = decltype(m_pairs)::const_iterator;
        const_iterator begin() const { return pairs().begin(); }
        const_iterator end() const { return pairs().end(); }

        std::ostream& display(std::ostream& out) const;
    };

    inline std::ostream& operator<<(std::ostream& out, substitution const& sub) { return sub.display(out); }

    inline std::ostream& operator<<(std::ostream& out, assignment const& a) { return a.display(out); }
}

