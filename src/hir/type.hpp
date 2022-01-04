/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/type.hpp
 * - HIR Type representation
 */
#ifndef _HIR_TYPE_HPP_
#define _HIR_TYPE_HPP_
#pragma once

#include <tagged_union.hpp>
#include <hir/path.hpp>
#include <hir/expr_ptr.hpp>
#include <span.hpp>
#include "type_ref.hpp"
#include "literal.hpp"
#include "generic_ref.hpp"

constexpr const char* CLOSURE_PATH_PREFIX = "closure#";
constexpr const char* GENERATOR_PATH_PREFIX = "generator#";

namespace HIR {

struct TraitMarkings;
class ExternType;
class Struct;
class Union;
class Enum;
struct ExprNode_Closure;
struct ExprNode_Generator;

class TypeRef;

enum class CoreType
{
    Usize, Isize,
    U8, I8,
    U16, I16,
    U32, I32,
    U64, I64,
    U128, I128,

    F32, F64,

    Bool,
    Char, Str,
};
extern ::std::ostream& operator<<(::std::ostream& os, const CoreType& ct);
static inline bool is_integer(const CoreType& v) {
    switch(v)
    {
    case CoreType::Usize: case CoreType::Isize:
    case CoreType::U8 : case CoreType::I8:
    case CoreType::U16: case CoreType::I16:
    case CoreType::U32: case CoreType::I32:
    case CoreType::U64: case CoreType::I64:
    case CoreType::U128: case CoreType::I128:
        return true;
    default:
        return false;
    }
}
static inline bool is_float(const CoreType& v) {
    switch(v)
    {
    case CoreType::F32:
    case CoreType::F64:
        return true;
    default:
        return false;
    }
}

enum class BorrowType
{
    Shared,
    Unique,
    Owned,
};
extern ::std::ostream& operator<<(::std::ostream& os, const BorrowType& bt);


/// Array size used for types AND array literals
TAGGED_UNION_EX(ArraySize, (), Unevaluated, (
    /// Un-evaluated size
    (Unevaluated, ConstGeneric),
    /// Fully known
    (Known, uint64_t)
    ),
    /*extra_move=*/(),
    /*extra_assign=*/(),
    /*extra=*/(
        ArraySize clone() const;
        Ordering ord(const ArraySize& x) const;
        bool operator==(const ArraySize& x) const { return ord(x) == OrdEqual; }
        bool operator!=(const ArraySize& x) const { return !operator==(x); }
    )
    );
extern ::std::ostream& operator<<(::std::ostream& os, const ArraySize& x);


TAGGED_UNION_EX(TypePathBinding, (), Unbound, (
    (Unbound, struct {}),   // Not yet bound, either during lowering OR during resolution (when associated and still being resolved)
    (Opaque, struct {}),    // Opaque, i.e. An associated type of a generic (or Self in a trait)
    (ExternType, const ::HIR::ExternType*),
    (Struct, const ::HIR::Struct*),
    (Union, const ::HIR::Union*),
    (Enum, const ::HIR::Enum*)
    ), (), (), (
        TypePathBinding clone() const;

        const TraitMarkings* get_trait_markings() const;

        bool operator==(const TypePathBinding& x) const;
        bool operator!=(const TypePathBinding& x) const { return !(*this == x); }
    )
    );


struct FunctionType
{
    bool    is_unsafe;
    ::std::string   m_abi;
    TypeRef m_rettype;
    ::std::vector<TypeRef>  m_arg_types;
};

TAGGED_UNION(TypeData, Diverge,
    (Infer, struct X1{
        unsigned int index;
        InferClass  ty_class;

        /// Returns true if the ivar is a literal
        bool is_lit() const {
            switch(this->ty_class)
            {
            case InferClass::None:
                return false;
            case InferClass::Integer:
            case InferClass::Float:
                return true;
            }
            throw "";
        }
        }),
    (Diverge, struct {}),
    (Primitive, ::HIR::CoreType),
    (Path, struct X2{  // TODO: Pointer wrap
        ::HIR::Path path;
        TypePathBinding binding;

        bool is_closure() const {
            return path.m_data.is_Generic()
                && path.m_data.as_Generic().m_path.m_components.back().size() > 8
                && path.m_data.as_Generic().m_path.m_components.back().compare(0,strlen(CLOSURE_PATH_PREFIX), CLOSURE_PATH_PREFIX) == 0
                ;
        }
        bool is_generator() const {
            return path.m_data.is_Generic()
                && path.m_data.as_Generic().m_path.m_components.back().size() > 8
                && path.m_data.as_Generic().m_path.m_components.back().compare(0,strlen(GENERATOR_PATH_PREFIX), GENERATOR_PATH_PREFIX) == 0
                ;
        }
        }),
    (Generic, GenericRef),
    (TraitObject, struct {  // TODO: Pointer wrap
        ::HIR::TraitPath    m_trait;
        ::std::vector< ::HIR::GenericPath > m_markers;
        ::HIR::LifetimeRef  m_lifetime;
        }),
    (ErasedType, struct {  // TODO: Pointer wrap
        ::HIR::Path m_origin;
        unsigned int m_index;
        bool m_is_sized;
        ::std::vector< ::HIR::TraitPath>    m_traits;
        ::HIR::LifetimeRef  m_lifetime;
        }),
    (Array, struct {
        TypeRef inner;
        ArraySize  size;
        }),
    (Slice, struct {
        TypeRef inner;
        }),
    (Tuple, ::std::vector<TypeRef>),
    (Borrow, struct {
        ::HIR::LifetimeRef  lifetime;
        ::HIR::BorrowType   type;
        TypeRef inner;
        }),
    (Pointer, struct {
        ::HIR::BorrowType   type;
        TypeRef inner;
        }),
    (Function, FunctionType),   // TODO: Pointer wrap, this is quite large
    (Closure, struct {
        const ::HIR::ExprNode_Closure*  node;
        TypeRef m_rettype;
        ::std::vector<TypeRef>  m_arg_types;
        }),
    (Generator, struct {
        const ::HIR::ExprNode_Generator* node;
        })
    );

class TypeInner
{
    friend class TypeRef;
public:
    // Existing TypeRef

private:
    unsigned    m_refcount;
public:
    TypeData   m_data;
private:
    TypeInner(TypeData d):
        m_refcount(1),
        m_data(mv$(d))
    {
    }
};

inline TypeRef::TypeRef():
    TypeRef(TypeData::make_Infer({ ~0u, InferClass::None }))
{
}
inline TypeRef::TypeRef(TypeData d):
    m_ptr(new TypeInner(mv$(d)))
{
}
inline TypeRef::TypeRef(const TypeRef& x):
    m_ptr(x.m_ptr)
{
    x.m_ptr->m_refcount += 1;
}
inline TypeRef::~TypeRef()
{
    if(m_ptr)
    {
        m_ptr->m_refcount -= 1;
        if(m_ptr->m_refcount == 0)
        {
            delete m_ptr;
            m_ptr = nullptr;
        }
    }
}
inline const TypeData& TypeRef::data() const { assert(m_ptr); return m_ptr->m_data; }
inline TypeData& TypeRef::data_mut() { assert(m_ptr); return m_ptr->m_data; }
inline TypeData& TypeRef::get_unique() { assert(m_ptr); if(m_ptr->m_refcount != 1) *this = this->clone_shallow(); return m_ptr->m_data; }


inline TypeRef::TypeRef(::HIR::CoreType ct):
    TypeRef( TypeData::make_Primitive(mv$(ct)) )
{}
inline TypeRef::TypeRef(RcString name, unsigned int slot):
    TypeRef( TypeData::make_Generic({ mv$(name), slot }) )
{}
inline TypeRef::TypeRef(::std::vector< ::HIR::TypeRef> sts):
    TypeRef( TypeData::make_Tuple(mv$(sts)) )
{}
inline TypeRef::TypeRef(FunctionType ft):
    TypeRef( TypeData::make_Function(mv$(ft)) )
{
}

inline TypeRef TypeRef::new_unit() {
    return TypeRef(TypeData::make_Tuple({}));
}
inline TypeRef TypeRef::new_diverge() {
    return TypeRef(TypeData::make_Diverge({}));
}
inline TypeRef TypeRef::new_infer(unsigned int idx /*= ~0u*/, InferClass ty_class /*= InferClass::None*/) {
    return TypeRef(TypeData::make_Infer({idx, ty_class}));
}
inline TypeRef TypeRef::new_borrow(BorrowType bt, TypeRef inner) {
    return TypeRef(TypeData::make_Borrow({ ::HIR::LifetimeRef(), bt, mv$(inner) }));
}
inline TypeRef TypeRef::new_pointer(BorrowType bt, TypeRef inner) {
    return TypeRef(TypeData::make_Pointer({bt, mv$(inner)}));
}
inline TypeRef TypeRef::new_slice(TypeRef inner) {
    return TypeRef(TypeData::make_Slice({mv$(inner)}));
}
inline TypeRef TypeRef::new_array(TypeRef inner, uint64_t size) {
    assert(size != ~0u);
    return TypeRef(TypeData::make_Array({mv$(inner), size}));
}
inline TypeRef TypeRef::new_array(TypeRef inner, ::HIR::ConstGeneric size_gen) {
    return TypeRef(TypeData::make_Array({mv$(inner), mv$(size_gen) }));
}
inline TypeRef TypeRef::new_path(::HIR::Path path, TypePathBinding binding) {
    return TypeRef(TypeData::make_Path({ mv$(path), mv$(binding) }));
}
inline TypeRef TypeRef::new_closure(::HIR::ExprNode_Closure* node_ptr, ::std::vector< ::HIR::TypeRef> args, ::HIR::TypeRef rv) {
    return TypeRef(TypeData::make_Closure({ node_ptr, mv$(rv), mv$(args) }));
}
inline TypeRef TypeRef::new_generator(::HIR::ExprNode_Generator* node_ptr) {
    return TypeRef(TypeData::make_Generator({ node_ptr }));
}

inline const ::HIR::SimplePath* TypeRef::get_sort_path() const {
    // - Generic paths get sorted
    if( TU_TEST1(this->data(), Path, .path.m_data.is_Generic()) )
    {
        return &this->data().as_Path().path.m_data.as_Generic().m_path;
    }
    // - So do trait objects
    else if( this->data().is_TraitObject() )
    {
        return &this->data().as_TraitObject().m_trait.m_path.m_path;
    }
    else
    {
        // Keep as nullptr, will search primitive list
        return nullptr;
    }
}

#if 0
// TODO: Convert to a shared_ptr (or interior RC)
// - How to handle resolution actions? (Replacing generics)
class TypeRef
{
public:
    TypeData   m_data;

    TypeRef():
        m_data(TypeData::make_Infer({ ~0u, InferClass::None }))
    {}
    TypeRef(TypeRef&& ) = default;
    TypeRef(const TypeRef& ) = delete;
    TypeRef& operator=(TypeRef&& ) = default;
    TypeRef& operator=(const TypeRef&) = delete;

    TypeRef(::HIR::TypeData x):
        m_data( mv$(x) )
    {}

    TypeRef(::std::vector< ::HIR::TypeRef> sts):
        m_data( TypeData::make_Tuple(mv$(sts)) )
    {}
    TypeRef(RcString name, unsigned int slot):
        m_data( TypeData::make_Generic({ mv$(name), slot }) )
    {}
    TypeRef(::HIR::CoreType ct):
        m_data( TypeData::make_Primitive(mv$(ct)) )
    {}

    static TypeRef new_unit() {
        return TypeRef(TypeData::make_Tuple({}));
    }
    static TypeRef new_diverge() {
        return TypeRef(TypeData::make_Diverge({}));
    }
    static TypeRef new_infer(unsigned int idx = ~0u, InferClass ty_class = InferClass::None) {
        return TypeRef(TypeData::make_Infer({idx, ty_class}));
    }
    static TypeRef new_borrow(BorrowType bt, TypeRef inner) {
        return TypeRef(TypeData::make_Borrow({ ::HIR::LifetimeRef(), bt, box$(mv$(inner)) }));
    }
    static TypeRef new_pointer(BorrowType bt, TypeRef inner) {
        return TypeRef(TypeData::make_Pointer({bt, box$(mv$(inner))}));
    }
    static TypeRef new_slice(TypeRef inner) {
        return TypeRef(TypeData::make_Slice({box$(mv$(inner))}));
    }
    static TypeRef new_array(TypeRef inner, uint64_t size) {
        assert(size != ~0u);
        return TypeRef(TypeData::make_Array({box$(mv$(inner)), size}));
    }
    static TypeRef new_array(TypeRef inner, ::HIR::ExprPtr size_expr) {
        return TypeRef(TypeData::make_Array({box$(mv$(inner)), std::make_shared<HIR::ExprPtr>(mv$(size_expr)) }));
    }
    static TypeRef new_path(::HIR::Path path, TypePathBinding binding) {
        return TypeRef(TypeData::make_Path({ mv$(path), mv$(binding) }));
    }
    static TypeRef new_closure(::HIR::ExprNode_Closure* node_ptr, ::std::vector< ::HIR::TypeRef> args, ::HIR::TypeRef rv) {
        return TypeRef(TypeData::make_Closure({ node_ptr, box$(mv$(rv)), mv$(args) }));
    }

    TypeRef clone() const;

    void fmt(::std::ostream& os) const;

    bool operator==(const ::HIR::TypeRef& x) const;
    bool operator!=(const ::HIR::TypeRef& x) const { return !(*this == x); }
    bool operator<(const ::HIR::TypeRef& x) const { return ord(x) == OrdLess; }
    Ordering ord(const ::HIR::TypeRef& x) const;

    bool contains_generics() const;

    // Match generics in `this` with types from `x`
    // Raises a bug against `sp` if there is a form mismatch or `this` has an infer
    void match_generics(const Span& sp, const ::HIR::TypeRef& x, t_cb_resolve_type resolve_placeholder, t_cb_match_generics) const;

    bool match_test_generics(const Span& sp, const ::HIR::TypeRef& x, t_cb_resolve_type resolve_placeholder, t_cb_match_generics) const;

    // Compares this type with another, calling the first callback to resolve placeholders in the other type, and the second callback for generics in this type
    ::HIR::Compare match_test_generics_fuzz(const Span& sp, const ::HIR::TypeRef& x_in, t_cb_resolve_type resolve_placeholder, t_cb_match_generics callback) const;

    // Compares this type with another, using `resolve_placeholder` to get replacements for generics/infers in `x`
    Compare compare_with_placeholders(const Span& sp, const ::HIR::TypeRef& x, t_cb_resolve_type resolve_placeholder) const;

    const ::HIR::SimplePath* get_sort_path() const {
        // - Generic paths get sorted
        if( TU_TEST1(this->m_data, Path, .path.m_data.is_Generic()) )
        {
            return &this->m_data.as_Path().path.m_data.as_Generic().m_path;
        }
        // - So do trait objects
        else if( this->m_data.is_TraitObject() )
        {
            return &this->m_data.as_TraitObject().m_trait.m_path.m_path;
        }
        else
        {
            // Keep as nullptr, will search primitive list
            return nullptr;
        }
    }
};
#endif

extern ::std::ostream& operator<<(::std::ostream& os, const ::HIR::TypeRef& ty);

}   // namespace HIR

#endif

