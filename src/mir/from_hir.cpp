/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/from_hir.cpp
 * - Construction of MIR from the HIR expression tree
 */
#include <type_traits>  // for TU_MATCHA
#include <algorithm>
#include "mir.hpp"
#include "mir_ptr.hpp"
#include <hir/expr.hpp>
#include <hir/hir.hpp>
#include <hir/visitor.hpp>
#include <hir_typeck/common.hpp>   // monomorphise_type
#include "main_bindings.hpp"
#include "from_hir.hpp"
#include "operations.hpp"
#include <mir/visit_crate_mir.hpp>
#include <hir/expr_state.hpp>
#include <trans/target.hpp> // Target_GetSizeAndAlignOf - for `box`
#include <cctype>   // isdigit
#include "helpers.hpp"

namespace {

    template<typename T>
    struct SaveAndEditVal {
        T&  m_dst;
        T   m_saved;
        SaveAndEditVal(T& dst, T newval):
            m_dst(dst),
            m_saved(dst)
        {
            m_dst = mv$(newval);
        }
        ~SaveAndEditVal()
        {
            this->m_dst = this->m_saved;
        }
    };
    template<typename T>
    SaveAndEditVal<T> save_and_edit(T& dst, typename ::std::remove_reference<T&>::type newval) {
        return SaveAndEditVal<T> { dst, mv$(newval) };
    }

    class ExprVisitor_Conv:
        public MirConverter
    {
        MirBuilder& m_builder;

        const ::std::vector< ::HIR::TypeRef>&  m_variable_types;

        /// Generators do some different codegen quirks
        bool    m_is_generator;

        struct LoopDesc {
            ScopeHandle scope;
            RcString   label;
            bool    require_label;
            unsigned int    cur;
            unsigned int    next;
            ::MIR::LValue   res_value;
        };
        ::std::vector<LoopDesc> m_loop_stack;

        const ScopeHandle*  m_block_tmp_scope = nullptr;
        const ScopeHandle*  m_borrow_raise_target = nullptr;
        const ScopeHandle*  m_stmt_scope = nullptr;
        bool m_in_borrow = false;

        struct GeneratorState {
            struct State {
                /// Entrypoint for the state
                MIR::BasicBlockId   entrypoint;
                /// List of saved variables when this state yields
                std::map<unsigned, MirBuilder::SavedActiveLocal>  saved;

                State(MIR::BasicBlockId entry): entrypoint(entry) {}

                State(State&&) = default;
            };
            // Basic block to be terminated with the state switch
            MIR::BasicBlockId   bb_open;
            /// Yield points/states
            std::vector<State>  states;

            ::HIR::SimplePath   state_idx_enm_path;
        } m_generator_state;

    public:
        ExprVisitor_Conv(MirBuilder& builder, const ::std::vector< ::HIR::TypeRef>& var_types, const ::HIR::ExprNode_GeneratorWrapper* is_generator):
            m_builder(builder),
            m_variable_types(var_types),
            m_is_generator(is_generator != nullptr)
        {
            if(m_is_generator)
            {
                m_generator_state.state_idx_enm_path = is_generator->m_state_idx_enum;
                m_generator_state.bb_open = builder.pause_cur_block();
                m_generator_state.states.push_back(GeneratorState::State(builder.new_bb_unlinked()));
                builder.set_cur_block(m_generator_state.states.back().entrypoint);
            }
        }

        // Get a LValue pointing at the state index
        ::MIR::LValue generator_state_lv() const
        {
            // (*self.ptr(?0)).state(0).value(?#1).idx(0)
            auto rv = ::MIR::LValue::new_Argument(0);
            rv = ::MIR::LValue::new_Field(mv$(rv), 0);   // .ptr (From Pin)
            rv = ::MIR::LValue::new_Deref(mv$(rv));     // .*
            rv = ::MIR::LValue::new_Field(mv$(rv), 0);   // .state
            rv = ::MIR::LValue::new_Downcast(mv$(rv), 1);   // .value (From MaybeUninit)
            rv = ::MIR::LValue::new_Field(mv$(rv), 0);   // .value (From ManuallyDrop)
            rv = ::MIR::LValue::new_Field(mv$(rv), 0);   // .idx
            return rv;
        }
        std::set<unsigned> generator_finalise(const Span& sp, ::HIR::Enum& state_enm)
        {
            std::set<unsigned>   used_vars;
            std::vector<MIR::BasicBlockId>  arm_targets; arm_targets.reserve(m_generator_state.states.size()+1);
            ::std::vector<HIR::Enum::ValueVariant> enum_variants; enum_variants.reserve(m_generator_state.states.size()+1);
            for(const auto& s : m_generator_state.states)
            {
                arm_targets.push_back(m_builder.new_bb_unlinked());

                m_builder.set_cur_block(arm_targets.back());
                m_builder.push_stmt_assign(sp, generator_state_lv(), ::MIR::RValue::make_EnumVariant({
                    m_generator_state.state_idx_enm_path, static_cast<unsigned>(m_generator_state.states.size()), {}
                    }));
                m_builder.end_block( ::MIR::Terminator::make_Goto(s.entrypoint) );

                enum_variants.push_back(HIR::Enum::ValueVariant {
                    RcString(), ::HIR::ExprPtr(), arm_targets.size()-1
                    });
                for(const auto& e : s.saved)
                {
                    used_vars.insert(e.first);
                }
            }
            // Final arm is the end/panic state - it's a bug to reach this
            arm_targets.push_back(m_builder.new_bb_unlinked());
            m_builder.set_cur_block(arm_targets.back());
            m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );

            enum_variants.push_back(HIR::Enum::ValueVariant {
                RcString::new_interned("END"), ::HIR::ExprPtr(), arm_targets.size()-1
                });
            state_enm.m_data = ::HIR::Enum::Class::make_Value({ mv$(enum_variants), true });

            m_builder.set_cur_block(m_generator_state.bb_open);

            // switch _n { ... }
            m_builder.end_block( ::MIR::Terminator::make_Switch({ generator_state_lv(), mv$(arm_targets) }) );

            return used_vars;
        }
        void generator_make_drop(const Span& sp, MirBuilder& out_builder, size_t n_captures, const ::std::map<unsigned, std::vector<MIR::LValue::Wrapper>>& mappings) const
        {
            ::MIR::LValue   self = ::MIR::LValue::new_Deref( ::MIR::LValue::new_Argument(0) );
            auto get_lv = [&self,&mappings](unsigned idx) {
                ::MIR::LValue rv = self.clone();
                rv.m_wrappers.insert(rv.m_wrappers.end(), mappings.at(idx).begin(), mappings.at(idx).end());
                return rv;
                };

            assert(m_generator_state.states.size() > 0);
            std::vector<::MIR::BasicBlockId>    arms; arms.reserve(m_generator_state.states.size()+1);

            auto entry_block = out_builder.pause_cur_block();
            // if state is 0, then drop captures (this is the pre-run state)
            arms.push_back(out_builder.new_bb_unlinked());
            out_builder.set_cur_block(arms.back());
            for(size_t i = 0; i < n_captures; i ++)
            {
                out_builder.push_stmt_drop(sp, get_lv(1+i));
            }
            out_builder.end_block(::MIR::Terminator::make_Return({}));

            // Else, drop yield saves (Note: final state has no saves, so acts as the "completed" state)
            for(size_t i = 0; i < m_generator_state.states.size(); i ++)
            {
                // 
                arms.push_back(out_builder.new_bb_unlinked());
                out_builder.set_cur_block(arms.back());
                for(const auto& v : m_generator_state.states[i].saved)
                {
                    if( v.first == 0 ) {
                        continue ;
                    }
                    out_builder.drop_actve_local(sp, get_lv(v.first), v.second);
                }
                out_builder.end_block(::MIR::Terminator::make_Return({}));
            }
            // Generate the dispatch switch
            out_builder.set_cur_block(entry_block);
            out_builder.push_stmt_assign( sp, ::MIR::LValue::new_Return(), ::MIR::RValue::make_Tuple({}) );
            auto stmt_idx_lv = mv$(self);
            stmt_idx_lv = ::MIR::LValue::new_Field(mv$(stmt_idx_lv), 0);   // .state
            stmt_idx_lv = ::MIR::LValue::new_Downcast(mv$(stmt_idx_lv), 1);   // .value (From MaybeUninit)
            stmt_idx_lv = ::MIR::LValue::new_Field(mv$(stmt_idx_lv), 0);   // .value (From ManuallyDrop)
            stmt_idx_lv = ::MIR::LValue::new_Field(mv$(stmt_idx_lv), 0);   // .idx
            out_builder.end_block( ::MIR::Terminator::make_Switch({ mv$(stmt_idx_lv), mv$(arms) }) );
        }

        void destructure_from(const Span& sp, const ::HIR::Pattern& pat, ::MIR::LValue lval, bool allow_refutable=false) override
        {
            auto cb = [&](const HIR::PatternBinding& binding, ::MIR::LValue lval) {
                MIR::RValue rv;
                switch( binding.m_type )
                {
                case ::HIR::PatternBinding::Type::Move:
                    rv = mv$(lval);
                    break;
                case ::HIR::PatternBinding::Type::Ref:
                    if(m_borrow_raise_target)
                    {
                        DEBUG("- Raising destructure borrow of " << lval << " to scope " << *m_borrow_raise_target);
                        m_builder.raise_temporaries(sp, lval, *m_borrow_raise_target);
                    }

                    rv = ::MIR::RValue::make_Borrow({ ::HIR::BorrowType::Shared, mv$(lval) });
                    break;
                case ::HIR::PatternBinding::Type::MutRef:
                    if(m_borrow_raise_target)
                    {
                        DEBUG("- Raising destructure borrow of " << lval << " to scope " << *m_borrow_raise_target);
                        m_builder.raise_temporaries(sp, lval, *m_borrow_raise_target);
                    }
                    rv = ::MIR::RValue::make_Borrow({ ::HIR::BorrowType::Unique, mv$(lval) });
                    break;
                }
                m_builder.push_stmt_assign( sp, m_builder.get_variable(sp, binding.m_slot), mv$(rv) );
                };
            destructure_from_ex(sp, pat, mv$(lval), cb, (allow_refutable ? AllowRefutable::Yes : AllowRefutable::No));
        }
        void destructure_aliases_from(const Span& sp, const ::HIR::Pattern& pat, ::MIR::LValue lval, bool allow_refutable=false) override
        {
            auto cb = [&](const HIR::PatternBinding& binding, ::MIR::LValue lv) {
                m_builder.add_variable_alias(sp, binding.m_slot, binding.m_type, mv$(lv));
            };
            destructure_from_ex(sp, pat, mv$(lval), cb, (allow_refutable ? AllowRefutable::Yes : AllowRefutable::No));
        }

        // Brings variables defined in `pat` into scope
        void define_vars_from(const Span& sp, const ::HIR::Pattern& pat) override
        {
            if( pat.m_binding.is_valid() ) {
                m_builder.define_variable( pat.m_binding.m_slot );
            }

            TU_MATCHA( (pat.m_data), (e),
            (Any,
                ),
            (Box,
                define_vars_from(sp, *e.sub);
                ),
            (Ref,
                define_vars_from(sp, *e.sub);
                ),
            (Tuple,
                for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                {
                    define_vars_from(sp, e.sub_patterns[i]);
                }
                ),
            (SplitTuple,
                for(unsigned int i = 0; i < e.leading.size(); i ++ )
                    define_vars_from(sp, e.leading[i]);
                for(unsigned int i = 0; i < e.trailing.size(); i ++ )
                    define_vars_from(sp, e.trailing[i]);
                ),
            (PathValue,
                // Nothing.
                ),
            (PathTuple,
                for(unsigned int i = 0; i < e.leading.size(); i ++ )
                    define_vars_from(sp, e.leading[i]);
                for(unsigned int i = 0; i < e.trailing.size(); i ++ )
                    define_vars_from(sp, e.trailing[i]);
                ),
            (PathNamed,
                for(const auto& fld_pat : e.sub_patterns)
                {
                    define_vars_from(sp, fld_pat.second);
                }
                ),
            // Refutable
            (Value,
                ),
            (Range,
                ),
            (Slice,
                for(const auto& subpat : e.sub_patterns)
                {
                    define_vars_from(sp, subpat);
                }
                ),
            (SplitSlice,
                for(const auto& subpat : e.leading)
                {
                    define_vars_from(sp, subpat);
                }
                if( e.extra_bind.is_valid() ) {
                    m_builder.define_variable( e.extra_bind.m_slot );
                }
                for(const auto& subpat : e.trailing)
                {
                    define_vars_from(sp, subpat);
                }
                )
            )
        }

        enum class AllowRefutable
        {
            No,
            Yes,
            NoBindings,
            IgnoreBindings,
        };

        void destructure_from_ex(const Span& sp, const ::HIR::Pattern& pat, ::MIR::LValue lval, std::function<void (const HIR::PatternBinding& binding, MIR::LValue lval)> cb, AllowRefutable allow_refutable = AllowRefutable::No) // 1 : yes, 2 : disallow binding
        {
            TRACE_FUNCTION_F(pat << " := " << lval << ", allow_refutable=" << static_cast<int>(allow_refutable));
            if( allow_refutable != AllowRefutable::IgnoreBindings && pat.m_binding.is_valid() ) {
                if( allow_refutable == AllowRefutable::NoBindings ) {
                    BUG(sp, "Binding when not expected");
                }
                else if( allow_refutable == AllowRefutable::No ) {
                    ASSERT_BUG(sp, pat.m_data.is_Any(), "Destructure patterns can't bind and match");
                }
                else {
                    // Refutable and binding allowed
                    destructure_from_ex(sp, pat, lval.clone(), cb, AllowRefutable::IgnoreBindings);
                }

                for(size_t i = 0; i < pat.m_binding.m_implicit_deref_count; i ++)
                {
                    lval = ::MIR::LValue::new_Deref(mv$(lval));
                }

                cb(pat.m_binding, std::move(lval));
                return;
            }
            if( allow_refutable == AllowRefutable::IgnoreBindings ) {
                allow_refutable = AllowRefutable::NoBindings;
            }

            for(size_t i = 0; i < pat.m_implicit_deref_count; i ++)
            {
                lval = ::MIR::LValue::new_Deref(mv$(lval));
            }

            TU_MATCH_HDRA( (pat.m_data), {)
            TU_ARMA(Any, e) {
                }
            TU_ARMA(Box, e) {
                destructure_from_ex(sp, *e.sub, ::MIR::LValue::new_Deref(mv$(lval)), cb, allow_refutable);
                }
            TU_ARMA(Ref, e) {
                destructure_from_ex(sp, *e.sub, ::MIR::LValue::new_Deref(mv$(lval)), cb, allow_refutable);
                }
            TU_ARMA(Tuple, e) {
                for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                {
                    destructure_from_ex(sp, e.sub_patterns[i], ::MIR::LValue::new_Field(lval.clone(), i), cb, allow_refutable);
                }
                }
            TU_ARMA(SplitTuple, e) {
                assert(e.total_size >= e.leading.size() + e.trailing.size());
                for(unsigned int i = 0; i < e.leading.size(); i ++ )
                {
                    destructure_from_ex(sp, e.leading[i], ::MIR::LValue::new_Field(lval.clone(), i), cb, allow_refutable);
                }
                // TODO: Is there a binding in the middle?
                unsigned int ofs = e.total_size - e.trailing.size();
                for(unsigned int i = 0; i < e.trailing.size(); i ++ )
                {
                    destructure_from_ex(sp, e.trailing[i], ::MIR::LValue::new_Field(lval.clone(), ofs+i), cb, allow_refutable);
                }
                }
            TU_ARMA(PathValue, e) {
                // Nothing.
                if( e.binding.is_Enum() && e.binding.as_Enum().ptr->num_variants() > 1 ) {
                    ASSERT_BUG(sp, allow_refutable != AllowRefutable::No, "Refutable pattern not expected - " << pat);
                }
                }
            TU_ARMA(PathTuple, e) {
                if(const auto* be = e.binding.opt_Enum())
                {
                    const auto& enm = *be->ptr;
                    const auto& variants = enm.m_data.as_Data();
                    // Check that this is the only non-impossible arm
                    if( allow_refutable == AllowRefutable::No )
                    {
                        for(size_t i = 0; i < variants.size(); i ++)
                        {
                            const auto& var_ty = variants[i].type;
                            // Skip this arm
                            if( i == be->var_idx ) {
                                continue;
                            }
                            ::HIR::TypeRef  tmp;
                            const auto& ty = monomorphise_type_with_opt(sp, tmp, var_ty, MonomorphStatePtr(nullptr, &e.path.m_data.as_Generic().m_params, nullptr));
                            if( m_builder.resolve().type_is_impossible(sp, ty) ) {
                                continue;
                            }
                            ERROR(sp, E0000, "Variant " << variants[i].name << " not handled");
                        }
                    }
                    lval = ::MIR::LValue::new_Downcast(mv$(lval), be->var_idx);
                }
                for(unsigned int i = 0; i < e.leading.size(); i ++ )
                {
                    destructure_from_ex(sp, e.leading[i], ::MIR::LValue::new_Field(lval.clone(), i), cb, allow_refutable);
                }
                for(unsigned int i = 0; i < e.trailing.size(); i ++ )
                {
                    destructure_from_ex(sp, e.trailing[i], ::MIR::LValue::new_Field(lval.clone(), e.total_size - e.trailing.size() + i), cb, allow_refutable);
                }
                }
            TU_ARMA(PathNamed, e) {
                if(const auto* be = e.binding.opt_Enum())
                {
                    const auto& enm = *be->ptr;
                    if(enm.m_data.is_Data())
                    {
                        const auto& variants = enm.m_data.as_Data();
                        // Check that this is the only non-impossible arm
                        if( allow_refutable == AllowRefutable::No )
                        {
                            for(size_t i = 0; i < variants.size(); i ++)
                            {
                                const auto& var_ty = variants[i].type;
                                // Skip this arm
                                if( i == be->var_idx ) {
                                    continue;
                                }
                                ::HIR::TypeRef  tmp;
                                const auto& ty = monomorphise_type_with_opt(sp, tmp, var_ty, MonomorphStatePtr(nullptr, &e.path.m_data.as_Generic().m_params, nullptr));
                                if( m_builder.resolve().type_is_impossible(sp, ty) ) {
                                    continue;
                                }
                                ERROR(sp, E0000, "Variant " << variants[i].name << " not handled");
                            }
                        }
                    }
                    lval = ::MIR::LValue::new_Downcast(mv$(lval), be->var_idx);
                }
                if( !e.sub_patterns.empty() )
                {
                    const auto& fields = ::HIR::pattern_get_named(sp, e.path, e.binding);
                    for(const auto& fld_pat : e.sub_patterns)
                    {
                        unsigned idx = ::std::find_if( fields.begin(), fields.end(), [&](const auto&x){ return x.first == fld_pat.first; } ) - fields.begin();
                        destructure_from_ex(sp, fld_pat.second, ::MIR::LValue::new_Field(lval.clone(), idx), cb, allow_refutable);
                    }
                }
                }
            // Refutable
            TU_ARMA(Value, e) {
                ASSERT_BUG(sp, allow_refutable != AllowRefutable::No, "Refutable pattern not expected - " << pat);
                }
            TU_ARMA(Range, e) {
                ASSERT_BUG(sp, allow_refutable != AllowRefutable::No, "Refutable pattern not expected - " << pat);
                }
            TU_ARMA(Slice, e) {
                // These are only refutable if T is [T]
                bool ty_is_array = false;
                m_builder.with_val_type(sp, lval, [&ty_is_array](const auto& ty){
                    ty_is_array = ty.data().is_Array();
                    });
                if( ty_is_array )
                {
                    // TODO: Assert array size
                    for(unsigned int i = 0; i < e.sub_patterns.size(); i ++)
                    {
                        const auto& subpat = e.sub_patterns[i];
                        destructure_from_ex(sp, subpat, ::MIR::LValue::new_Field(lval.clone(), i), cb, allow_refutable );
                    }
                }
                else
                {
                    ASSERT_BUG(sp, allow_refutable != AllowRefutable::No, "Refutable pattern not expected - " << pat);

                    // TODO: Emit code to triple-check the size? Or just assume that match did that correctly.
                    for(unsigned int i = 0; i < e.sub_patterns.size(); i ++)
                    {
                        const auto& subpat = e.sub_patterns[i];
                        destructure_from_ex(sp, subpat, ::MIR::LValue::new_Field(lval.clone(), i), cb, allow_refutable );
                    }
                }
                }
            TU_ARMA(SplitSlice, e) {
                // These are only refutable if T is [T]
                bool ty_is_array = false;
                unsigned int array_size = 0;
                ::HIR::TypeRef  inner_type;
                m_builder.with_val_type(sp, lval, [&ty_is_array,&array_size,&e,&inner_type](const auto& ty){
                    if( ty.data().is_Array() ) {
                        array_size = ty.data().as_Array().size.as_Known();
                        if( e.extra_bind.is_valid() )
                            inner_type = ty.data().as_Array().inner.clone();
                        ty_is_array = true;
                    }
                    else {
                        if( e.extra_bind.is_valid() )
                            inner_type = ty.data().as_Slice().inner.clone();
                        ty_is_array = false;
                    }
                    });
                if( ty_is_array )
                {
                    assert(array_size >= e.leading.size() + e.trailing.size());
                    for(unsigned int i = 0; i < e.leading.size(); i ++)
                    {
                        unsigned int idx = 0 + i;
                        destructure_from_ex(sp, e.leading[i], ::MIR::LValue::new_Field(lval.clone(), idx), cb, allow_refutable );
                    }
                    if( e.extra_bind.is_valid() )
                    {
                        TODO(sp, "Destructure array obtaining remainder");
                    }
                    for(unsigned int i = 0; i < e.trailing.size(); i ++)
                    {
                        unsigned int idx = array_size - e.trailing.size() + i;
                        destructure_from_ex(sp, e.trailing[i], ::MIR::LValue::new_Field(lval.clone(), idx), cb, allow_refutable );
                    }
                }
                else
                {
                    ASSERT_BUG(sp, allow_refutable != AllowRefutable::No, "Refutable pattern not expected - " << pat);

                    struct H {
                        static ::HIR::BorrowType get_borrow_type(const Span& sp, const ::HIR::PatternBinding& pb) {
                            switch(pb.m_type)
                            {
                            case ::HIR::PatternBinding::Type::Move:
                                BUG(sp, "By-value pattern binding of a slice");
                            case ::HIR::PatternBinding::Type::Ref:
                                return ::HIR::BorrowType::Shared;
                            case ::HIR::PatternBinding::Type::MutRef:
                                return ::HIR::BorrowType::Unique;
                            }
                            throw "";
                        }
                    };

                    // Acquire the slice size variable.
                    ::MIR::LValue   len_lval;
                    if( e.extra_bind.is_valid() || e.trailing.size() > 0 )
                    {
                        len_lval = m_builder.lvalue_or_temp(sp, ::HIR::CoreType::Usize, ::MIR::RValue::make_DstMeta({ m_builder.get_ptr_to_dst(sp, lval) }));
                    }

                    for(unsigned int i = 0; i < e.leading.size(); i ++)
                    {
                        unsigned int idx = i;
                        destructure_from_ex(sp, e.leading[i], ::MIR::LValue::new_Field(lval.clone(), idx), cb, allow_refutable );
                    }
                    if( e.extra_bind.is_valid() )
                    {
                        // 1. Obtain remaining length
                        auto sub_val = ::MIR::Param(::MIR::Constant::make_Uint({ e.leading.size() + e.trailing.size(), ::HIR::CoreType::Usize }));
                        ::MIR::LValue len_val = m_builder.lvalue_or_temp(sp, ::HIR::CoreType::Usize, ::MIR::RValue::make_BinOp({ len_lval.clone(), ::MIR::eBinOp::SUB, mv$(sub_val) }) );

                        // 2. Obtain pointer to element
                        ::HIR::BorrowType   bt = H::get_borrow_type(sp, e.extra_bind);
                        ::MIR::LValue ptr_val = m_builder.lvalue_or_temp(sp,
                            ::HIR::TypeRef::new_borrow( bt, inner_type.clone() ),
                            ::MIR::RValue::make_Borrow({ bt, ::MIR::LValue::new_Field( lval.clone(), static_cast<unsigned int>(e.leading.size()) ) })
                            );
                        // TODO: Cast to raw pointer? Or keep as a borrow?

                        // Construct fat pointer
                        m_builder.push_stmt_assign( sp, m_builder.get_variable(sp, e.extra_bind.m_slot), ::MIR::RValue::make_MakeDst({ mv$(ptr_val), mv$(len_val) }) );
                    }
                    if( e.trailing.size() > 0 )
                    {
                        for(size_t i = 0; i < e.trailing.size(); i ++)
                        {
                            // Dynamically create an index
                            auto sub_val = ::MIR::Param(::MIR::Constant::make_Uint({ e.trailing.size() - i, ::HIR::CoreType::Usize }));
                            ::MIR::LValue ofs_val = m_builder.lvalue_or_temp(sp, ::HIR::CoreType::Usize, ::MIR::RValue::make_BinOp({ len_lval.clone(), ::MIR::eBinOp::SUB, mv$(sub_val) }) );
                            // Recurse with the indexed value
                            destructure_from_ex(sp, e.trailing[i], ::MIR::LValue::new_Index( lval.clone(), ofs_val.m_root.as_Local() ), cb, allow_refutable);
                        }
                    }
                }
                }
            } // TU_MATCH_HDRA
        }

        // -- ExprVisitor
        void visit_node_ptr(::HIR::ExprNodeP& node_p) override
        {
            DEBUG(node_p.get());
            ::HIR::ExprVisitor::visit_node_ptr(node_p);
        }
        void visit(::HIR::ExprNode_Block& node) override
        {
            TRACE_FUNCTION_F("_Block");
            // NOTE: This doesn't create a BB, as BBs are not needed for scoping
            bool diverged = false;

            auto res_val = (node.m_value_node ? m_builder.new_temporary(node.m_res_type) : ::MIR::LValue());
            auto scope = m_builder.new_scope_var(node.span());
            auto tmp_scope = m_builder.new_scope_temp(node.span());
            auto _block_tmp_scope = save_and_edit(m_block_tmp_scope, &tmp_scope);

            for(unsigned int i = 0; i < node.m_nodes.size(); i ++)
            {
                auto _ = save_and_edit(m_borrow_raise_target, nullptr);
                auto& subnode = node.m_nodes[i];
                const Span& sp = subnode->span();

                auto stmt_scope = m_builder.new_scope_temp(sp);
                auto _stmt_scope_push = save_and_edit(m_stmt_scope, &stmt_scope);
                this->visit_node_ptr(subnode);

                if( m_builder.block_active() || m_builder.has_result() ) {
                    // TODO: Emit a drop
                    m_builder.get_result(sp);
                    m_builder.terminate_scope(sp, mv$(stmt_scope));
                    diverged |= subnode->m_res_type.data().is_Diverge();
                }
                else {
                    m_builder.terminate_scope(sp, mv$(stmt_scope), false);

                    m_builder.set_cur_block( m_builder.new_bb_unlinked() );
                    diverged = true;
                }
            }

            // For the last node, specially handle.
            // TODO: Any temporaries defined within this node must be elevated into the parent scope
            if( node.m_value_node )
            {
                auto& subnode = node.m_value_node;
                const Span& sp = subnode->span();

                auto stmt_scope = m_builder.new_scope_temp(sp);
                this->visit_node_ptr(subnode);
                if( m_builder.has_result() || m_builder.block_active() )
                {
                    ASSERT_BUG(sp, m_builder.block_active(), "Result yielded, but no active block");
                    ASSERT_BUG(sp, m_builder.has_result(), "Active block but no result yeilded");
                    // PROBLEM: This can drop the result before we want to use it.

                    m_builder.push_stmt_assign(sp, res_val.clone(), m_builder.get_result(sp));

                    // If this block is part of a statement, raise all temporaries from this final scope to the enclosing scope
                    if( m_stmt_scope )
                    {
                        m_builder.raise_all(sp, mv$(stmt_scope), *m_stmt_scope);
                        //m_builder.terminate_scope(sp, mv$(stmt_scope));
                    }
                    else
                    {
                        m_builder.terminate_scope(sp, mv$(stmt_scope));
                    }
                    m_builder.set_result( node.span(), mv$(res_val) );
                }
                else
                {
                    m_builder.terminate_scope( sp, mv$(stmt_scope), false );
                    // Block diverged in final node.
                }
                m_builder.terminate_scope( node.span(), mv$(tmp_scope), m_builder.block_active() );
                m_builder.terminate_scope( node.span(), mv$(scope), m_builder.block_active() );
            }
            else
            {
                if( diverged )
                {
                    m_builder.terminate_scope( node.span(), mv$(tmp_scope), false );
                    m_builder.terminate_scope( node.span(), mv$(scope), false );
                    m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );
                    // Don't set a result if there's no block.
                }
                else
                {
                    m_builder.terminate_scope( node.span(), mv$(tmp_scope) );
                    m_builder.terminate_scope( node.span(), mv$(scope) );
                    m_builder.set_result(node.span(), ::MIR::RValue::make_Tuple({}));
                }
            }
        }
        void visit(::HIR::ExprNode_Asm& node) override
        {
            TRACE_FUNCTION_F("_Asm");

            ::std::vector< ::std::pair< ::std::string, ::MIR::LValue> > inputs;
            // Inputs just need to be in lvalues
            for(auto& v : node.m_inputs) {
                this->visit_node_ptr(v.value);
                auto lv = m_builder.get_result_in_lvalue(v.value->span(), v.value->m_res_type);
                inputs.push_back( ::std::make_pair(v.spec, mv$(lv)) );
            }

            ::std::vector< ::std::pair< ::std::string, ::MIR::LValue> > outputs;
            // Outputs can also (sometimes) be rvalues (only for `*m`?)
            for(auto& v : node.m_outputs) {
                this->visit_node_ptr(v.value);
                if( v.spec[0] != '=' && v.spec[0] != '+' )  // TODO: what does '+' mean?
                    ERROR(node.span(), E0000, "Assembly output specifiers must start with =");
                ::MIR::LValue   lv;
                if(v.spec[1] == '*')
                    lv = m_builder.get_result_in_lvalue(v.value->span(), v.value->m_res_type);
                else
                    lv = m_builder.get_result_unwrap_lvalue(v.value->span());
                outputs.push_back( ::std::make_pair(v.spec, mv$(lv)) );
            }

            m_builder.push_stmt_asm( node.span(), { node.m_template, mv$(outputs), mv$(inputs), node.m_clobbers, node.m_flags } );
            m_builder.set_result(node.span(), ::MIR::RValue::make_Tuple({}));
        }
        void visit(::HIR::ExprNode_Return& node) override
        {
            TRACE_FUNCTION_F("_Return");
            this->visit_node_ptr(node.m_value);

            if( m_is_generator )
            {
                ::HIR::GenericPath enm_path;
                m_builder.with_val_type(node.span(), ::MIR::LValue::new_Return(), [&](const ::HIR::TypeRef& ty) {
                    const auto& te = ty.data().as_Path();
                    enm_path = te.path.m_data.as_Generic().clone();
                    ASSERT_BUG(node.span(), te.binding.as_Enum()->find_variant("Complete") == 1, "");
                    });

                ::std::vector< ::MIR::Param>   values;
                values.push_back( m_builder.get_result_in_param(node.span(), node.m_value->m_res_type) );
                auto res = ::MIR::RValue::make_EnumVariant({
                    mv$(enm_path),
                    1,  // Complete is the second variant
                    mv$(values)
                    });
                m_builder.push_stmt_assign( node.span(), ::MIR::LValue::new_Return(), mv$(res) );
            }
            else
            {
                m_builder.push_stmt_assign( node.span(), ::MIR::LValue::new_Return(),  m_builder.get_result(node.span()) );
            }
            m_builder.terminate_scope_early( node.span(), m_builder.fcn_scope() );
            m_builder.end_block( ::MIR::Terminator::make_Return({}) );
        }
        void visit(::HIR::ExprNode_Yield& node) override
        {
            TRACE_FUNCTION_F("_Yield");
            if( m_is_generator )
            {
                ::HIR::GenericPath enm_path;
                m_builder.with_val_type(node.span(), ::MIR::LValue::new_Return(), [&](const ::HIR::TypeRef& ty) {
                    const auto& te = ty.data().as_Path();
                    enm_path = te.path.m_data.as_Generic().clone();
                    ASSERT_BUG(node.span(), te.binding.as_Enum()->find_variant("Yielded") == 0, "");
                    });

                this->visit_node_ptr(node.m_value);
                // Emit return, wrapped in GeneratorState::Yielded
                ::std::vector< ::MIR::Param>   values;
                values.push_back( m_builder.get_result_in_param(node.span(), node.m_value->m_res_type) );
                auto res = ::MIR::RValue::make_EnumVariant({
                    mv$(enm_path),
                    0,  // Yielded is the first variant
                    mv$(values)
                    });
                m_builder.push_stmt_assign( node.span(), ::MIR::LValue::new_Return(), mv$(res) );
                m_builder.push_stmt_assign( node.span(), generator_state_lv(), ::MIR::RValue::make_EnumVariant({
                    m_generator_state.state_idx_enm_path.clone(),
                    static_cast<unsigned>(m_generator_state.states.size()),
                    {}
                    }) );
                // NOTE: No scope terminate
                m_builder.end_block( ::MIR::Terminator::make_Return({}) );

                m_generator_state.states.back().saved = m_builder.get_active_locals();
                m_generator_state.states.push_back( m_builder.new_bb_unlinked() );
                m_builder.set_cur_block( m_generator_state.states.back().entrypoint );

                m_builder.set_result( node.span(), ::MIR::RValue::make_Tuple({}) );
            }
            else
            {
                BUG(node.span(), "Unexpected ExprNode_Yield (should have been re-written)");
            }
        }
        void visit(::HIR::ExprNode_Let& node) override
        {
            TRACE_FUNCTION_F("_Let " << node.m_pattern);
            this->define_vars_from(node.span(), node.m_pattern);
            if( node.m_value )
            {
                auto _ = save_and_edit(m_borrow_raise_target, m_block_tmp_scope);
                this->visit_node_ptr(node.m_value);

                if( ! m_builder.block_active() ) {
                    return ;
                }
                auto res = m_builder.get_result(node.span());

                if( node.m_pattern.m_binding.is_valid() && node.m_pattern.m_data.is_Any() && node.m_pattern.m_binding.m_type == ::HIR::PatternBinding::Type::Move )
                {
                    m_builder.push_stmt_assign( node.span(), m_builder.get_variable(node.span(), node.m_pattern.m_binding.m_slot),  mv$(res) );
                }
                else
                {
                    this->destructure_from(node.span(), node.m_pattern, m_builder.lvalue_or_temp(node.m_value->span(), node.m_type, mv$(res)));
                }
            }
            m_builder.set_result(node.span(), ::MIR::RValue::make_Tuple({}));
        }
        void visit(::HIR::ExprNode_Loop& node) override
        {
            TRACE_FUNCTION_FR("_Loop", "_Loop");
            auto loop_block = m_builder.new_bb_linked();
            auto loop_body_scope = m_builder.new_scope_loop(node.span());
            auto loop_next = m_builder.new_bb_unlinked();

            auto loop_result_lvaue = m_builder.new_temporary(node.m_res_type);

            auto loop_tmp_scope = m_builder.new_scope_temp(node.span());
            auto _ = save_and_edit(m_stmt_scope, &loop_tmp_scope);

            m_loop_stack.push_back( LoopDesc { mv$(loop_body_scope), node.m_label, node.m_require_label, loop_block, loop_next, loop_result_lvaue.clone() } );
            this->visit_node_ptr(node.m_code);
            auto loop_scope = mv$(m_loop_stack.back().scope);
            m_loop_stack.pop_back();

            // If there's a stray result, drop it
            if( m_builder.has_result() ) {
                assert( m_builder.block_active() );
                // TODO: Properly drop this? Or just discard it? It should be ()
                m_builder.get_result(node.span());
            }
            // Terminate block with a jump back to the start
            // - Also inserts the jump if this didn't uncondtionally diverge
            if( m_builder.block_active() )
            {
                DEBUG("- Reached end, loop back");
                // Insert drop of all scopes within the current scope
                m_builder.terminate_scope( node.span(), mv$(loop_tmp_scope) );
                m_builder.terminate_scope( node.span(), mv$(loop_scope) );
                m_builder.end_block( ::MIR::Terminator::make_Goto(loop_block) );
            }
            else
            {
                // Terminate scope without emitting cleanup (cleanup was handled by `break`)
                m_builder.terminate_scope( node.span(), mv$(loop_tmp_scope), false );
                m_builder.terminate_scope( node.span(), mv$(loop_scope), false );
            }

            if( ! node.m_diverges )
            {
                DEBUG("- Doesn't diverge");
                m_builder.set_cur_block(loop_next);
                m_builder.set_result(node.span(), mv$(loop_result_lvaue));
            }
            else
            {
                DEBUG("- Diverges");
                assert( !m_builder.has_result() );

                m_builder.set_cur_block(loop_next);
                m_builder.end_split_arm_early(node.span());
                assert( !m_builder.has_result() );
                m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );
            }

            // TODO: Store the variable state on a break for restoration at the end of the loop.
        }

        /// Locate a loop given a name
        const LoopDesc& find_loop(const Span& sp, const RcString& target_label) const
        {
            if( target_label != "" ) {
                auto it = ::std::find_if(m_loop_stack.rbegin(), m_loop_stack.rend(), [&](const auto& x){ return x.label == target_label; });
                if( it == m_loop_stack.rend() ) {
                    BUG(sp, "Named loop '" << target_label << " doesn't exist");
                }
                return *it;
            }
            else {
                auto it = ::std::find_if(m_loop_stack.rbegin(), m_loop_stack.rend(), [](const auto& x){ return !x.require_label; });
                if( it == m_loop_stack.rend() ) {
                    BUG(sp, "Break outside of a breakable block");
                }
                if( it->label != "" && it->label.c_str()[0] == '#' ) {
                    TODO(sp, "Break within try block, want to break parent loop instead");
                }
                return *it;
            }
        }

        void visit(::HIR::ExprNode_LoopControl& node) override
        {
            TRACE_FUNCTION_F("_LoopControl \"" << node.m_label << "\"");
            if( m_loop_stack.size() == 0 ) {
                BUG(node.span(), "Loop control outside of a loop");
            }

            // Visit value before looking up the loop (loop stack may be manipulated during the inner visit)
            if( node.m_value )
            {
                ASSERT_BUG(node.span(), !node.m_continue, "Continue with a value isn't valid");
                DEBUG("break value;");
                this->visit_node_ptr(node.m_value);
            }

            const LoopDesc& target_block = this->find_loop(node.span(), node.m_label);

            if( node.m_continue ) {
                m_builder.terminate_scope_early( node.span(), target_block.scope, /*loop_exit=*/false );
                m_builder.end_block( ::MIR::Terminator::make_Goto(target_block.cur) );
            }
            else {
                if( node.m_value )
                {
                    m_builder.push_stmt_assign( node.span(), target_block.res_value.clone(),  m_builder.get_result(node.span()) );
                }
                else
                {
                    // Set result to ()
                    m_builder.push_stmt_assign( node.span(), target_block.res_value.clone(), ::MIR::RValue::make_Tuple({{}}) );
                }
                m_builder.terminate_scope_early( node.span(), target_block.scope, /*loop_exit=*/true );
                m_builder.end_block( ::MIR::Terminator::make_Goto(target_block.next) );
            }
        }

        void visit(::HIR::ExprNode_Match& node) override
        {
            TRACE_FUNCTION_FR("_Match", "_Match");
            auto _ = save_and_edit(m_borrow_raise_target, nullptr);
            //auto stmt_scope = m_builder.new_scope_temp(node.span());
            this->visit_node_ptr(node.m_value);
            auto match_val = m_builder.get_result_in_lvalue(node.m_value->span(), node.m_value->m_res_type);

            if( node.m_arms.size() == 0 ) {
                // Nothing
                //const auto& ty = node.m_value->m_res_type;
                // TODO: Ensure that the type is a zero-variant enum or !
                m_builder.end_split_arm_early(node.span());
                m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );
                // Push an "diverge" result
                //m_builder.set_cur_block( m_builder.new_bb_unlinked() );
                //m_builder.set_result(node.span(), ::MIR::LValue::make_Invalid({}) );
            }
            else if( node.m_arms.size() == 1 && node.m_arms[0].m_patterns.size() == 1 && ! node.m_arms[0].m_cond ) {
                // - Shortcut: Single-arm match
                auto& arm = node.m_arms[0];
                const auto& pat = arm.m_patterns[0];

                auto scope = m_builder.new_scope_var(arm.m_code->span());
                auto tmp_scope = m_builder.new_scope_temp(arm.m_code->span());
                this->define_vars_from(node.span(), pat);
                // TODO: Do the same shortcut as _Let?
                this->destructure_from(node.span(), pat, mv$(match_val));

                // Temp scope.
                this->visit_node_ptr(arm.m_code);

                if( m_builder.block_active() ) {
                    auto res = m_builder.get_result(arm.m_code->span());
                    m_builder.raise_temporaries( arm.m_code->span(), res, scope, /*to_above=*/true);
                    m_builder.set_result(arm.m_code->span(), mv$(res));

                    m_builder.terminate_scope( node.span(), mv$(tmp_scope) );
                    m_builder.terminate_scope( node.span(), mv$(scope) );
                }
                else {
                    m_builder.terminate_scope( node.span(), mv$(tmp_scope), false );
                    m_builder.terminate_scope( node.span(), mv$(scope), false );
                }
            }
            else {
                MIR_LowerHIR_Match(m_builder, *this, node, mv$(match_val));
            }

            if( m_builder.block_active() ) {
                const auto& sp = node.span();

                auto res = m_builder.get_result(sp);
                //m_builder.raise_variables(sp, res, stmt_scope, /*to_above=*/true);
                m_builder.set_result(sp, mv$(res));

                //m_builder.terminate_scope( node.span(), mv$(stmt_scope) );
            }
            else {
                //m_builder.terminate_scope( node.span(), mv$(stmt_scope), false );
            }
        } // ExprNode_Match

        void emit_if(/*const*/ ::HIR::ExprNodeP& cond, ::MIR::BasicBlockId true_branch, ::MIR::BasicBlockId false_branch)
        {
            TRACE_FUNCTION_F("true=bb" << true_branch <<", false=bb" << false_branch);
            auto* cond_p = &cond;

            // - Convert ! into a reverse of the branches
            {
                bool reverse = false;
                while( auto* cond_uni = dynamic_cast<::HIR::ExprNode_UniOp*>(cond_p->get()) )
                {
                    ASSERT_BUG(cond_uni->span(), cond_uni->m_op == ::HIR::ExprNode_UniOp::Op::Invert, "Unexpected UniOp on boolean in `if` condition");
                    cond_p = &cond_uni->m_value;
                    reverse = !reverse;
                }

                if( reverse )
                {
                    ::std::swap(true_branch, false_branch);
                }
            }

            // Short-circuit && and ||
            if( auto* cond_bin = dynamic_cast<::HIR::ExprNode_BinOp*>(cond_p->get()) )
            {
                switch( cond_bin->m_op )
                {
                case ::HIR::ExprNode_BinOp::Op::BoolAnd: {
                    DEBUG("- Short-circuit BoolAnd");
                    // TODO: Generate a SplitScope

                    // IF left false: go to false immediately
                    auto inner_true_branch = m_builder.new_bb_unlinked();
                    emit_if(cond_bin->m_left, inner_true_branch, false_branch);
                    // ELSE use right
                    m_builder.set_cur_block(inner_true_branch);
                    emit_if(cond_bin->m_right, true_branch, false_branch);
                    } return;
                case ::HIR::ExprNode_BinOp::Op::BoolOr: {
                    DEBUG("- Short-circuit BoolOr");
                    // TODO: Generate a SplitScope

                    // IF left true: got to true
                    auto inner_false_branch = m_builder.new_bb_unlinked();
                    emit_if(cond_bin->m_left, true_branch, inner_false_branch);
                    // ELSE use right
                    m_builder.set_cur_block(inner_false_branch);
                    emit_if(cond_bin->m_right, true_branch, false_branch);
                    } return;
                default:
                    break;
                }
            }

            if( auto* cond_lit = dynamic_cast<::HIR::ExprNode_Literal*>(cond_p->get()) )
            {
                DEBUG("- constant condition");
                if( cond_lit->m_data.as_Boolean() ) {
                    m_builder.end_block( ::MIR::Terminator::make_Goto( true_branch ) );
                }
                else {
                    m_builder.end_block( ::MIR::Terminator::make_Goto( false_branch ) );
                }
                return ;
            }

            // If short-circuiting didn't apply, emit condition
            ::MIR::LValue   decision_val;
            {
                auto scope = m_builder.new_scope_temp( cond->span() );
                this->visit_node_ptr(*cond_p);
                ASSERT_BUG(cond->span(), cond->m_res_type == ::HIR::CoreType::Bool, "If condition wasn't a bool");
                decision_val = m_builder.get_result_in_if_cond(cond->span());
                m_builder.terminate_scope(cond->span(), mv$(scope));
            }

            m_builder.end_block( ::MIR::Terminator::make_If({ mv$(decision_val), true_branch, false_branch }) );
        }

        void visit(::HIR::ExprNode_If& node) override
        {
            TRACE_FUNCTION_FR("_If", "_If");

            auto true_branch = m_builder.new_bb_unlinked();
            auto false_branch = m_builder.new_bb_unlinked();
            emit_if(node.m_cond, true_branch, false_branch);

            auto next_block = m_builder.new_bb_unlinked();
            auto result_val = m_builder.new_temporary(node.m_res_type);

            // Scope handles cases where one arm moves a value but the other doesn't
            auto scope = m_builder.new_scope_split( node.m_true->span() );

            // 'true' branch
            {
                auto stmt_scope = m_builder.new_scope_temp(node.m_true->span());
                m_builder.set_cur_block(true_branch);
                this->visit_node_ptr(node.m_true);
                if( m_builder.block_active() || m_builder.has_result() ) {
                    m_builder.push_stmt_assign( node.span(), result_val.clone(), m_builder.get_result(node.m_true->span()) );
                    m_builder.terminate_scope(node.span(), mv$(stmt_scope));
                    m_builder.end_split_arm(node.span(), scope, true);
                    m_builder.end_block( ::MIR::Terminator::make_Goto(next_block) );
                }
                else {
                    m_builder.terminate_scope(node.span(), mv$(stmt_scope), false);
                    m_builder.end_split_arm(node.span(), scope, false);
                }
            }

            // 'false' branch
            m_builder.set_cur_block(false_branch);
            if( node.m_false )
            {
                auto stmt_scope = m_builder.new_scope_temp(node.m_false->span());
                this->visit_node_ptr(node.m_false);
                if( m_builder.block_active() )
                {
                    m_builder.push_stmt_assign( node.span(), result_val.clone(), m_builder.get_result(node.m_false->span()) );
                    m_builder.terminate_scope(node.span(), mv$(stmt_scope));
                    m_builder.end_split_arm(node.span(), scope, true);
                    m_builder.end_block( ::MIR::Terminator::make_Goto(next_block) );
                }
                else {
                    m_builder.terminate_scope(node.span(), mv$(stmt_scope), false);
                    m_builder.end_split_arm(node.span(), scope, false);
                }
            }
            else
            {
                // Assign `()` to the result
                m_builder.push_stmt_assign(node.span(),  result_val.clone(), ::MIR::RValue::make_Tuple({}) );
                m_builder.end_split_arm(node.span(), scope, true);
                m_builder.end_block( ::MIR::Terminator::make_Goto(next_block) );
            }
            m_builder.set_cur_block(next_block);
            m_builder.terminate_scope( node.span(), mv$(scope) );

            m_builder.set_result( node.span(), mv$(result_val) );
        }

        void generate_checked_binop(const Span& sp, ::MIR::LValue res_slot, ::MIR::eBinOp op, ::MIR::Param val_l, const ::HIR::TypeRef& ty_l, ::MIR::Param val_r, const ::HIR::TypeRef& ty_r)
        {
            switch(op)
            {
            case ::MIR::eBinOp::EQ: case ::MIR::eBinOp::NE:
            case ::MIR::eBinOp::LT: case ::MIR::eBinOp::LE:
            case ::MIR::eBinOp::GT: case ::MIR::eBinOp::GE:
                ASSERT_BUG(sp, ty_l == ty_r, "Types in comparison operators must be equal - " << ty_l << " != " << ty_r);
                // Defensive assert that the type is a valid MIR comparison
                TU_MATCH_HDRA( (ty_l.data()), {)
                default:
                    BUG(sp, "Invalid type in comparison - " << ty_l);
                TU_ARMA(Pointer, e) {
                    // Valid
                    }
                // TODO: Should straight comparisons on &str be supported here?
                TU_ARMA(Primitive, e) {
                    if( e == ::HIR::CoreType::Str ) {
                        BUG(sp, "Invalid type in comparison - " << ty_l);
                    }
                    }
                }
                m_builder.push_stmt_assign(sp, mv$(res_slot), ::MIR::RValue::make_BinOp({ mv$(val_l), op, mv$(val_r) }));
                break;
            // Bitwise masking operations: Require equal integer types or bool
            case ::MIR::eBinOp::BIT_XOR:
            case ::MIR::eBinOp::BIT_OR :
            case ::MIR::eBinOp::BIT_AND:
                ASSERT_BUG(sp, ty_l == ty_r, "Types in bitwise operators must be equal - " << ty_l << " != " << ty_r);
                ASSERT_BUG(sp, ty_l.data().is_Primitive(), "Only primitives allowed in bitwise operators");
                switch(ty_l.data().as_Primitive())
                {
                case ::HIR::CoreType::Str:
                case ::HIR::CoreType::Char:
                case ::HIR::CoreType::F32:
                case ::HIR::CoreType::F64:
                    BUG(sp, "Invalid type for bitwise operator - " << ty_l);
                default:
                    break;
                }
                m_builder.push_stmt_assign(sp, mv$(res_slot), ::MIR::RValue::make_BinOp({ mv$(val_l), op, mv$(val_r) }));
                break;
            case ::MIR::eBinOp::ADD:    case ::MIR::eBinOp::ADD_OV:
            case ::MIR::eBinOp::SUB:    case ::MIR::eBinOp::SUB_OV:
            case ::MIR::eBinOp::MUL:    case ::MIR::eBinOp::MUL_OV:
            case ::MIR::eBinOp::DIV:    case ::MIR::eBinOp::DIV_OV:
            case ::MIR::eBinOp::MOD:
                ASSERT_BUG(sp, ty_l == ty_r, "Types in arithmatic operators must be equal - " << ty_l << " != " << ty_r);
                ASSERT_BUG(sp, ty_l.data().is_Primitive(), "Only primitives allowed in arithmatic operators");
                switch(ty_l.data().as_Primitive())
                {
                case ::HIR::CoreType::Str:
                case ::HIR::CoreType::Char:
                case ::HIR::CoreType::Bool:
                    BUG(sp, "Invalid type for arithmatic operator - " << ty_l);
                default:
                    break;
                }
                // TODO: Overflow checks (none for eBinOp::MOD)
                m_builder.push_stmt_assign(sp, mv$(res_slot), ::MIR::RValue::make_BinOp({ mv$(val_l), op, mv$(val_r) }));
                break;
            case ::MIR::eBinOp::BIT_SHL:
            case ::MIR::eBinOp::BIT_SHR:
                ;
                ASSERT_BUG(sp, ty_l.data().is_Primitive(), "Only primitives allowed in arithmatic operators");
                ASSERT_BUG(sp, ty_r.data().is_Primitive(), "Only primitives allowed in arithmatic operators");
                switch(ty_l.data().as_Primitive())
                {
                case ::HIR::CoreType::Str:
                case ::HIR::CoreType::Char:
                case ::HIR::CoreType::F32:
                case ::HIR::CoreType::F64:
                    BUG(sp, "Invalid type for shift op-assignment - " << ty_l);
                default:
                    break;
                }
                switch(ty_r.data().as_Primitive())
                {
                case ::HIR::CoreType::Str:
                case ::HIR::CoreType::Char:
                case ::HIR::CoreType::F32:
                case ::HIR::CoreType::F64:
                    BUG(sp, "Invalid type for shift op-assignment - " << ty_r);
                default:
                    break;
                }
                // TODO: Overflow check
                m_builder.push_stmt_assign(sp, mv$(res_slot), ::MIR::RValue::make_BinOp({ mv$(val_l), op, mv$(val_r) }));
                break;
            }
        }

        void visit(::HIR::ExprNode_Assign& node) override
        {
            TRACE_FUNCTION_F("_Assign");
            const auto& sp = node.span();

            this->visit_node_ptr(node.m_value);
            ::MIR::RValue val = m_builder.get_result(sp);

            this->visit_node_ptr(node.m_slot);
            auto dst = m_builder.get_result_unwrap_lvalue(sp);

            const auto& ty_slot = node.m_slot->m_res_type;
            const auto& ty_val  = node.m_value->m_res_type;

            if( node.m_op != ::HIR::ExprNode_Assign::Op::None )
            {
                auto dst_clone = dst.clone();
                ::MIR::Param    val_p;
                if( auto* e = val.opt_Use() ) {
                    val_p = mv$(*e);
                }
                else if( auto* e = val.opt_Constant() ) {
                    val_p = mv$(*e);
                }
                else {
                    val_p = m_builder.lvalue_or_temp( node.span(), ty_val, mv$(val) );
                }

                ASSERT_BUG(sp, ty_slot.data().is_Primitive(), "Assignment operator overloads are only valid on primitives - ty_slot="<<ty_slot);
                ASSERT_BUG(sp, ty_val.data().is_Primitive(), "Assignment operator overloads are only valid on primitives - ty_val="<<ty_val);

                #define _(v)    ::HIR::ExprNode_Assign::Op::v
                ::MIR::eBinOp   op;
                switch(node.m_op)
                {
                case _(None):  throw "";
                case _(Add): op = ::MIR::eBinOp::ADD; if(0)
                case _(Sub): op = ::MIR::eBinOp::SUB; if(0)
                case _(Mul): op = ::MIR::eBinOp::MUL; if(0)
                case _(Div): op = ::MIR::eBinOp::DIV; if(0)
                case _(Mod): op = ::MIR::eBinOp::MOD;
                    this->generate_checked_binop(sp, mv$(dst), op, mv$(dst_clone), ty_slot,  mv$(val_p), ty_val);
                    break;
                case _(Xor): op = ::MIR::eBinOp::BIT_XOR; if(0)
                case _(Or ): op = ::MIR::eBinOp::BIT_OR ; if(0)
                case _(And): op = ::MIR::eBinOp::BIT_AND;
                    this->generate_checked_binop(sp, mv$(dst), op, mv$(dst_clone), ty_slot,  mv$(val_p), ty_val);
                    break;
                case _(Shl): op = ::MIR::eBinOp::BIT_SHL; if(0)
                case _(Shr): op = ::MIR::eBinOp::BIT_SHR;
                    this->generate_checked_binop(sp, mv$(dst), op, mv$(dst_clone), ty_slot,  mv$(val_p), ty_val);
                    break;
                }
                #undef _
            }
            else
            {
                ASSERT_BUG(sp, ty_slot == ty_val, "Types must match for assignment - " << ty_slot << " != " << ty_val);
                m_builder.push_stmt_assign(node.span(), mv$(dst), mv$(val));
            }
            m_builder.set_result(node.span(), ::MIR::RValue::make_Tuple({}));
        }

        void visit(::HIR::ExprNode_BinOp& node) override
        {
            const auto& sp = node.span();
            TRACE_FUNCTION_F("_BinOp");

            const auto& ty_l = node.m_left->m_res_type;
            const auto& ty_r = node.m_right->m_res_type;
            auto res = m_builder.new_temporary(node.m_res_type);

            // Short-circuiting boolean operations
            if( node.m_op == ::HIR::ExprNode_BinOp::Op::BoolAnd || node.m_op == ::HIR::ExprNode_BinOp::Op::BoolOr )
            {

                DEBUG("- ShortCircuit Left");
                this->visit_node_ptr(node.m_left);
                auto left = m_builder.get_result_in_lvalue(node.m_left->span(), ty_l);

                auto bb_next = m_builder.new_bb_unlinked();
                auto bb_true = m_builder.new_bb_unlinked();
                auto bb_false = m_builder.new_bb_unlinked();
                m_builder.end_block( ::MIR::Terminator::make_If({ mv$(left), bb_true, bb_false }) );

                // Generate a SplitScope to handle the conditional nature of the next code
                auto split_scope = m_builder.new_scope_split(node.span());

                if( node.m_op == ::HIR::ExprNode_BinOp::Op::BoolOr )
                {
                    DEBUG("- ShortCircuit ||");
                    // If left is true, assign result true and return
                    m_builder.set_cur_block( bb_true );
                    m_builder.push_stmt_assign(node.span(), res.clone(), ::MIR::RValue( ::MIR::Constant::make_Bool({true}) ));
                    m_builder.end_split_arm(node.m_left->span(), split_scope, /*reachable=*/true);
                    m_builder.end_block( ::MIR::Terminator::make_Goto(bb_next) );

                    // If left is false, assign result to right
                    m_builder.set_cur_block( bb_false );
                }
                else
                {
                    DEBUG("- ShortCircuit &&");
                    // If left is false, assign result false and return
                    m_builder.set_cur_block( bb_false );
                    m_builder.push_stmt_assign(node.span(), res.clone(), ::MIR::RValue( ::MIR::Constant::make_Bool({false}) ));
                    m_builder.end_split_arm(node.m_left->span(), split_scope, /*reachable=*/true);
                    m_builder.end_block( ::MIR::Terminator::make_Goto(bb_next) );

                    // If left is true, assign result to right
                    m_builder.set_cur_block( bb_true );
                }

                DEBUG("- ShortCircuit Right");
                auto tmp_scope = m_builder.new_scope_temp(node.m_right->span());
                this->visit_node_ptr(node.m_right);
                m_builder.push_stmt_assign(node.span(), res.clone(), m_builder.get_result(node.m_right->span()));
                m_builder.terminate_scope(node.m_right->span(), mv$(tmp_scope));

                m_builder.end_split_arm(node.m_right->span(), split_scope, /*reachable=*/true);
                m_builder.end_block( ::MIR::Terminator::make_Goto(bb_next) );

                m_builder.set_cur_block( bb_next );
                m_builder.terminate_scope(node.span(), mv$(split_scope));
                m_builder.set_result( node.span(), mv$(res) );
                return ;
            }
            else
            {
            }

            this->visit_node_ptr(node.m_left);
            auto left = m_builder.get_result_in_param(node.m_left->span(), ty_l);
            this->visit_node_ptr(node.m_right);
            auto right = m_builder.get_result_in_param(node.m_right->span(), ty_r);

            ::MIR::eBinOp   op;
            switch(node.m_op)
            {
            case ::HIR::ExprNode_BinOp::Op::CmpEqu: op = ::MIR::eBinOp::EQ; if(0)
            case ::HIR::ExprNode_BinOp::Op::CmpNEqu:op = ::MIR::eBinOp::NE; if(0)
            case ::HIR::ExprNode_BinOp::Op::CmpLt:  op = ::MIR::eBinOp::LT; if(0)
            case ::HIR::ExprNode_BinOp::Op::CmpLtE: op = ::MIR::eBinOp::LE; if(0)
            case ::HIR::ExprNode_BinOp::Op::CmpGt:  op = ::MIR::eBinOp::GT; if(0)
            case ::HIR::ExprNode_BinOp::Op::CmpGtE: op = ::MIR::eBinOp::GE;
                this->generate_checked_binop(sp, res.clone(), op, mv$(left), ty_l, mv$(right), ty_r);
                break;

            case ::HIR::ExprNode_BinOp::Op::Xor: op = ::MIR::eBinOp::BIT_XOR; if(0)
            case ::HIR::ExprNode_BinOp::Op::Or : op = ::MIR::eBinOp::BIT_OR ; if(0)
            case ::HIR::ExprNode_BinOp::Op::And: op = ::MIR::eBinOp::BIT_AND;
                this->generate_checked_binop(sp, res.clone(), op, mv$(left), ty_l, mv$(right), ty_r);
                break;

            case ::HIR::ExprNode_BinOp::Op::Shr: op = ::MIR::eBinOp::BIT_SHR; if(0)
            case ::HIR::ExprNode_BinOp::Op::Shl: op = ::MIR::eBinOp::BIT_SHL;
                this->generate_checked_binop(sp, res.clone(), op, mv$(left), ty_l, mv$(right), ty_r);
                break;

            case ::HIR::ExprNode_BinOp::Op::Add:    op = ::MIR::eBinOp::ADD; if(0)
            case ::HIR::ExprNode_BinOp::Op::Sub:    op = ::MIR::eBinOp::SUB; if(0)
            case ::HIR::ExprNode_BinOp::Op::Mul:    op = ::MIR::eBinOp::MUL; if(0)
            case ::HIR::ExprNode_BinOp::Op::Div:    op = ::MIR::eBinOp::DIV; if(0)
            case ::HIR::ExprNode_BinOp::Op::Mod:    op = ::MIR::eBinOp::MOD;
                this->generate_checked_binop(sp, res.clone(), op, mv$(left), ty_l, mv$(right), ty_r);
                break;

            // Short-circuiting boolean operations
            case ::HIR::ExprNode_BinOp::Op::BoolAnd:
            case ::HIR::ExprNode_BinOp::Op::BoolOr:
                BUG(node.span(), "");
                break;
            }
            m_builder.set_result( node.span(), mv$(res) );
        }

        void visit(::HIR::ExprNode_UniOp& node) override
        {
            TRACE_FUNCTION_F("_UniOp");

            const auto& ty_val = node.m_value->m_res_type;
            this->visit_node_ptr(node.m_value);
            auto val = m_builder.get_result_in_lvalue(node.m_value->span(), ty_val);

            ::MIR::RValue   res;
            switch(node.m_op)
            {
            case ::HIR::ExprNode_UniOp::Op::Invert:
                if( ty_val.data().is_Primitive() ) {
                    switch( ty_val.data().as_Primitive() )
                    {
                    case ::HIR::CoreType::Str:
                    case ::HIR::CoreType::Char:
                    case ::HIR::CoreType::F32:
                    case ::HIR::CoreType::F64:
                        BUG(node.span(), "`!` operator on invalid type - " << ty_val);
                        break;
                    default:
                        break;
                    }
                }
                else {
                    BUG(node.span(), "`!` operator on invalid type - " << ty_val);
                }
                res = ::MIR::RValue::make_UniOp({ mv$(val), ::MIR::eUniOp::INV });
                break;
            case ::HIR::ExprNode_UniOp::Op::Negate:
                if( ty_val.data().is_Primitive() ) {
                    switch( ty_val.data().as_Primitive() )
                    {
                    case ::HIR::CoreType::Str:
                    case ::HIR::CoreType::Char:
                    case ::HIR::CoreType::Bool:
                        BUG(node.span(), "`-` operator on invalid type - " << ty_val);
                        break;
                    case ::HIR::CoreType::U8:
                    case ::HIR::CoreType::U16:
                    case ::HIR::CoreType::U32:
                    case ::HIR::CoreType::U64:
                    case ::HIR::CoreType::U128:
                    case ::HIR::CoreType::Usize:
                        BUG(node.span(), "`-` operator on unsigned integer - " << ty_val);
                        break;
                    default:
                        break;
                    }
                }
                else {
                    BUG(node.span(), "`!` operator on invalid type - " << ty_val);
                }
                res = ::MIR::RValue::make_UniOp({ mv$(val), ::MIR::eUniOp::NEG });
                break;
            }
            m_builder.set_result( node.span(), mv$(res) );
        }
        void visit(::HIR::ExprNode_Borrow& node) override
        {
            TRACE_FUNCTION_F("_Borrow");

            auto _ = save_and_edit(m_in_borrow, true);

            const auto& ty_val = node.m_value->m_res_type;
            this->visit_node_ptr(node.m_value);
            auto val = m_builder.get_result_in_lvalue(node.m_value->span(), ty_val);

            if( m_borrow_raise_target )
            {
                DEBUG("- Raising borrow to scope " << *m_borrow_raise_target);
                m_builder.raise_temporaries(node.span(), val, *m_borrow_raise_target);
            }

            m_builder.set_result( node.span(), ::MIR::RValue::make_Borrow({ node.m_type, mv$(val) }) );
        }
        void visit(::HIR::ExprNode_Cast& node) override
        {
            TRACE_FUNCTION_F("_Cast " << node.m_res_type);
            this->visit_node_ptr(node.m_value);

            const auto& ty_out = node.m_res_type;
            const auto& ty_in = node.m_value->m_res_type;

            // TODO: The correct behavior is to do the cast (into a rvalue) no matter what.
            // See test run-pass/issue-36936
            if( ty_out == ty_in ) {
                return ;
            }

            auto val = m_builder.get_result_in_lvalue(node.m_value->span(), node.m_value->m_res_type);

            TU_MATCH_HDRA( (ty_out.data()), {)
            default:
                BUG(node.span(), "Invalid cast to " << ty_out << " from " << ty_in);
            TU_ARMA(Function, de) {
                // Just trust the previous stages.
                ASSERT_BUG(node.span(), ty_in.data().is_Function(), ty_in);
                ASSERT_BUG(node.span(), de.m_arg_types == ty_in.data().as_Function().m_arg_types, ty_in);
                }
            TU_ARMA(Pointer, de) {
                if( ty_in.data().is_Primitive() ) {
                    const auto& ie = ty_in.data().as_Primitive();
                    switch(ie)
                    {
                    case ::HIR::CoreType::Bool:
                    case ::HIR::CoreType::Char:
                    case ::HIR::CoreType::Str:
                    case ::HIR::CoreType::F32:
                    case ::HIR::CoreType::F64:
                        BUG(node.span(), "Cannot cast to pointer from " << ty_in);
                    default:
                        break;
                    }
                    // TODO: Only valid if T: Sized in *{const/mut/move} T
                }
                else if(const auto* se = ty_in.data().opt_Borrow() )
                {
                    if( de.inner != se->inner ) {
                        BUG(node.span(), "Cannot cast to " << ty_out << " from " << ty_in);
                    }
                    // Valid
                }
                else if( const auto* se = ty_in.data().opt_Function() )
                {
                    if( de.inner != ::HIR::TypeRef::new_unit() && de.inner != ::HIR::CoreType::U8 && de.inner != ::HIR::CoreType::I8 ) {
                        BUG(node.span(), "Cannot cast to " << ty_out << " from " << ty_in);
                    }
                    // Valid
                }
                else if( ty_in.data().is_Pointer() )
                {
                    // Valid
                }
                else {
                    BUG(node.span(), "Cannot cast to pointer from " << ty_in);
                }
                }
            TU_ARMA(Primitive, de) {
                switch(de)
                {
                case ::HIR::CoreType::Str:
                    BUG(node.span(), "Cannot cast to str");
                    break;
                case ::HIR::CoreType::Char:
                    if( ty_in.data().is_Primitive() && ty_in.data().as_Primitive() == ::HIR::CoreType::U8 ) {
                        // Valid
                    }
                    else {
                        BUG(node.span(), "Cannot cast to char from " << ty_in);
                    }
                    break;
                case ::HIR::CoreType::Bool:
                    BUG(node.span(), "Cannot cast to bool");
                    break;
                case ::HIR::CoreType::F32:
                case ::HIR::CoreType::F64:
                    if(ty_in.data().is_Primitive())
                    {
                        switch(de)
                        {
                        case ::HIR::CoreType::Str:
                        case ::HIR::CoreType::Char:
                        case ::HIR::CoreType::Bool:
                            BUG(node.span(), "Cannot cast to " << ty_out << " from " << ty_in);
                            break;
                        default:
                            // Valid
                            break;
                        }
                    }
                    else {
                        BUG(node.span(), "Cannot cast to " << ty_out << " from " << ty_in);
                    }
                    break;
                default:
                    if(ty_in.data().opt_Primitive())
                    {
                        switch(de)
                        {
                        case ::HIR::CoreType::Str:
                            BUG(node.span(), "Cannot cast to " << ty_out << " from " << ty_in);
                        default:
                            // Valid
                            break;
                        }
                    }
                    else if( const auto* se = ty_in.data().opt_Path() )
                    {
                        if( se->binding.is_Enum() )
                        {
                            // TODO: Check if it's a repr(ty/C) enum - and if the type matches
                        }
                        else {
                            BUG(node.span(), "Cannot cast to " << ty_out << " from " << ty_in);
                        }
                    }
                    // NOTE: Valid for all integer types
                    else if( ty_in.data().is_Pointer() ) {
                        // TODO: Only valid for T: Sized?
                    }
                    else if( de == ::HIR::CoreType::Usize && ty_in.data().is_Function() ) {
                        // TODO: Always valid?
                    }
                    else {
                        BUG(node.span(), "Cannot cast to " << ty_out << " from " << ty_in);
                    }
                    break;
                }
                }
            }
            auto res = m_builder.new_temporary(node.m_res_type);
            m_builder.push_stmt_assign(node.span(), res.clone(), ::MIR::RValue::make_Cast({ mv$(val), node.m_res_type.clone() }));
            m_builder.set_result( node.span(), mv$(res) );
        }
        void visit(::HIR::ExprNode_Unsize& node) override
        {
            TRACE_FUNCTION_F("_Unsize");
            this->visit_node_ptr(node.m_value);

            const auto& ty_out = node.m_res_type;
            const auto& ty_in = node.m_value->m_res_type;

            if( ty_out == ty_in ) {
                return ;
            }

            auto ptr_lval = m_builder.get_result_in_lvalue(node.m_value->span(), node.m_value->m_res_type);

            if( ty_out.data().is_Borrow() && ty_in.data().is_Borrow() )
            {
                const auto& oe = ty_out.data().as_Borrow();
                const auto& ie = ty_in.data().as_Borrow();
                const auto& ty_out = oe.inner;
                const auto& ty_in = ie.inner;
                TU_MATCH_HDRA( (ty_out.data()), {)
                default: {
                    const auto& lang_Unsize = m_builder.crate().get_lang_item_path(node.span(), "unsize");
                    if( m_builder.resolve().find_impl( node.span(), lang_Unsize, ::HIR::PathParams(ty_out.clone()), ty_in.clone(), [](auto , bool ){ return true; }) )
                    {
                        // - HACK: Emit a cast operation on the pointers. Leave it up to monomorph to 'fix' it
                        m_builder.set_result( node.span(), ::MIR::RValue::make_Cast({ mv$(ptr_lval), node.m_res_type.clone() }) );
                    }
                    else
                    {
                        // Probably an error?
                        m_builder.set_result( node.span(), ::MIR::RValue::make_Cast({ mv$(ptr_lval), node.m_res_type.clone() }) );
                        //TODO(node.span(), "MIR _Unsize to " << ty_out);
                    }
                    }
                TU_ARMA(Slice, e) {
                    if( ty_in.data().is_Array() )
                    {
                        const auto& in_array = ty_in.data().as_Array();
                        ::MIR::Constant size_val;
                        TU_MATCH_HDRA( (in_array.size), {)
                        default:
                            BUG(node.span(), "Unsize Array with unknown size " << ty_in);
                        TU_ARMA(Generic, se) {
                            size_val = se;
                            }
                        TU_ARMA(Known, se) {
                            size_val = ::MIR::Constant::make_Uint({ se, ::HIR::CoreType::Usize });
                            }
                        }
                        m_builder.set_result( node.span(), ::MIR::RValue::make_MakeDst({ mv$(ptr_lval), mv$(size_val) }) );
                    }
                    else if( ty_in.data().is_Generic() || (ty_in.data().is_Path() && ty_in.data().as_Path().binding.is_Opaque()) )
                    {
                        // HACK: FixedSizeArray uses `A: Unsize<[T]>` which will lead to the above code not working (as the size isn't known).
                        // - Maybe _Meta on the `&A` would work as a stopgap (since A: Sized, it won't collide with &[T] or similar)
                        auto size_lval = m_builder.lvalue_or_temp( node.span(), ::HIR::TypeRef(::HIR::CoreType::Usize), ::MIR::RValue::make_DstMeta({ ptr_lval.clone() }) );
                        m_builder.set_result( node.span(), ::MIR::RValue::make_MakeDst({ mv$(ptr_lval), mv$(size_lval) }) );
                    }
                    else
                    {
                        ASSERT_BUG(node.span(), ty_in.data().is_Array(), "Unsize to slice from non-array - " << ty_in);
                    }
                    }
                TU_ARMA(TraitObject, e) {
                    m_builder.set_result( node.span(), ::MIR::RValue::make_Cast({ mv$(ptr_lval), node.m_res_type.clone() }) );
                    }
                }
            }
            else
            {
                // NOTES: (from IRC: eddyb)
                // < eddyb> they're required that T and U are the same struct definition (with different type parameters) and exactly one field differs in type between T and U (ignoring PhantomData)
                // < eddyb> Mutabah: I forgot to mention that the field that differs in type must also impl CoerceUnsized

                // TODO: Just emit a cast and leave magic handling to codegen
                // - This code _could_ do inspection of the types and insert a destructure+unsize+restructure, but that does't handle direct `T: CoerceUnsize<U>`
                m_builder.set_result( node.span(), ::MIR::RValue::make_Cast({ mv$(ptr_lval), node.m_res_type.clone() }) );
            }
        }
        void visit(::HIR::ExprNode_Index& node) override
        {
            TRACE_FUNCTION_F("_Index");

            // NOTE: Calculate the index first (so if it borrows from the source, it's over by the time that's needed)
            const auto& ty_idx = node.m_index->m_res_type;
            this->visit_node_ptr(node.m_index);
            auto index = m_builder.get_result_in_lvalue(node.m_index->span(), ty_idx);

            const auto& ty_val = node.m_value->m_res_type;
            this->visit_node_ptr(node.m_value);
            auto value = m_builder.get_result_in_lvalue(node.m_value->span(), ty_val);

            ::MIR::RValue   limit_val;
            TU_MATCH_HDRA( (ty_val.data()), {)
            default:
                BUG(node.span(), "Indexing unsupported type " << ty_val);
            TU_ARMA(Array, e) {
                TU_MATCH_HDRA( (e.size), {)
                default:
                    BUG(node.span(), "Indexing with unknown size" << ty_val);
                TU_ARMA(Known, se) {
                    limit_val = ::MIR::Constant::make_Uint({ se, ::HIR::CoreType::Usize });
                    }
                }
                }
            TU_ARMA(Slice, e) {
                limit_val = ::MIR::RValue::make_DstMeta({ m_builder.get_ptr_to_dst(node.m_value->span(), value) });
                }
            }

            if( ty_idx != ::HIR::CoreType::Usize )
            {
                BUG(node.span(), "Indexing using unsupported index type " << ty_idx);
            }

            // Range checking (DISABLED)
            if( false )
            {
                auto limit_lval = m_builder.lvalue_or_temp( node.span(), ty_idx, mv$(limit_val) );

                auto cmp_res = m_builder.new_temporary( ::HIR::CoreType::Bool );
                m_builder.push_stmt_assign(node.span(), cmp_res.clone(), ::MIR::RValue::make_BinOp({ index.clone(), ::MIR::eBinOp::GE, mv$(limit_lval) }));
                auto arm_panic = m_builder.new_bb_unlinked();
                auto arm_continue = m_builder.new_bb_unlinked();
                m_builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_res), arm_panic, arm_continue }) );

                m_builder.set_cur_block( arm_panic );
                // TODO: Call an "index fail" method which always panics.
                //m_builder.end_block( ::MIR::Terminator::make_Panic({}) );
                m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );

                m_builder.set_cur_block( arm_continue );
            }

            if( !index.is_Local())
            {
                auto local_idx = m_builder.new_temporary(::HIR::CoreType::Usize);
                m_builder.push_stmt_assign(node.span(), local_idx.clone(), mv$(index));
                index = mv$(local_idx);
            }
            m_builder.set_result( node.span(), ::MIR::LValue::new_Index( mv$(value), index.m_root.as_Local() ) );
        }

        void visit(::HIR::ExprNode_Deref& node) override
        {
            const Span& sp = node.span();
            TRACE_FUNCTION_F("_Deref");

            const auto& ty_val = node.m_value->m_res_type;
            this->visit_node_ptr(node.m_value);
            auto val = m_builder.get_result_in_lvalue(node.m_value->span(), ty_val);

            TU_MATCH_HDRA( (ty_val.data()), {)
            default: {
                if( m_builder.is_type_owned_box( ty_val ) )
                {
                    // Box magically derefs.
                }
                else
                {
                    // TODO: Do operator replacement here after handling scope-raising for _Borrow
                    if( m_borrow_raise_target && m_in_borrow )
                    {
                        DEBUG("- Raising deref in borrow to scope " << *m_borrow_raise_target);
                        m_builder.raise_temporaries(node.span(), val, *m_borrow_raise_target);
                    }


                    const char* langitem = nullptr;
                    const char* method = nullptr;
                    ::HIR::BorrowType   bt;
                    // - Uses the value's usage beacuse for T: Copy node.m_value->m_usage is Borrow, but node.m_usage is Move
                    switch( node.m_value->m_usage )
                    {
                    case ::HIR::ValueUsage::Unknown:
                        BUG(sp, "Unknown usage type of deref value - " << ty_val);
                        break;
                    case ::HIR::ValueUsage::Borrow:
                        bt = ::HIR::BorrowType::Shared;
                        langitem = method = "deref";
                        break;
                    case ::HIR::ValueUsage::Mutate:
                        bt = ::HIR::BorrowType::Unique;
                        langitem = method = "deref_mut";
                        break;
                    case ::HIR::ValueUsage::Move:
                        TODO(sp, "ValueUsage::Move for desugared Deref of " << node.m_value->m_res_type);
                        break;
                    }
                    // Needs replacement, continue
                    assert(langitem);
                    assert(method);

                    // - Construct trait path - Index*<IdxTy>
                    auto method_path = ::HIR::Path(ty_val.clone(), ::HIR::GenericPath(m_builder.resolve().m_crate.get_lang_item_path(node.span(), langitem), {}), method);

                    // Store a borrow of the input value
                    ::std::vector<::MIR::Param>    args;
                    args.push_back( m_builder.lvalue_or_temp(sp,
                                ::HIR::TypeRef::new_borrow(bt, node.m_value->m_res_type.clone()),
                                ::MIR::RValue::make_Borrow({ bt, mv$(val) })
                                ) );
                    m_builder.moved_lvalue(node.span(), args[0].as_LValue());
                    val = m_builder.new_temporary(::HIR::TypeRef::new_borrow(bt, node.m_res_type.clone()));
                    // Call the above trait method
                    // Store result of that call in `val` (which will be derefed below)
                    auto ok_block = m_builder.new_bb_unlinked();
                    auto panic_block = m_builder.new_bb_unlinked();
                    m_builder.end_block(::MIR::Terminator::make_Call({ ok_block, panic_block, val.clone(), mv$(method_path), mv$(args) }));
                    m_builder.set_cur_block(panic_block);
                    m_builder.end_block(::MIR::Terminator::make_Diverge({}));

                    m_builder.set_cur_block(ok_block);
                }
                }
            TU_ARMA(Pointer, te) {
                // Deref on a pointer - TODO: Requires unsafe
                }
            TU_ARMA(Borrow, te) {
                // Deref on a borrow - Always valid... assuming borrowck is there :)
                }
            }

            m_builder.set_result( node.span(), ::MIR::LValue::new_Deref( mv$(val) ) );
        }

        void visit(::HIR::ExprNode_Emplace& node) override
        {
            switch(gTargetVersion)
            {
            case TargetVersion::Rustc1_19:
                return visit_emplace_119(node);
            case TargetVersion::Rustc1_29:
            case TargetVersion::Rustc1_39:
                return visit_emplace_129(node);
            }
            throw "BUG: Unhandled target version";
        }
        void visit_emplace_119(::HIR::ExprNode_Emplace& node)
        {
            if( node.m_type == ::HIR::ExprNode_Emplace::Type::Noop ) {
                return node.m_value->visit(*this);
            }
            const auto& path_Placer = m_builder.crate().get_lang_item_path(node.span(), "placer_trait");
            const auto& path_Boxed = m_builder.crate().get_lang_item_path(node.span(), "boxed_trait");
            const auto& path_Place = m_builder.crate().get_lang_item_path(node.span(), "place_trait");
            const auto& path_BoxPlace = m_builder.crate().get_lang_item_path(node.span(), "box_place_trait");
            const auto& path_InPlace = m_builder.crate().get_lang_item_path(node.span(), "in_place_trait");

            const auto& data_ty = node.m_value->m_res_type;

            ::HIR::PathParams   trait_params_data;
            trait_params_data.m_types.push_back( data_ty.clone() );
            // 1. Obtain the type of the `place` variable
            ::HIR::TypeRef  place_type;
            switch( node.m_type )
            {
            case ::HIR::ExprNode_Emplace::Type::Noop:
                throw "";
            case ::HIR::ExprNode_Emplace::Type::Boxer:
                place_type = ::HIR::TypeRef::new_path( ::HIR::Path(node.m_res_type.clone(), ::HIR::GenericPath(path_Boxed), "Place", {}), {} );
                break;
            case ::HIR::ExprNode_Emplace::Type::Placer:
                place_type = ::HIR::TypeRef::new_path( ::HIR::Path(node.m_place->m_res_type.clone(), ::HIR::GenericPath(path_Placer, trait_params_data.clone()), "Place", {}), {} );
                break;
            }
            m_builder.resolve().expand_associated_types( node.span(), place_type );

            // 2. Initialise the place
            auto place = m_builder.new_temporary( place_type );
            auto place__panic = m_builder.new_bb_unlinked();
            auto place__ok = m_builder.new_bb_unlinked();
            switch( node.m_type )
            {
            case ::HIR::ExprNode_Emplace::Type::Noop:
                throw "";
            case ::HIR::ExprNode_Emplace::Type::Boxer: {
                m_builder.end_block(::MIR::Terminator::make_Call({
                    place__ok, place__panic,
                    place.clone(), ::HIR::Path(place_type.clone(), ::HIR::GenericPath(path_BoxPlace, mv$(trait_params_data)), "make_place", {}),
                    {}
                    }));
                break; }
            case ::HIR::ExprNode_Emplace::Type::Placer: {
                // Visit the place
                node.m_place->visit(*this);
                auto val = m_builder.get_result_in_param(node.m_place->span(), node.m_place->m_res_type);
                if(const auto* e = val.opt_LValue() ) {
                    m_builder.moved_lvalue( node.m_place->span(), *e );
                }
                // Extract the "Place" type
                m_builder.end_block(::MIR::Terminator::make_Call({
                    place__ok, place__panic,
                    place.clone(), ::HIR::Path(node.m_place->m_res_type.clone(), ::HIR::GenericPath(path_Placer, trait_params_data.clone()), "make_place", {}),
                    ::make_vec1( mv$(val) )
                    }));
                break; }
            }

            // TODO: Proper panic handling, including scope destruction
            m_builder.set_cur_block(place__panic);
            //m_builder.terminate_scope_early( node.span(), m_builder.fcn_scope() );
            // TODO: Drop `place`
            m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );
            m_builder.set_cur_block(place__ok);

            // 2. Get `place_raw`
            auto place_raw__type = ::HIR::TypeRef::new_pointer(::HIR::BorrowType::Unique, node.m_value->m_res_type.clone());
            auto place_raw = m_builder.new_temporary( place_raw__type );
            auto place_raw__panic = m_builder.new_bb_unlinked();
            auto place_raw__ok = m_builder.new_bb_unlinked();
            {
                auto place_refmut__type = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Unique, place_type.clone());
                auto place_refmut = m_builder.lvalue_or_temp(node.span(), place_refmut__type,  ::MIR::RValue::make_Borrow({ ::HIR::BorrowType::Unique, place.clone() }));
                // <typeof(place) as ops::Place<T>>::pointer (T = inner)
                auto fcn_path = ::HIR::Path(place_type.clone(), ::HIR::GenericPath(path_Place, ::HIR::PathParams(data_ty.clone())), "pointer");
                m_builder.moved_lvalue(node.span(), place_refmut);
                m_builder.end_block(::MIR::Terminator::make_Call({
                    place_raw__ok, place_raw__panic,
                    place_raw.clone(), mv$(fcn_path),
                    ::make_vec1( ::MIR::Param(mv$(place_refmut)) )
                    }));
            }

            // TODO: Proper panic handling, including scope destruction
            m_builder.set_cur_block(place_raw__panic);
            //m_builder.terminate_scope_early( node.span(), m_builder.fcn_scope() );
            // TODO: Drop `place`
            m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );
            m_builder.set_cur_block(place_raw__ok);


            // 3. Get the value and assign it into `place_raw`
            node.m_value->visit(*this);
            auto val = m_builder.get_result(node.span());
            m_builder.push_stmt_assign( node.span(), ::MIR::LValue::new_Deref(place_raw.clone()), mv$(val), /*drop_destination=*/false );

            // 3. Return a call to `finalize`
            ::HIR::Path  finalize_path(::HIR::GenericPath {});
            switch( node.m_type )
            {
            case ::HIR::ExprNode_Emplace::Type::Noop:
                throw "";
            case ::HIR::ExprNode_Emplace::Type::Boxer:
                finalize_path = ::HIR::Path(node.m_res_type.clone(), ::HIR::GenericPath(path_Boxed), "finalize");
                break;
            case ::HIR::ExprNode_Emplace::Type::Placer:
                finalize_path = ::HIR::Path(place_type.clone(), ::HIR::GenericPath(path_InPlace, trait_params_data.clone()), "finalize");
                break;
            }

            auto res = m_builder.new_temporary( node.m_res_type );
            auto res__panic = m_builder.new_bb_unlinked();
            auto res__ok = m_builder.new_bb_unlinked();
            m_builder.moved_lvalue(node.span(), place);
            m_builder.end_block(::MIR::Terminator::make_Call({
                res__ok, res__panic,
                res.clone(), mv$(finalize_path),
                ::make_vec1( ::MIR::Param(mv$(place)) )
                }));

            // TODO: Proper panic handling, including scope destruction
            m_builder.set_cur_block(res__panic);
            //m_builder.terminate_scope_early( node.span(), m_builder.fcn_scope() );
            // TODO: Should this drop the value written to the rawptr?
            // - No, becuase it's likely invalid now. Goodbye!
            m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );
            m_builder.set_cur_block(res__ok);

            m_builder.mark_value_assigned(node.span(), res);
            m_builder.set_result( node.span(), mv$(res) );
        }
        void visit_emplace_129(::HIR::ExprNode_Emplace& node)
        {
            assert( node.m_type == ::HIR::ExprNode_Emplace::Type::Boxer );
            const auto& data_ty = node.m_value->m_res_type;

            node.m_value->visit(*this);
            auto val = m_builder.get_result(node.span());

            const auto& lang_exchange_malloc = m_builder.crate().get_lang_item_path(node.span(), "exchange_malloc");
            const auto& lang_owned_box = m_builder.crate().get_lang_item_path(node.span(), "owned_box");

            ::HIR::PathParams   trait_params_data;
            trait_params_data.m_types.push_back( data_ty.clone() );

            // 1. Determine the size/alignment of the type
            ::MIR::Param    size_param, align_param;
            size_t  item_size, item_align;
            if( Target_GetSizeAndAlignOf(node.span(), m_builder.resolve(), data_ty, item_size, item_align) ) {
                size_param = ::MIR::Constant::make_Int({ static_cast<int64_t>(item_size), ::HIR::CoreType::Usize });
                align_param = ::MIR::Constant::make_Int({ static_cast<int64_t>(item_align), ::HIR::CoreType::Usize });
            }
            else {
                // Insert calls to "size_of" and "align_of" intrinsics
                auto size_slot = m_builder.new_temporary( ::HIR::CoreType::Usize );
                auto size__panic = m_builder.new_bb_unlinked();
                auto size__ok = m_builder.new_bb_unlinked();
                m_builder.end_block(::MIR::Terminator::make_Call({
                    size__ok, size__panic,
                    size_slot.clone(), ::MIR::CallTarget::make_Intrinsic({ "size_of", trait_params_data.clone() }),
                    {}
                    }));
                m_builder.set_cur_block(size__panic); m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );   // HACK
                m_builder.set_cur_block(size__ok);
                auto align_slot = m_builder.new_temporary( ::HIR::CoreType::Usize );
                auto align__panic = m_builder.new_bb_unlinked();
                auto align__ok = m_builder.new_bb_unlinked();
                m_builder.end_block(::MIR::Terminator::make_Call({
                    align__ok, align__panic,
                    align_slot.clone(), ::MIR::CallTarget::make_Intrinsic({ "align_of", trait_params_data.clone() }),
                    {}
                    }));
                m_builder.set_cur_block(align__panic); m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );   // HACK
                m_builder.set_cur_block(align__ok);

                size_param = ::std::move(size_slot);
                align_param = ::std::move(align_slot);
            }

            // 2. Call the allocator function and get a pointer
            // - NOTE: "exchange_malloc" returns a `*mut u8`, need to cast that to the target type
            auto place_raw_type = ::HIR::TypeRef::new_pointer(::HIR::BorrowType::Unique, ::HIR::CoreType::U8);
            auto place_raw = m_builder.new_temporary( place_raw_type );

            auto place__panic = m_builder.new_bb_unlinked();
            auto place__ok = m_builder.new_bb_unlinked();
            m_builder.end_block(::MIR::Terminator::make_Call({
                place__ok, place__panic,
                place_raw.clone(), ::HIR::Path(lang_exchange_malloc),
                make_vec2<::MIR::Param>( ::std::move(size_param), ::std::move(align_param) )
                }));
            m_builder.set_cur_block(place__panic); m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );   // HACK
            m_builder.set_cur_block(place__ok);

            auto place_type = ::HIR::TypeRef::new_pointer(::HIR::BorrowType::Unique, data_ty.clone());
            auto place = m_builder.new_temporary( place_type );
            m_builder.push_stmt_assign(node.span(), place.clone(), ::MIR::RValue::make_Cast({ mv$(place_raw), place_type.clone() }));
            // 3. Do a non-dropping write into the target location (i.e. just a MIR assignment)
            m_builder.push_stmt_assign(node.span(), ::MIR::LValue::new_Deref(place.clone()), mv$(val), /*drop_destination=*/false);
            // 4. Convert the pointer into an `owned_box`
            auto res_type = ::HIR::TypeRef::new_path(::HIR::GenericPath(lang_owned_box, mv$(trait_params_data)), &m_builder.crate().get_struct_by_path(node.span(), lang_owned_box));
            auto res = m_builder.new_temporary(res_type);
            auto cast__panic = m_builder.new_bb_unlinked();
            auto cast__ok = m_builder.new_bb_unlinked();
            ::HIR::PathParams   transmute_params;
            transmute_params.m_types.push_back( res_type.clone() );
            transmute_params.m_types.push_back( place_type.clone() );
            m_builder.end_block(::MIR::Terminator::make_Call({
                cast__ok, cast__panic,
                res.clone(), ::MIR::CallTarget::make_Intrinsic({ "transmute", mv$(transmute_params) }),
                make_vec1( ::MIR::Param( mv$(place) ) )
                }));
            m_builder.set_cur_block(cast__panic); m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );   // HACK
            m_builder.set_cur_block(cast__ok);

            m_builder.set_result(node.span(), mv$(res));
        }

        void visit(::HIR::ExprNode_TupleVariant& node) override
        {
            const Span& sp = node.span();
            TRACE_FUNCTION_F("_TupleVariant");
            ::std::vector< ::MIR::Param>   values;
            values.reserve( node.m_args.size() );
            for(auto& arg : node.m_args)
            {
                this->visit_node_ptr(arg);
                values.push_back( m_builder.get_result_in_param(arg->span(), arg->m_res_type) );
            }

            if( node.m_is_struct )
            {
                m_builder.set_result( node.span(), ::MIR::RValue::make_Struct({
                    node.m_path.clone(),
                    mv$(values)
                    }) );
            }
            else
            {
                // Get the variant index from the enum.
                auto enum_path = node.m_path.clone();
                enum_path.m_path.m_components.pop_back();
                const auto& var_name = node.m_path.m_path.m_components.back();
                const auto& enm = m_builder.crate().get_enum_by_path(sp, enum_path.m_path);

                size_t idx = enm.find_variant(var_name);
                ASSERT_BUG(sp, idx != SIZE_MAX, "Variant " << node.m_path.m_path << " isn't present");

                // TODO: Validation?
                ASSERT_BUG(sp, enm.m_data.is_Data(), "TupleVariant on non-data enum - " << node.m_path.m_path);

#if 0
                const auto& var_ty = enm.m_data.as_Data()[idx].type;
                // Take advantage of the identical generics to cheaply clone/monomorph the path.
                const auto& str = *var_ty.data().as_Path().binding.as_Struct();
                ::HIR::GenericPath struct_path = node.m_path.clone();
                struct_path.m_path = var_ty.data().as_Path().path.m_data.as_Generic().m_path;

                auto ty = ::HIR::TypeRef::new_path( mv$(struct_path), &str );
                auto v = m_builder.get_result_in_param(node.span(), ty);
#endif

                m_builder.set_result(node.span(), ::MIR::RValue::make_EnumVariant({
                    mv$(enum_path),
                    static_cast<unsigned>(idx),
                    mv$(values)
                    }) );
            }
        }

        ::std::vector< ::MIR::Param> get_args(/*const*/ ::std::vector<::HIR::ExprNodeP>& args)
        {
            ::std::vector< ::MIR::Param>   values;
            values.reserve( args.size() );
            for(auto& arg : args)
            {
                this->visit_node_ptr(arg);
                if( !m_builder.block_active() )
                {
                    auto tmp = m_builder.new_temporary(arg->m_res_type);
                    values.push_back( mv$(tmp) );
                }
                else if( args.size() == 1 )
                {
                    values.push_back( m_builder.get_result_in_param(arg->span(), arg->m_res_type, /*allow_missing_value=*/true) );
                }
                else
                {
                    auto res = m_builder.get_result(arg->span());
                    if( auto* e = res.opt_Constant() )
                    {
                        values.push_back( mv$(*e) );
                    }
                    else
                    {
                        // NOTE: Have to allocate a new temporary because ordering matters
                        auto tmp = m_builder.new_temporary(arg->m_res_type);
                        m_builder.push_stmt_assign( arg->span(), tmp.clone(), mv$(res) );
                        values.push_back( mv$(tmp) );
                    }
                }

                if(const auto* e = values.back().opt_LValue() )
                {
                    m_builder.moved_lvalue( arg->span(), *e );
                }
            }
            return values;
        }

        void visit(::HIR::ExprNode_CallPath& node) override
        {
            TRACE_FUNCTION_F("_CallPath " << node.m_path);
            auto _ = save_and_edit(m_borrow_raise_target, nullptr);
            auto values = get_args(node.m_args);

            auto panic_block = m_builder.new_bb_unlinked();
            auto next_block = m_builder.new_bb_unlinked();
            auto res = m_builder.new_temporary( node.m_res_type );

            bool unconditional_diverge = false;

            // Emit intrinsics as a special call type
            if( node.m_path.m_data.is_Generic() )
            {
                const auto& gpath = node.m_path.m_data.as_Generic();
                const auto& fcn = m_builder.crate().get_function_by_path(node.span(), gpath.m_path);
                if( fcn.m_abi == "rust-intrinsic" )
                {
                    m_builder.end_block(::MIR::Terminator::make_Call({
                        next_block, panic_block,
                        res.clone(), ::MIR::CallTarget::make_Intrinsic({ gpath.m_path.m_components.back(), gpath.m_params.clone() }),
                        mv$(values)
                        }));
                }
                if( fcn.m_abi == "platform-intrinsic" )
                {
                    m_builder.end_block(::MIR::Terminator::make_Call({
                        next_block, panic_block,
                        res.clone(), ::MIR::CallTarget::make_Intrinsic({ RcString(FMT("platform:" << gpath.m_path.m_components.back())), gpath.m_params.clone() }),
                        mv$(values)
                        }));
                }

                // rustc has drop_in_place as a lang item, mrustc uses an intrinsic
                if( gpath.m_path == m_builder.crate().get_lang_item_path_opt("drop_in_place") )
                {
                    m_builder.end_block(::MIR::Terminator::make_Call({
                        next_block, panic_block,
                        res.clone(), ::MIR::CallTarget::make_Intrinsic({ "drop_in_place", gpath.m_params.clone() }),
                        mv$(values)
                        }));
                }

                if( fcn.m_return.data().is_Diverge() )
                {
                    unconditional_diverge = true;
                }
            }
            else
            {
                // TODO: Know if the call unconditionally diverges.
                if( node.m_cache.m_arg_types.back().data().is_Diverge() )
                    unconditional_diverge = true;
            }

            // If the call wasn't to an intrinsic, emit it as a path
            if( m_builder.block_active() )
            {
                m_builder.end_block(::MIR::Terminator::make_Call({
                    next_block, panic_block,
                    res.clone(), node.m_path.clone(),
                    mv$(values)
                    }));
            }

            m_builder.set_cur_block(panic_block);
            // TODO: Proper panic handling, including scope destruction
            m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );

            m_builder.set_cur_block( next_block );

            // If the function doesn't return, early-terminate the return block.
            if( unconditional_diverge )
            {
                m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );
                m_builder.set_cur_block( m_builder.new_bb_unlinked() );
            }
            else
            {
                // NOTE: This has to be done here because the builder can't easily do it.
                m_builder.mark_value_assigned(node.span(), res);
            }
            m_builder.set_result( node.span(), mv$(res) );
        }

        void visit(::HIR::ExprNode_CallValue& node) override
        {
            TRACE_FUNCTION_F("_CallValue " << node.m_value->m_res_type);
            auto _ = save_and_edit(m_borrow_raise_target, nullptr);

            // _CallValue is ONLY valid on function pointers (all others must be desugared)
            ASSERT_BUG(node.span(), node.m_value->m_res_type.data().is_Function(), "Leftover _CallValue on a non-fn()");
            this->visit_node_ptr(node.m_value);

            // Get the function pointer in a temporary BEFORE getting arguments
            auto fcn_val = m_builder.new_temporary(node.m_value->m_res_type);
            m_builder.push_stmt_assign( node.m_value->span(), fcn_val.clone(), m_builder.get_result(node.m_value->span()) );

            auto values = get_args(node.m_args);


            auto panic_block = m_builder.new_bb_unlinked();
            auto next_block = m_builder.new_bb_unlinked();
            auto res = m_builder.new_temporary( node.m_res_type );
            m_builder.end_block(::MIR::Terminator::make_Call({
                next_block, panic_block,
                res.clone(), mv$(fcn_val),
                mv$(values)
                }));

            m_builder.set_cur_block(panic_block);
            // TODO: Proper panic handling
            m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );

            m_builder.set_cur_block( next_block );
            // TODO: Support diverging value calls
            m_builder.mark_value_assigned(node.span(), res);
            m_builder.set_result( node.span(), mv$(res) );
        }
        void visit(::HIR::ExprNode_CallMethod& node) override
        {
            // TODO: Allow use on trait objects? May not be needed, depends.
            BUG(node.span(), "Leftover _CallMethod");
        }
        void visit(::HIR::ExprNode_Field& node) override
        {
            TRACE_FUNCTION_F("_Field \"" << node.m_field << "\"");
            this->visit_node_ptr(node.m_value);
            auto val = m_builder.get_result_in_lvalue(node.m_value->span(), node.m_value->m_res_type);

            const auto& val_ty = node.m_value->m_res_type;

            unsigned int idx;
            if( ::std::isdigit(node.m_field.c_str()[0]) ) {
                ::std::stringstream(node.m_field.c_str()) >> idx;
                m_builder.set_result( node.span(), ::MIR::LValue::new_Field( mv$(val), idx ) );
            }
            else if( const auto* bep = val_ty.data().as_Path().binding.opt_Struct() ) {
                const auto& str = **bep;
                const auto& fields = str.m_data.as_Named();
                idx = ::std::find_if( fields.begin(), fields.end(), [&](const auto& x){ return x.first == node.m_field; } ) - fields.begin();
                m_builder.set_result( node.span(), ::MIR::LValue::new_Field( mv$(val), idx ) );
            }
            else if( const auto* bep = val_ty.data().as_Path().binding.opt_Union() ) {
                const auto& unm = **bep;
                const auto& fields = unm.m_variants;
                idx = ::std::find_if( fields.begin(), fields.end(), [&](const auto& x){ return x.first == node.m_field; } ) - fields.begin();

                m_builder.set_result( node.span(), ::MIR::LValue::new_Downcast( mv$(val), idx ) );
            }
            else {
                BUG(node.span(), "Field access on non-union/struct - " << val_ty);
            }
        }
        void visit(::HIR::ExprNode_Literal& node) override
        {
            TRACE_FUNCTION_F("_Literal");
            TU_MATCHA( (node.m_data), (e),
            (Integer,
                ASSERT_BUG(node.span(), node.m_res_type.data().is_Primitive(), "Non-primitive return type for Integer literal - " << node.m_res_type);
                auto ity = node.m_res_type.data().as_Primitive();
                switch(ity)
                {
                case ::HIR::CoreType::U8:
                case ::HIR::CoreType::U16:
                case ::HIR::CoreType::U32:
                case ::HIR::CoreType::U64:
                case ::HIR::CoreType::U128:
                case ::HIR::CoreType::Usize:
                    m_builder.set_result(node.span(), ::MIR::Constant::make_Uint({ e.m_value, ity }) );
                    break;
                case ::HIR::CoreType::Char:
                    m_builder.set_result(node.span(), ::MIR::Constant::make_Uint({ static_cast<uint64_t>(e.m_value), ity }) );
                    break;
                case ::HIR::CoreType::I8:
                case ::HIR::CoreType::I16:
                case ::HIR::CoreType::I32:
                case ::HIR::CoreType::I64:
                case ::HIR::CoreType::I128:
                case ::HIR::CoreType::Isize:
                    m_builder.set_result(node.span(), ::MIR::Constant::make_Int({ static_cast<int64_t>(e.m_value), ity }) );
                    break;
                default:
                    BUG(node.span(), "Integer literal with unexpected type - " << node.m_res_type);
                }
                ),
            (Float,
                ASSERT_BUG(node.span(), node.m_res_type.data().is_Primitive(), "Non-primitive return type for Float literal - " << node.m_res_type);
                auto ity = node.m_res_type.data().as_Primitive();
                m_builder.set_result(node.span(), ::MIR::RValue::make_Constant( ::MIR::Constant::make_Float({ e.m_value, ity }) ));
                ),
            (Boolean,
                m_builder.set_result(node.span(), ::MIR::RValue::make_Constant( ::MIR::Constant::make_Bool({e}) ));
                ),
            (String,
                m_builder.set_result(node.span(), ::MIR::RValue::make_Constant( ::MIR::Constant(e) ));
                ),
            (ByteString,
                auto v = mv$( *reinterpret_cast< ::std::vector<uint8_t>*>( &e) );
                m_builder.set_result(node.span(), ::MIR::RValue::make_Constant( ::MIR::Constant(mv$(v)) ));
                )
            )
        }
        void visit(::HIR::ExprNode_UnitVariant& node) override
        {
            const Span& sp = node.span();
            TRACE_FUNCTION_F("_UnitVariant");
            if( !node.m_is_struct )
            {
                // Get the variant index from the enum.
                auto enum_path = node.m_path.clone();
                enum_path.m_path.m_components.pop_back();
                const auto& var_name = node.m_path.m_path.m_components.back();

                const auto& enm = m_builder.crate().get_enum_by_path(sp, enum_path.m_path);

                auto idx = enm.find_variant(var_name);
                ASSERT_BUG(sp, idx != SIZE_MAX, "Variant " << node.m_path.m_path << " isn't present");

                // VALIDATION
                if( const auto* e = enm.m_data.opt_Data() )
                {
                    const auto& var = (*e)[idx];
                    ASSERT_BUG(sp, !var.is_struct, "Variant " << node.m_path.m_path << " isn't a unit variant");
                }

                m_builder.set_result( node.span(), ::MIR::RValue::make_EnumVariant({
                    mv$(enum_path),
                    static_cast<unsigned>(idx),
                    {}
                    }) );
            }
            else
            {
                m_builder.set_result( node.span(), ::MIR::RValue::make_Struct({
                    node.m_path.clone(),
                    {}
                    }) );
            }
        }
        void visit(::HIR::ExprNode_PathValue& node) override
        {
            const auto& sp = node.span();
            TRACE_FUNCTION_F("_PathValue - " << node.m_path);
            TU_MATCH_HDRA( (node.m_path.m_data), { )
            TU_ARMA(Generic, pe) {
                // Enum variant constructor.
                if( node.m_target == ::HIR::ExprNode_PathValue::ENUM_VAR_CONSTR ) {
                    auto enum_path = pe.m_path;
                    enum_path.m_components.pop_back();
                    const auto& var_name = pe.m_path.m_components.back();

                    // Validation only.
                    const auto& enm = m_builder.crate().get_enum_by_path(sp, enum_path);
                    ASSERT_BUG(sp, enm.m_data.is_Data(), "Getting variant constructor of value varianta");
                    size_t idx = enm.find_variant(var_name);
                    ASSERT_BUG(sp, idx != SIZE_MAX, "Variant " << pe.m_path << " isn't present");
                    const auto& var = enm.m_data.as_Data()[idx];
                    ASSERT_BUG(sp, var.type.data().is_Path(), "Variant " << pe.m_path << " isn't a tuple");
                    const auto& str = *var.type.data().as_Path().binding.as_Struct();
                    ASSERT_BUG(sp, str.m_data.is_Tuple(), "Variant " << pe.m_path << " isn't a tuple");

                    // TODO: Ideally, the creation of the wrapper function would happen somewhere before trans?
                    auto tmp = m_builder.new_temporary( node.m_res_type );
                    m_builder.push_stmt_assign( sp, tmp.clone(), ::MIR::Constant::make_ItemAddr(box$(node.m_path.clone())) );
                    m_builder.set_result( sp, mv$(tmp) );
                    return ;
                }
                const auto& vi = m_builder.crate().get_valitem_by_path(node.span(), pe.m_path);
                TU_MATCH_HDRA( (vi), {)
                TU_ARMA(Import, e) {
                    BUG(sp, "All references via imports should be replaced");
                    }
                TU_ARMA(Constant, e) {
                    auto tmp = m_builder.new_temporary( e.m_type );
                    m_builder.push_stmt_assign( sp, tmp.clone(), ::MIR::Constant::make_Const({box$(node.m_path.clone())}) );
                    m_builder.set_result( node.span(), mv$(tmp) );
                    }
                TU_ARMA(Static, e) {
                    m_builder.set_result( node.span(), ::MIR::LValue::new_Static(node.m_path.clone()) );
                    }
                TU_ARMA(StructConstant, e) {
                    // TODO: Why is this still a PathValue?
                    m_builder.set_result( node.span(), ::MIR::RValue::make_Struct({
                        pe.clone(),
                        {}
                        }) );
                    }
                TU_ARMA(Function, e) {
                    // TODO: Why not use the result type?
                    auto monomorph_cb = MonomorphStatePtr(nullptr, nullptr, &pe.m_params);

                    // TODO: Obtain function type for this function (i.e. a type that is specifically for this function)
                    auto fcn_ty_data = ::HIR::FunctionType {
                        e.m_unsafe,
                        e.m_abi,
                        monomorph_cb.monomorph_type(sp, e.m_return),
                        {}
                        };
                    fcn_ty_data.m_arg_types.reserve( e.m_args.size() );
                    for(const auto& arg : e.m_args)
                    {
                        fcn_ty_data.m_arg_types.push_back( monomorph_cb.monomorph_type(sp, arg.second) );
                    }
                    auto tmp = m_builder.new_temporary( ::HIR::TypeRef( mv$(fcn_ty_data) ) );
                    m_builder.push_stmt_assign( sp, tmp.clone(), ::MIR::Constant::make_ItemAddr(box$(node.m_path.clone())) );
                    m_builder.set_result( sp, mv$(tmp) );
                    }
                TU_ARMA(StructConstructor, e) {
                    // TODO: Ideally, the creation of the wrapper function would happen somewhere before this?
                    auto tmp = m_builder.new_temporary( node.m_res_type );
                    m_builder.push_stmt_assign( sp, tmp.clone(), ::MIR::Constant::make_ItemAddr(box$(node.m_path.clone())) );
                    m_builder.set_result( sp, mv$(tmp) );
                    }
                }
                }
            TU_ARMA(UfcsKnown, pe) {
                // Check what item type this is (from the trait)
                const auto& tr = m_builder.crate().get_trait_by_path(sp, pe.trait.m_path);
                auto it = tr.m_values.find(pe.item);
                ASSERT_BUG(sp, it != tr.m_values.end(), "Cannot find trait item for " << node.m_path);
                TU_MATCHA( (it->second), (e),
                (Constant,
                    m_builder.set_result( sp, ::MIR::Constant::make_Const({box$(node.m_path.clone())}) );
                    ),
                (Static,
                    TODO(sp, "Associated statics (non-rustc) - " << node.m_path);
                    ),
                (Function,
                    m_builder.set_result( sp, ::MIR::Constant::make_ItemAddr(box$(node.m_path.clone())) );
                    )
                )
                }
            TU_ARMA(UfcsUnknown, pe) {
                BUG(sp, "PathValue - Encountered UfcsUnknown - " << node.m_path);
                }
            TU_ARMA(UfcsInherent, pe) {
                // 1. Find item in an impl block
                auto rv = m_builder.crate().find_type_impls(pe.type, [&](const auto& ty)->const ::HIR::TypeRef& { return ty; },
                    [&](const auto& impl) {
                        DEBUG("- impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                        // Associated functions
                        {
                            auto it = impl.m_methods.find(pe.item);
                            if( it != impl.m_methods.end() ) {
                                m_builder.set_result( sp, ::MIR::Constant::make_ItemAddr(box$(node.m_path.clone())) );
                                return true;
                            }
                        }
                        // Associated consts
                        {
                            auto it = impl.m_constants.find(pe.item);
                            if( it != impl.m_constants.end() ) {
                                m_builder.set_result( sp, ::MIR::Constant::make_Const({box$(node.m_path.clone())}) );
                                return true;
                            }
                        }
                        // Associated static (undef)
                        return false;
                    });
                if( !rv ) {
                    ERROR(sp, E0000, "Failed to locate item for " << node.m_path);
                }
                }
            }
        }
        void visit(::HIR::ExprNode_Variable& node) override
        {
            TRACE_FUNCTION_F("_Variable - " << node.m_name << " #" << node.m_slot);
#if 1
            // If there's an alias active, emit that
            if( const auto* a = m_builder.get_variable_alias(node.span(), node.m_slot) )
            {
                switch( a->first )
                {
                case ::HIR::PatternBinding::Type::Move:
                    m_builder.set_result( node.span(), a->second.clone() );
                    break;
                case ::HIR::PatternBinding::Type::Ref:
                    m_builder.set_result( node.span(), ::MIR::RValue::make_Borrow({ ::HIR::BorrowType::Shared, a->second.clone() }) );
                    break;
                case ::HIR::PatternBinding::Type::MutRef:
                    m_builder.set_result( node.span(), ::MIR::RValue::make_Borrow({ ::HIR::BorrowType::Unique, a->second.clone() }) );
                    break;
                }
                return ;
            }
#endif
            m_builder.set_result( node.span(), m_builder.get_variable(node.span(), node.m_slot) );
        }
        void visit(::HIR::ExprNode_ConstParam& node) override
        {
            TRACE_FUNCTION_F("_ConstParam - " << node.m_name << " #" << node.m_binding);
            m_builder.set_result( node.span(), ::MIR::Constant::make_Generic({ node.m_name, node.m_binding }));
        }

        void visit_sl_inner(::HIR::ExprNode_StructLiteral& node, const ::HIR::Struct& str, const ::HIR::GenericPath& path)
        {
            const Span& sp = node.span();

            ASSERT_BUG(sp, str.m_data.is_Named(), "");
            const ::HIR::t_struct_fields& fields = str.m_data.as_Named();

            auto base_val = ::MIR::LValue::new_Return();
            if( node.m_base_value )
            {
                DEBUG("_StructLiteral - base");
                this->visit_node_ptr(node.m_base_value);
                base_val = m_builder.get_result_in_lvalue(node.m_base_value->span(), node.m_base_value->m_res_type);
            }

            ::std::vector<bool> values_set;
            ::std::vector< ::MIR::Param>   values;
            values.resize( fields.size() );
            values_set.resize( fields.size() );

            for(auto& ent : node.m_values)
            {
                auto& valnode = ent.second;
                auto idx = ::std::find_if(fields.begin(), fields.end(), [&](const auto&x){ return x.first == ent.first; }) - fields.begin();
                assert( !values_set[idx] );
                values_set[idx] = true;
                DEBUG("_StructLiteral - fld '" << ent.first << "' (idx " << idx << ")");
                this->visit_node_ptr(valnode);

                auto res = m_builder.get_result(valnode->span());
                if( auto* e = res.opt_Constant() )
                {
                    values.at(idx) = mv$(*e);
                }
                else
                {
                    // NOTE: Have to allocate a new temporary because ordering matters
                    auto tmp = m_builder.new_temporary(valnode->m_res_type);
                    m_builder.push_stmt_assign( valnode->span(), tmp.clone(), mv$(res) );
                    values.at(idx) = mv$(tmp);
                }
            }
            for(unsigned int i = 0; i < values.size(); i ++)
            {
                if( !values_set[i] ) {
                    if( !node.m_base_value) {
                        ERROR(node.span(), E0000, "Field '" << fields[i].first << "' not specified");
                    }
                    values[i] = ::MIR::LValue::new_Field( base_val.clone(), i );
                }
                else {
                    // Partial move support will handle dropping the rest?
                }
            }

            m_builder.set_result( node.span(), ::MIR::RValue::make_Struct({
                path.clone(),
                mv$(values)
                }) );
        }

        void visit(::HIR::ExprNode_StructLiteral& node) override
        {
            TRACE_FUNCTION_F("_StructLiteral");

            const auto& ty_path = node.m_real_path;

            TU_MATCH_HDRA( (node.m_res_type.data().as_Path().binding), {)
            TU_ARMA(Unbound, _e) {
                }
            TU_ARMA(Opaque, _e) {
                }
            TU_ARMA(Enum, e) {
                auto enum_path = ty_path.clone();
                enum_path.m_path.m_components.pop_back();
                const auto& var_name = ty_path.m_path.m_components.back();

                const auto& enm = *e;
                size_t idx = enm.find_variant(var_name);
                ASSERT_BUG(node.span(), idx != SIZE_MAX, "");
                ASSERT_BUG(node.span(), enm.m_data.is_Data(), "");
                const auto& var_ty = enm.m_data.as_Data()[idx].type;
                const auto& str = *var_ty.data().as_Path().binding.as_Struct();

                // Take advantage of the identical generics to cheaply clone/monomorph the path.
                ::HIR::GenericPath struct_path = ty_path.clone();
                struct_path.m_path = var_ty.data().as_Path().path.m_data.as_Generic().m_path;

                this->visit_sl_inner(node, str, struct_path);
                auto vals = std::move(m_builder.get_result(node.span()).as_Struct().vals);

                // And create Variant
                m_builder.set_result( node.span(), ::MIR::RValue::make_EnumVariant({
                    mv$(enum_path),
                    static_cast<unsigned>(idx),
                    mv$(vals)
                    }) );
                }
            TU_ARMA(Union, e) {
                const auto& variant_name = node.m_values.front().first;
                auto& value_node =  node.m_values.front().second;
                this->visit_node_ptr(value_node);
                auto val = m_builder.get_result_in_lvalue(value_node->span(), value_node->m_res_type);

                const auto& unm = *e;
                auto it = ::std::find_if(unm.m_variants.begin(), unm.m_variants.end(), [&](const auto&v)->auto{ return v.first == variant_name; });
                assert(it != unm.m_variants.end());
                unsigned int idx = it - unm.m_variants.begin();

                m_builder.set_result( node.span(), ::MIR::RValue::make_UnionVariant({
                    node.m_real_path.clone(),
                    idx,
                    mv$(val)
                    }) );
                }
            TU_ARMA(ExternType, e) {
                BUG(node.span(), "_StructLiteral ExternType isn't valid?");
                }
            TU_ARMA(Struct, e) {
                if(e->m_data.is_Unit()) {
                    m_builder.set_result( node.span(), ::MIR::RValue::make_Struct({
                        ty_path.clone(),
                        {}
                        }) );
                    return ;
                }

                this->visit_sl_inner(node, *e, ty_path);
                }
            }
        }

        void visit(::HIR::ExprNode_Tuple& node) override
        {
            TRACE_FUNCTION_F("_Tuple");
            ::std::vector< ::MIR::Param>   values;
            values.reserve( node.m_vals.size() );
            for(auto& arg : node.m_vals)
            {
                this->visit_node_ptr(arg);
                values.push_back( m_builder.get_result_in_param(arg->span(), arg->m_res_type) );
            }

            m_builder.set_result( node.span(), ::MIR::RValue::make_Tuple({
                mv$(values)
                }) );
        }

        void visit(::HIR::ExprNode_ArrayList& node) override
        {
            TRACE_FUNCTION_F("_ArrayList");
            ::std::vector< ::MIR::Param>   values;
            values.reserve( node.m_vals.size() );
            for(auto& arg : node.m_vals)
            {
                this->visit_node_ptr(arg);
                values.push_back( m_builder.get_result_in_param(arg->span(), arg->m_res_type) );
            }

            m_builder.set_result( node.span(), ::MIR::RValue::make_Array({
                mv$(values)
                }) );
        }

        void visit(::HIR::ExprNode_ArraySized& node) override
        {
            TRACE_FUNCTION_F("_ArraySized");
            this->visit_node_ptr( node.m_val );
            auto value = m_builder.get_result_in_param(node.span(), node.m_val->m_res_type);

            m_builder.set_result( node.span(), ::MIR::RValue::make_SizedArray({
                mv$(value),
                static_cast<unsigned int>(node.m_size_val)
                }) );
        }

        void visit(::HIR::ExprNode_Closure& node) override
        {
            TRACE_FUNCTION_F("_Closure - " << node.m_obj_path);
            auto _ = save_and_edit(m_borrow_raise_target, nullptr);

            ::std::vector< ::MIR::Param>   vals;
            vals.reserve( node.m_captures.size() );
            for(auto& arg : node.m_captures)
            {
                this->visit_node_ptr(arg);
                vals.push_back( m_builder.get_result_in_lvalue(arg->span(), arg->m_res_type) );
            }

            m_builder.set_result( node.span(), ::MIR::RValue::make_Struct({
                node.m_obj_path.clone(),
                mv$(vals)
                }) );
        }
        void visit(::HIR::ExprNode_Generator& node) override
        {
            TRACE_FUNCTION_F("_Generator - " << node.m_obj_path);
            ASSERT_BUG(node.span(), node.m_obj_ptr, "Generator not created");
            ASSERT_BUG(node.span(), !node.m_code, "Encountered outer generator wrapper");
            auto _ = save_and_edit(m_borrow_raise_target, nullptr);

            ::std::vector< ::MIR::Param>   vals;
            vals.reserve( 1 + node.m_captures.size() );

            // Zero the state index
            {
                const ::HIR::TypeRef& state_type = node.m_state_data_type;
                const auto& lang_MaybeUninit = m_builder.resolve().m_crate.get_lang_item_path(node.span(), "maybe_uninit");
                const auto& unm_MaybeUninit = m_builder.resolve().m_crate.get_union_by_path(node.span(), lang_MaybeUninit);
                auto slot_type = ::HIR::TypeRef::new_path( ::HIR::GenericPath(lang_MaybeUninit, ::HIR::PathParams(state_type.clone())), &unm_MaybeUninit );

                auto res_slot = m_builder.new_temporary( slot_type.clone() );
                auto size__panic = m_builder.new_bb_unlinked();
                auto size__ok = m_builder.new_bb_unlinked();
                m_builder.end_block(::MIR::Terminator::make_Call({
                    size__ok, size__panic,
                    res_slot.clone(), ::MIR::CallTarget::make_Intrinsic({ "init", ::HIR::PathParams(mv$(slot_type)) }), // I.e. `mem::zeroed`
                    {}
                    }));
                m_builder.set_cur_block(size__panic); m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );   // HACK
                m_builder.set_cur_block(size__ok);
                vals.push_back( std::move(res_slot) );
            }
            // Populate the rest
            for(auto& arg : node.m_captures)
            {
                this->visit_node_ptr(arg);
                vals.push_back( m_builder.get_result_in_lvalue(arg->span(), arg->m_res_type) );
            }

            m_builder.set_result( node.span(), ::MIR::RValue::make_Struct({
                node.m_obj_path.clone(),
                mv$(vals)
                }) );
        }
        void visit(::HIR::ExprNode_GeneratorWrapper& node) override
        {
            BUG(node.span(), "Unexpected");
        }
    };
}


::MIR::FunctionPointer LowerMIR(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, const ::HIR::ExprPtr& ptr, const ::HIR::TypeRef& ret_ty, const ::HIR::Function::args_t& args)
{
    TRACE_FUNCTION_F(path);

    ::MIR::Function fcn;
    fcn.locals.reserve(ptr.m_bindings.size());
    for(const auto& t : ptr.m_bindings)
        fcn.locals.push_back( t.clone() );

    // Scope ensures that builder cleanup happens before `fcn` is moved
    {
        const Span& sp = ptr->span();

        ::HIR::ExprNode& root_node = const_cast<::HIR::ExprNode&>(*ptr);
        MirBuilder  builder { ptr->span(), resolve, ret_ty, args, fcn };
        ExprVisitor_Conv    ev { builder, ptr.m_bindings, dynamic_cast<::HIR::ExprNode_GeneratorWrapper*>(&root_node) };

        // 1. Apply destructuring to arguments
        unsigned int i = 0;
        for( const auto& arg : args )
        {
            const auto& pat = arg.first;
            // What's the idea of this check? How could it _not_ be true in most cases (and thus not prepare the input)
            if( pat.m_binding.is_valid() && pat.m_binding.m_type == ::HIR::PatternBinding::Type::Move )
            {
                // Simple `var: Type` arguments are handled by `MirBuilder.m_var_arg_mappings`
            }
            else
            {
                ev.define_vars_from(ptr->span(), arg.first);
                ev.destructure_from(ptr->span(), arg.first, ::MIR::LValue::new_Argument(i));
            }
            i ++;
        }

        // 2. Destructure code
        if(auto* gen_node = dynamic_cast<::HIR::ExprNode_GeneratorWrapper*>(&root_node) ) 
        {
            // Mark all capture locals as valid (for later rewrite into variable acesses)
            ::std::map<unsigned, std::vector<MIR::LValue::Wrapper> >   mappings;
            for(size_t i = 0; i < gen_node->m_capture_usages.size(); i ++)
            {
                unsigned idx = args.size() + i;
                builder.define_variable(idx);
                builder.mark_value_assigned(root_node.span(), ::MIR::LValue::new_Local(idx));
                // self.IDX
                mappings.insert(std::make_pair( idx, ::make_vec1(::MIR::LValue::Wrapper::new_Field(1+i)) ));
                switch(gen_node->m_capture_usages[i])
                {
                case ::HIR::ValueUsage::Borrow:
                case ::HIR::ValueUsage::Mutate:
                    mappings[idx].push_back(::MIR::LValue::Wrapper::new_Deref());
                    break;
                case ::HIR::ValueUsage::Move:
                case ::HIR::ValueUsage::Unknown:
                    break;
                }
            }

            // ------------

            gen_node->m_code->visit(ev);
            if( builder.block_active() && builder.has_result() )
            {
                ::std::vector< ::MIR::Param>   values;
                values.push_back( builder.get_result_in_param(sp, gen_node->m_code->m_res_type) );

                ::HIR::GenericPath enm_path;
                builder.with_val_type(sp, ::MIR::LValue::new_Return(), [&](const ::HIR::TypeRef& ty) {
                    const auto& te = ty.data().as_Path();
                    enm_path = te.path.m_data.as_Generic().clone();
                    ASSERT_BUG(sp, te.binding.as_Enum()->find_variant("Complete") == 1, "");
                    });

                builder.set_result(sp, ::MIR::RValue::make_EnumVariant({
                    mv$(enm_path),
                    1,  // Complete is the second variant
                    mv$(values)
                    }) );
            }
            builder.final_cleanup();

            // ------------

            // 1. Generate the state machine switch (and enumerate saved variables)
            std::set<unsigned>  saved = ev.generator_finalise(gen_node->span(), const_cast<HIR::Enum&>(resolve.m_crate.get_enum_by_path(sp, gen_node->m_state_idx_enum)));
            // 2. Populate state structure
            auto& state_ty = const_cast<HIR::Struct&>(*gen_node->m_state_data_type.data().as_Path().binding.as_Struct());
            unsigned value_var_idx; {
                const auto& unm_MaybeUninit = resolve.m_crate.get_union_by_path(sp, resolve.m_crate.get_lang_item_path(gen_node->span(), "maybe_uninit"));
                value_var_idx = std::find_if(unm_MaybeUninit.m_variants.begin(), unm_MaybeUninit.m_variants.end(), [&](const auto& e){ return e.first == "value";}) - unm_MaybeUninit.m_variants.begin();
            }
            ASSERT_BUG(sp, value_var_idx == 1, "Assumption on MaybeUninit.value's variant index failed");
            // - Any variables that are saved twice need to have a static address, others can share?
            // - Lazy option (doesn't require making sub-types): Toss everything together
            auto& fields = state_ty.m_data.as_Tuple();
            for(auto idx : saved)
            {
                if( idx < 1+gen_node->m_capture_usages.size()) {
                }
                else {
                    auto field_idx = fields.size();
                    ASSERT_BUG(sp, idx < fcn.locals.size(), idx << " >= " << fcn.locals.size());
                    fields.push_back(::HIR::VisEnt<HIR::TypeRef> { HIR::Publicity::new_none(), fcn.locals.at(idx).clone() });
                    // self.state(0).value(?#1).value(?0).IDX
                    mappings.insert(std::make_pair( idx, std::vector<MIR::LValue::Wrapper> {
                        ::MIR::LValue::Wrapper::new_Field(0),
                        ::MIR::LValue::Wrapper::new_Downcast(value_var_idx),    // MaybeUninit.value
                        ::MIR::LValue::Wrapper::new_Field(0),   // ManuallyDrop.value
                        ::MIR::LValue::Wrapper::new_Field(field_idx)
                        } ));
                }
            }

            DEBUG("mappings={" << mappings << "}");

            // 3. Rewrite usage of saved values
            // - Note: Need to allocate new temporaries if indexing by an updated lvalue
            class Rewriter: public ::MIR::visit::VisitorMut
            {
                ::std::map<unsigned, std::vector<MIR::LValue::Wrapper> >& m_mappings;
                ::std::vector< ::MIR::Statement>    m_new_statements;
            public:
                Rewriter(::std::map<unsigned, std::vector<MIR::LValue::Wrapper> >& mappings)
                    :m_mappings(mappings)
                {
                }

                bool visit_lvalue(::MIR::LValue& lv, ::MIR::visit::ValUsage u) override
                {
                    if( lv.m_root.is_Local() ) {
                        auto it = m_mappings.find(lv.m_root.as_Local());
                        if( it != m_mappings.end() ) {
                            lv.m_root = ::MIR::LValue::Storage::new_Argument(0);
                            auto dit = lv.m_wrappers.begin();
                            dit = lv.m_wrappers.insert(dit, ::MIR::LValue::Wrapper::new_Field(0)) + 1;  // Pin.ptr
                            dit = lv.m_wrappers.insert(dit, ::MIR::LValue::Wrapper::new_Deref()) + 1;   // *
                            dit = lv.m_wrappers.insert(dit, it->second.begin(), it->second.end()) + 1;
                        }
                    }
                    for(auto& w : lv.m_wrappers)
                    {
                        if( w.is_Index() )
                        {
                            auto it = m_mappings.find(w.as_Index());
                            if( it != m_mappings.end() ) {
                                // Allocate a new temporary, assign it before this statement, use that
                                TODO(Span(), "");
                            }
                        }
                    }

                    return true;
                }

                void push_statements(::MIR::BasicBlock& bb, size_t& ofs)
                {
                    for(auto& e : m_new_statements)
                    {
                        bb.statements.insert( bb.statements.begin() + ofs, std::move(e) );
                        ofs += 1;
                    }
                    m_new_statements.clear();
                }

                void rewrite_fcn(::MIR::Function& f)
                {
                    for(auto& bb : f.blocks)
                    {
                        for(size_t stmt_idx = 0; stmt_idx < bb.statements.size(); stmt_idx ++)
                        {
                            this->visit_stmt(bb.statements[stmt_idx]);
                            this->push_statements(bb, stmt_idx);
                        }
                        this->visit_terminator(bb.terminator);
                        size_t stmt_idx = bb.statements.size();
                        this->push_statements(bb, stmt_idx);
                    }
                }
            };
            Rewriter(mappings).rewrite_fcn(fcn);

            // 4. Generate drop glue for the generator type and save for later
            // - Make a builder
            // - Insert the switch for each arm
            // - Trigger drops
            auto drop_impl_body = ::MIR::FunctionPointer(new ::MIR::Function());
            {
                TRACE_FUNCTION_F("Generating drop impl");
                MirBuilder  drop_builder(sp, resolve, HIR::TypeRef::new_unit(), gen_node->m_drop_fcn_ptr->m_args, *drop_impl_body);
                ev.generator_make_drop(sp, drop_builder, gen_node->m_capture_usages.size(), mappings);
                drop_builder.final_cleanup();
            }
            gen_node->m_drop_fcn_ptr->m_code.m_mir = std::move(drop_impl_body);
        }
        else
        {
            root_node.visit( ev );
            builder.final_cleanup();
        }
    }

    // NOTE: Can't clean up yet, as consteval isn't done
    //MIR_Cleanup(resolve, path, fcn, args, ret_ty);
    //DEBUG("MIR Dump:" << ::std::endl << FMT_CB(ss, MIR_Dump_Fcn(ss, fcn, 1);));
    MIR_Validate(resolve, path, fcn, args, ret_ty);

    if( getenv("MRUSTC_VALIDATE_FULL_EARLY") ) {
        MIR_Validate_Full(resolve, path, fcn, args, ptr->m_res_type);
    }

    return ::MIR::FunctionPointer(new ::MIR::Function(mv$(fcn)));
}

// --------------------------------------------------------------------

void HIR_GenerateMIR_Expr(const ::HIR::Crate& crate, const ::HIR::ItemPath& path, ::HIR::ExprPtr& expr_ptr, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& res_ty)
{
    if( !expr_ptr.m_mir )
    {
        StaticTraitResolve  resolve { crate };
        if(expr_ptr.m_state->m_impl_generics)   resolve.set_impl_generics(*expr_ptr.m_state->m_impl_generics);
        if(expr_ptr.m_state->m_item_generics)   resolve.set_item_generics(*expr_ptr.m_state->m_item_generics);
        expr_ptr.set_mir( LowerMIR(resolve, path, expr_ptr, res_ty, args) );
        // Run cleanup to simplify consteval?
        // - This ends up running before things like vtable generation
        //MIR_Cleanup(resolve, path, *expr_ptr.m_mir, args, res_ty);
    }
}

void HIR_GenerateMIR(::HIR::Crate& crate)
{
    ::MIR::OuterVisitor    ov { crate, [&](const auto& res, const auto& p, ::HIR::ExprPtr& expr_ptr, const auto& args, const auto& ty){
            if( !expr_ptr.get_mir_opt() )
            {
                expr_ptr.set_mir( LowerMIR(res, p, expr_ptr, ty, args) );
            }
        } };
    ov.visit_crate(crate);

    // Once MIR is generated, free the HIR expression tree (replace each node with an empty tuple node)
    ::MIR::OuterVisitor ov_free(crate, [&](const auto& res, const auto& p, ::HIR::ExprPtr& expr_ptr, const auto& args, const auto& ty){
        if( expr_ptr )
        {
            expr_ptr.reset(new ::HIR::ExprNode_Tuple(expr_ptr->m_span, {}));
        }
        });
    ov_free.visit_crate(crate);
}

