/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#if ENABLE(WEBASSEMBLY)

#include <JavaScriptCore/WasmGCType.h>
#include <JavaScriptCore/WasmOps.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace TestWebKitAPI {

using namespace JSC::Wasm;

// Helper to create a simple function type: (i32) -> i32
static WasmGCFunctionType* createSimpleFunctionType()
{
    auto* func = WasmGCFunctionType::tryCreate(1, 1);
    EXPECT_TRUE(func);
    func->getReturnType(0) = Types::I32;
    func->getArgumentType(0) = Types::I32;
    return func;
}

// Helper to create a function type: (i32, i64) -> f32
static WasmGCFunctionType* createDifferentFunctionType()
{
    auto* func = WasmGCFunctionType::tryCreate(1, 2);
    EXPECT_TRUE(func);
    func->getReturnType(0) = Types::F32;
    func->getArgumentType(0) = Types::I32;
    func->getArgumentType(1) = Types::I64;
    return func;
}

TEST(WasmGCType, FunctionTypeCreationAndAccessors)
{
    auto* func = createSimpleFunctionType();
    ASSERT_TRUE(func);

    EXPECT_TRUE(func->is<WasmGCFunctionType>());
    EXPECT_FALSE(func->is<WasmGCStructType>());
    EXPECT_FALSE(func->is<WasmGCArrayType>());
    EXPECT_EQ(func->typeKind(), WasmGCTypeKind::FunctionType);

    EXPECT_EQ(func->argumentCount(), 1u);
    EXPECT_EQ(func->returnCount(), 1u);
    EXPECT_EQ(func->argumentType(0).kind, TypeKind::I32);
    EXPECT_EQ(func->returnType(0).kind, TypeKind::I32);

    func->destroy();
}

TEST(WasmGCType, StructTypeCreationAndAccessors)
{
    FieldType fields[] = {
        { StorageType(Types::I32), Mutability::Mutable },
        { StorageType(Types::F64), Mutability::Immutable },
    };

    auto* structType = WasmGCStructType::tryCreate(fields);
    ASSERT_TRUE(structType);

    EXPECT_TRUE(structType->is<WasmGCStructType>());
    EXPECT_FALSE(structType->is<WasmGCFunctionType>());
    EXPECT_EQ(structType->typeKind(), WasmGCTypeKind::StructType);

    EXPECT_EQ(structType->fieldCount(), 2u);
    EXPECT_EQ(structType->field(0).mutability, Mutability::Mutable);
    EXPECT_EQ(structType->field(1).mutability, Mutability::Immutable);
    EXPECT_TRUE(structType->field(0).type.is<Type>());
    EXPECT_EQ(structType->field(0).type.as<Type>().kind, TypeKind::I32);
    EXPECT_EQ(structType->field(1).type.as<Type>().kind, TypeKind::F64);

    // Field offsets should be computed.
    EXPECT_EQ(structType->offsetOfFieldInPayload(0), 0u);
    // f64 is 8 bytes and requires 8-byte alignment, so offset should be 8.
    EXPECT_EQ(structType->offsetOfFieldInPayload(1), 8u);

    structType->destroy();
}

TEST(WasmGCType, ArrayTypeCreationAndAccessors)
{
    FieldType elemType = { StorageType(Types::I32), Mutability::Mutable };
    auto* arrayType = WasmGCArrayType::tryCreate(elemType);
    ASSERT_TRUE(arrayType);

    EXPECT_TRUE(arrayType->is<WasmGCArrayType>());
    EXPECT_FALSE(arrayType->is<WasmGCStructType>());
    EXPECT_EQ(arrayType->typeKind(), WasmGCTypeKind::ArrayType);

    EXPECT_EQ(arrayType->elementType().mutability, Mutability::Mutable);
    EXPECT_TRUE(arrayType->elementType().type.is<Type>());
    EXPECT_EQ(arrayType->elementType().type.as<Type>().kind, TypeKind::I32);

    arrayType->destroy();
}

TEST(WasmGCType, NonRecursiveHashEquality)
{
    auto* func1 = createSimpleFunctionType();
    auto* func2 = createSimpleFunctionType();
    ASSERT_TRUE(func1);
    ASSERT_TRUE(func2);

    // Two structurally identical function types should hash the same.
    EXPECT_EQ(func1->hash(), func2->hash());
    EXPECT_TRUE(WasmGCType::structurallyEqual(func1, func2));

    func1->destroy();
    func2->destroy();
}

TEST(WasmGCType, DifferentTypesInequality)
{
    auto* func1 = createSimpleFunctionType();
    auto* func2 = createDifferentFunctionType();
    ASSERT_TRUE(func1);
    ASSERT_TRUE(func2);

    // Different function types should not be structurally equal.
    EXPECT_FALSE(WasmGCType::structurallyEqual(func1, func2));

    func1->destroy();
    func2->destroy();
}

TEST(WasmGCType, SelfReferentialStructHash)
{
    // Create a struct with a field that refers to itself (via TypeIndex).
    // struct S { field: ref S }
    // We create it with a placeholder first, then patch.
    FieldType fields[] = {
        { StorageType(Type { TypeKind::Ref, 0 }), Mutability::Mutable },
    };

    auto* structType = WasmGCStructType::tryCreate(fields);
    ASSERT_TRUE(structType);

    // Patch the field's TypeIndex to point back to this struct.
    TypeIndex selfIndex = structType->index();
    auto& mutableField = const_cast<FieldType&>(structType->field(0));
    mutableField.type = StorageType(Type { TypeKind::Ref, selfIndex });

    // hash() should terminate without infinite recursion.
    unsigned h = structType->hash();
    EXPECT_NE(h, 0u);

    // A self-referential struct should be structurally equal to itself.
    EXPECT_TRUE(WasmGCType::structurallyEqual(structType, structType));

    structType->destroy();
}

TEST(WasmGCType, MutuallyRecursiveStructEquality)
{
    // Create two pairs of mutually recursive structs:
    // A1 { field: ref B1 }, B1 { field: ref A1 }
    // A2 { field: ref B2 }, B2 { field: ref A2 }
    // Then verify A1 == A2 via bisimulation.

    FieldType placeholder[] = {
        { StorageType(Type { TypeKind::Ref, 0 }), Mutability::Mutable },
    };

    auto* a1 = WasmGCStructType::tryCreate(placeholder);
    auto* b1 = WasmGCStructType::tryCreate(placeholder);
    auto* a2 = WasmGCStructType::tryCreate(placeholder);
    auto* b2 = WasmGCStructType::tryCreate(placeholder);
    ASSERT_TRUE(a1 && b1 && a2 && b2);

    // Patch: A1.field -> B1, B1.field -> A1
    const_cast<FieldType&>(a1->field(0)).type = StorageType(Type { TypeKind::Ref, b1->index() });
    const_cast<FieldType&>(b1->field(0)).type = StorageType(Type { TypeKind::Ref, a1->index() });

    // Patch: A2.field -> B2, B2.field -> A2
    const_cast<FieldType&>(a2->field(0)).type = StorageType(Type { TypeKind::Ref, b2->index() });
    const_cast<FieldType&>(b2->field(0)).type = StorageType(Type { TypeKind::Ref, a2->index() });

    // A1 and A2 should be structurally equal (bisimulation).
    EXPECT_TRUE(WasmGCType::structurallyEqual(a1, a2));

    // B1 and B2 should also be structurally equal.
    EXPECT_TRUE(WasmGCType::structurallyEqual(b1, b2));

    // A1 and B1 are structurally identical too (both are struct { ref <other> }),
    // so they should be equal via bisimulation.
    EXPECT_TRUE(WasmGCType::structurallyEqual(a1, b1));

    a1->destroy();
    b1->destroy();
    a2->destroy();
    b2->destroy();
}

TEST(WasmGCType, SubtypeMetadata)
{
    auto* parent = createSimpleFunctionType();
    auto* child = createSimpleFunctionType();
    ASSERT_TRUE(parent && child);

    // Default state.
    EXPECT_TRUE(parent->isFinal());
    EXPECT_EQ(parent->supertype(), nullptr);

    // Set up subtyping.
    parent->setIsFinal(false);
    child->setSupertype(parent);
    child->setIsFinal(true);

    EXPECT_FALSE(parent->isFinal());
    EXPECT_EQ(child->supertype(), parent);
    EXPECT_TRUE(child->isFinal());

    // Two types with different finality/supertype should not be structurally equal.
    EXPECT_FALSE(WasmGCType::structurallyEqual(parent, child));

    parent->destroy();
    child->destroy();
}

// --- A. Function type edge cases ---

TEST(WasmGCType, FunctionTypeMultipleReturns)
{
    // (i32, f64) -> [i32, i64]
    auto* func = WasmGCFunctionType::tryCreate(2, 2);
    ASSERT_TRUE(func);

    func->getReturnType(0) = Types::I32;
    func->getReturnType(1) = Types::I64;
    func->getArgumentType(0) = Types::I32;
    func->getArgumentType(1) = Types::F64;

    EXPECT_EQ(func->argumentCount(), 2u);
    EXPECT_EQ(func->returnCount(), 2u);
    EXPECT_EQ(func->argumentType(0).kind, TypeKind::I32);
    EXPECT_EQ(func->argumentType(1).kind, TypeKind::F64);
    EXPECT_EQ(func->returnType(0).kind, TypeKind::I32);
    EXPECT_EQ(func->returnType(1).kind, TypeKind::I64);

    func->destroy();
}

TEST(WasmGCType, FunctionTypeNoArgsNoReturns)
{
    // () -> []
    auto* func = WasmGCFunctionType::tryCreate(0, 0);
    ASSERT_TRUE(func);

    EXPECT_EQ(func->argumentCount(), 0u);
    EXPECT_EQ(func->returnCount(), 0u);
    EXPECT_TRUE(func->is<WasmGCFunctionType>());

    func->destroy();
}

TEST(WasmGCType, FunctionTypeWithRefArgs)
{
    // (funcref, externref) -> [funcref]
    auto* func = WasmGCFunctionType::tryCreate(1, 2);
    ASSERT_TRUE(func);

    func->getReturnType(0) = Types::Funcref;
    func->getArgumentType(0) = Types::Funcref;
    func->getArgumentType(1) = Types::Externref;

    EXPECT_EQ(func->argumentType(0).kind, TypeKind::Funcref);
    EXPECT_EQ(func->argumentType(1).kind, TypeKind::Externref);
    EXPECT_EQ(func->returnType(0).kind, TypeKind::Funcref);

    // Two identical copies should be structurally equal.
    auto* func2 = WasmGCFunctionType::tryCreate(1, 2);
    ASSERT_TRUE(func2);
    func2->getReturnType(0) = Types::Funcref;
    func2->getArgumentType(0) = Types::Funcref;
    func2->getArgumentType(1) = Types::Externref;

    EXPECT_EQ(func->hash(), func2->hash());
    EXPECT_TRUE(WasmGCType::structurallyEqual(func, func2));

    func->destroy();
    func2->destroy();
}

// --- B. Struct type edge cases ---

TEST(WasmGCType, StructTypePackedFields)
{
    FieldType fields[] = {
        { StorageType(PackedType::I8), Mutability::Mutable },
        { StorageType(PackedType::I16), Mutability::Immutable },
    };

    auto* structType = WasmGCStructType::tryCreate(fields);
    ASSERT_TRUE(structType);

    EXPECT_EQ(structType->fieldCount(), 2u);
    EXPECT_TRUE(structType->field(0).type.is<PackedType>());
    EXPECT_EQ(structType->field(0).type.as<PackedType>(), PackedType::I8);
    EXPECT_TRUE(structType->field(1).type.is<PackedType>());
    EXPECT_EQ(structType->field(1).type.as<PackedType>(), PackedType::I16);

    // I8 = 1 byte at offset 0, I16 = 2 bytes aligned to 2 at offset 2.
    EXPECT_EQ(structType->offsetOfFieldInPayload(0), 0u);
    EXPECT_EQ(structType->offsetOfFieldInPayload(1), 2u);

    structType->destroy();
}

TEST(WasmGCType, StructTypeMutabilityPerField)
{
    FieldType allMutable[] = {
        { StorageType(Types::I32), Mutability::Mutable },
        { StorageType(Types::I64), Mutability::Mutable },
    };
    FieldType allImmutable[] = {
        { StorageType(Types::I32), Mutability::Immutable },
        { StorageType(Types::I64), Mutability::Immutable },
    };

    auto* sm = WasmGCStructType::tryCreate(allMutable);
    auto* si = WasmGCStructType::tryCreate(allImmutable);
    ASSERT_TRUE(sm && si);

    EXPECT_EQ(sm->field(0).mutability, Mutability::Mutable);
    EXPECT_EQ(sm->field(1).mutability, Mutability::Mutable);
    EXPECT_EQ(si->field(0).mutability, Mutability::Immutable);
    EXPECT_EQ(si->field(1).mutability, Mutability::Immutable);

    // They differ in mutability, so not structurally equal.
    EXPECT_FALSE(WasmGCType::structurallyEqual(sm, si));

    sm->destroy();
    si->destroy();
}

TEST(WasmGCType, StructTypeSingleField)
{
    FieldType fields[] = {
        { StorageType(Types::F32), Mutability::Immutable },
    };

    auto* structType = WasmGCStructType::tryCreate(fields);
    ASSERT_TRUE(structType);

    EXPECT_EQ(structType->fieldCount(), 1u);
    EXPECT_EQ(structType->field(0).type.as<Type>().kind, TypeKind::F32);
    EXPECT_EQ(structType->offsetOfFieldInPayload(0), 0u);

    structType->destroy();
}

TEST(WasmGCType, StructTypeFieldAlignment)
{
    // {i8, f64}: i8 is 1 byte, f64 is 8 bytes with 8-byte alignment.
    // Expected: i8 at offset 0, f64 at offset 8.
    FieldType fields[] = {
        { StorageType(PackedType::I8), Mutability::Mutable },
        { StorageType(Types::F64), Mutability::Mutable },
    };

    auto* structType = WasmGCStructType::tryCreate(fields);
    ASSERT_TRUE(structType);

    EXPECT_EQ(structType->offsetOfFieldInPayload(0), 0u);
    EXPECT_EQ(structType->offsetOfFieldInPayload(1), 8u);

    structType->destroy();
}

// --- C. Array type edge cases ---

TEST(WasmGCType, ArrayTypePackedElement)
{
    FieldType elemType = { StorageType(PackedType::I8), Mutability::Mutable };
    auto* arrayType = WasmGCArrayType::tryCreate(elemType);
    ASSERT_TRUE(arrayType);

    EXPECT_TRUE(arrayType->elementType().type.is<PackedType>());
    EXPECT_EQ(arrayType->elementType().type.as<PackedType>(), PackedType::I8);
    EXPECT_EQ(arrayType->elementType().mutability, Mutability::Mutable);

    arrayType->destroy();
}

TEST(WasmGCType, ArrayTypeRefElement)
{
    // Create a struct, then an array whose element is ref null <struct>.
    FieldType structFields[] = {
        { StorageType(Types::I32), Mutability::Mutable },
    };
    auto* structType = WasmGCStructType::tryCreate(structFields);
    ASSERT_TRUE(structType);

    FieldType elemType = { StorageType(Type { TypeKind::RefNull, structType->index() }), Mutability::Immutable };
    auto* arrayType = WasmGCArrayType::tryCreate(elemType);
    ASSERT_TRUE(arrayType);

    EXPECT_TRUE(arrayType->elementType().type.is<Type>());
    EXPECT_EQ(arrayType->elementType().type.as<Type>().kind, TypeKind::RefNull);
    EXPECT_EQ(arrayType->elementType().type.as<Type>().index, structType->index());

    arrayType->destroy();
    structType->destroy();
}

// --- D. Cross-kind inequality ---

TEST(WasmGCType, CrossKindInequality)
{
    auto* func = createSimpleFunctionType();
    FieldType structFields[] = { { StorageType(Types::I32), Mutability::Mutable } };
    auto* structType = WasmGCStructType::tryCreate(structFields);
    FieldType elemType = { StorageType(Types::I32), Mutability::Mutable };
    auto* arrayType = WasmGCArrayType::tryCreate(elemType);
    ASSERT_TRUE(func && structType && arrayType);

    EXPECT_FALSE(WasmGCType::structurallyEqual(func, structType));
    EXPECT_FALSE(WasmGCType::structurallyEqual(func, arrayType));
    EXPECT_FALSE(WasmGCType::structurallyEqual(structType, arrayType));

    func->destroy();
    structType->destroy();
    arrayType->destroy();
}

// --- E. Hash equality/inequality across type kinds ---

TEST(WasmGCType, StructHashEquality)
{
    FieldType fields[] = {
        { StorageType(Types::I32), Mutability::Mutable },
        { StorageType(Types::F64), Mutability::Immutable },
    };

    auto* s1 = WasmGCStructType::tryCreate(fields);
    auto* s2 = WasmGCStructType::tryCreate(fields);
    ASSERT_TRUE(s1 && s2);

    EXPECT_EQ(s1->hash(), s2->hash());
    EXPECT_TRUE(WasmGCType::structurallyEqual(s1, s2));

    s1->destroy();
    s2->destroy();
}

TEST(WasmGCType, ArrayHashEquality)
{
    FieldType elemType = { StorageType(Types::F64), Mutability::Immutable };

    auto* a1 = WasmGCArrayType::tryCreate(elemType);
    auto* a2 = WasmGCArrayType::tryCreate(elemType);
    ASSERT_TRUE(a1 && a2);

    EXPECT_EQ(a1->hash(), a2->hash());
    EXPECT_TRUE(WasmGCType::structurallyEqual(a1, a2));

    a1->destroy();
    a2->destroy();
}

TEST(WasmGCType, StructHashInequality)
{
    FieldType fields1[] = { { StorageType(Types::I32), Mutability::Mutable } };
    FieldType fields2[] = { { StorageType(Types::I32), Mutability::Immutable } };

    auto* s1 = WasmGCStructType::tryCreate(fields1);
    auto* s2 = WasmGCStructType::tryCreate(fields2);
    ASSERT_TRUE(s1 && s2);

    EXPECT_FALSE(WasmGCType::structurallyEqual(s1, s2));

    s1->destroy();
    s2->destroy();
}

TEST(WasmGCType, ArrayHashInequality)
{
    FieldType elem1 = { StorageType(Types::I32), Mutability::Mutable };
    FieldType elem2 = { StorageType(Types::F64), Mutability::Mutable };

    auto* a1 = WasmGCArrayType::tryCreate(elem1);
    auto* a2 = WasmGCArrayType::tryCreate(elem2);
    ASSERT_TRUE(a1 && a2);

    EXPECT_FALSE(WasmGCType::structurallyEqual(a1, a2));

    a1->destroy();
    a2->destroy();
}

// --- F. Packed type equality/inequality ---

TEST(WasmGCType, PackedFieldEquality)
{
    FieldType fields1[] = {
        { StorageType(PackedType::I8), Mutability::Mutable },
        { StorageType(PackedType::I16), Mutability::Immutable },
    };
    FieldType fields2[] = {
        { StorageType(PackedType::I8), Mutability::Mutable },
        { StorageType(PackedType::I16), Mutability::Immutable },
    };

    auto* s1 = WasmGCStructType::tryCreate(fields1);
    auto* s2 = WasmGCStructType::tryCreate(fields2);
    ASSERT_TRUE(s1 && s2);

    EXPECT_EQ(s1->hash(), s2->hash());
    EXPECT_TRUE(WasmGCType::structurallyEqual(s1, s2));

    s1->destroy();
    s2->destroy();
}

TEST(WasmGCType, PackedFieldInequality)
{
    FieldType fields1[] = { { StorageType(PackedType::I8), Mutability::Mutable } };
    FieldType fields2[] = { { StorageType(PackedType::I16), Mutability::Mutable } };

    auto* s1 = WasmGCStructType::tryCreate(fields1);
    auto* s2 = WasmGCStructType::tryCreate(fields2);
    ASSERT_TRUE(s1 && s2);

    EXPECT_FALSE(WasmGCType::structurallyEqual(s1, s2));

    s1->destroy();
    s2->destroy();
}

TEST(WasmGCType, PackedVsUnpackedInequality)
{
    // PackedType::I8 vs Type::I32 — different StorageType variants.
    FieldType fields1[] = { { StorageType(PackedType::I8), Mutability::Mutable } };
    FieldType fields2[] = { { StorageType(Types::I32), Mutability::Mutable } };

    auto* s1 = WasmGCStructType::tryCreate(fields1);
    auto* s2 = WasmGCStructType::tryCreate(fields2);
    ASSERT_TRUE(s1 && s2);

    EXPECT_FALSE(WasmGCType::structurallyEqual(s1, s2));

    s1->destroy();
    s2->destroy();
}

// --- G. Recursive type edge cases ---

TEST(WasmGCType, SelfReferentialVsNonRecursive)
{
    // Self-referential: struct S { ref S }
    FieldType placeholder[] = { { StorageType(Type { TypeKind::Ref, 0 }), Mutability::Mutable } };
    auto* selfRef = WasmGCStructType::tryCreate(placeholder);
    ASSERT_TRUE(selfRef);
    const_cast<FieldType&>(selfRef->field(0)).type = StorageType(Type { TypeKind::Ref, selfRef->index() });

    // Non-recursive: struct T { ref U } where U is a separate non-recursive struct.
    FieldType uFields[] = { { StorageType(Types::I32), Mutability::Mutable } };
    auto* u = WasmGCStructType::tryCreate(uFields);
    ASSERT_TRUE(u);

    auto* nonRecursive = WasmGCStructType::tryCreate(placeholder);
    ASSERT_TRUE(nonRecursive);
    const_cast<FieldType&>(nonRecursive->field(0)).type = StorageType(Type { TypeKind::Ref, u->index() });

    // Self-referential struct and non-recursive struct should NOT be equal.
    // selfRef's field references a StructType (itself), nonRecursive's field
    // references a different StructType (u) which has a different structure (i32 field).
    EXPECT_FALSE(WasmGCType::structurallyEqual(selfRef, nonRecursive));

    selfRef->destroy();
    nonRecursive->destroy();
    u->destroy();
}

TEST(WasmGCType, DifferentRecursiveStructures)
{
    // A { i32, ref A } vs B { f64, ref B }
    // Both self-recursive but differ in the first field.
    FieldType placeholderA[] = {
        { StorageType(Types::I32), Mutability::Mutable },
        { StorageType(Type { TypeKind::Ref, 0 }), Mutability::Mutable },
    };
    FieldType placeholderB[] = {
        { StorageType(Types::F64), Mutability::Mutable },
        { StorageType(Type { TypeKind::Ref, 0 }), Mutability::Mutable },
    };

    auto* a = WasmGCStructType::tryCreate(placeholderA);
    auto* b = WasmGCStructType::tryCreate(placeholderB);
    ASSERT_TRUE(a && b);

    const_cast<FieldType&>(a->field(1)).type = StorageType(Type { TypeKind::Ref, a->index() });
    const_cast<FieldType&>(b->field(1)).type = StorageType(Type { TypeKind::Ref, b->index() });

    EXPECT_FALSE(WasmGCType::structurallyEqual(a, b));

    a->destroy();
    b->destroy();
}

TEST(WasmGCType, RecursiveFunctionType)
{
    // Function type: (ref self) -> []
    auto* func1 = WasmGCFunctionType::tryCreate(0, 1);
    auto* func2 = WasmGCFunctionType::tryCreate(0, 1);
    ASSERT_TRUE(func1 && func2);

    func1->getArgumentType(0) = Type { TypeKind::Ref, func1->index() };
    func2->getArgumentType(0) = Type { TypeKind::Ref, func2->index() };

    // Hash should terminate.
    unsigned h1 = func1->hash();
    unsigned h2 = func2->hash();
    EXPECT_EQ(h1, h2);

    // Two isomorphic self-recursive function types should be equal.
    EXPECT_TRUE(WasmGCType::structurallyEqual(func1, func2));

    func1->destroy();
    func2->destroy();
}

TEST(WasmGCType, MixedRecursiveGroup)
{
    // Struct S { ref F } and Function F (ref S) -> []
    // forming a mutual cycle across different type kinds.
    // Create two isomorphic copies and verify equality.

    FieldType placeholder[] = { { StorageType(Type { TypeKind::Ref, 0 }), Mutability::Mutable } };

    auto* s1 = WasmGCStructType::tryCreate(placeholder);
    auto* f1 = WasmGCFunctionType::tryCreate(0, 1);
    auto* s2 = WasmGCStructType::tryCreate(placeholder);
    auto* f2 = WasmGCFunctionType::tryCreate(0, 1);
    ASSERT_TRUE(s1 && f1 && s2 && f2);

    // Wire up copy 1: S1.field -> F1, F1.arg -> S1
    const_cast<FieldType&>(s1->field(0)).type = StorageType(Type { TypeKind::Ref, f1->index() });
    f1->getArgumentType(0) = Type { TypeKind::Ref, s1->index() };

    // Wire up copy 2: S2.field -> F2, F2.arg -> S2
    const_cast<FieldType&>(s2->field(0)).type = StorageType(Type { TypeKind::Ref, f2->index() });
    f2->getArgumentType(0) = Type { TypeKind::Ref, s2->index() };

    EXPECT_TRUE(WasmGCType::structurallyEqual(s1, s2));
    EXPECT_TRUE(WasmGCType::structurallyEqual(f1, f2));

    // S1 and F1 should NOT be equal (different kinds).
    EXPECT_FALSE(WasmGCType::structurallyEqual(s1, f1));

    s1->destroy();
    f1->destroy();
    s2->destroy();
    f2->destroy();
}

// --- H. Supertype edge cases ---

TEST(WasmGCType, SupertypeChain)
{
    // grandparent -> parent -> child
    // child-with-parent != child-with-grandparent
    auto* grandparent = createSimpleFunctionType();
    auto* parent = createSimpleFunctionType();
    auto* child1 = createSimpleFunctionType();
    auto* child2 = createSimpleFunctionType();
    ASSERT_TRUE(grandparent && parent && child1 && child2);

    grandparent->setIsFinal(false);
    parent->setIsFinal(false);
    parent->setSupertype(grandparent);

    child1->setSupertype(parent);
    child2->setSupertype(grandparent);

    // child1 has supertype=parent, child2 has supertype=grandparent.
    // They are structurally identical except for their supertype pointers,
    // so they should NOT be equal.
    EXPECT_FALSE(WasmGCType::structurallyEqual(child1, child2));

    grandparent->destroy();
    parent->destroy();
    child1->destroy();
    child2->destroy();
}

TEST(WasmGCType, SupertypeInRecursiveType)
{
    // A self-recursive struct with a supertype set.
    FieldType placeholderParent[] = { { StorageType(Type { TypeKind::Ref, 0 }), Mutability::Mutable } };
    auto* parent = WasmGCStructType::tryCreate(placeholderParent);
    ASSERT_TRUE(parent);
    const_cast<FieldType&>(parent->field(0)).type = StorageType(Type { TypeKind::Ref, parent->index() });
    parent->setIsFinal(false);

    FieldType placeholderChild[] = { { StorageType(Type { TypeKind::Ref, 0 }), Mutability::Mutable } };
    auto* child = WasmGCStructType::tryCreate(placeholderChild);
    ASSERT_TRUE(child);
    const_cast<FieldType&>(child->field(0)).type = StorageType(Type { TypeKind::Ref, child->index() });
    child->setSupertype(parent);

    // Hash should terminate despite recursion + supertype.
    unsigned h = child->hash();
    EXPECT_NE(h, 0u);

    // Self-equality should hold.
    EXPECT_TRUE(WasmGCType::structurallyEqual(child, child));

    // Create an isomorphic copy.
    auto* parent2 = WasmGCStructType::tryCreate(placeholderParent);
    ASSERT_TRUE(parent2);
    const_cast<FieldType&>(parent2->field(0)).type = StorageType(Type { TypeKind::Ref, parent2->index() });
    parent2->setIsFinal(false);

    auto* child2 = WasmGCStructType::tryCreate(placeholderChild);
    ASSERT_TRUE(child2);
    const_cast<FieldType&>(child2->field(0)).type = StorageType(Type { TypeKind::Ref, child2->index() });
    child2->setSupertype(parent2);

    // Isomorphic copies should be structurally equal.
    EXPECT_TRUE(WasmGCType::structurallyEqual(child, child2));

    parent->destroy();
    child->destroy();
    parent2->destroy();
    child2->destroy();
}

TEST(WasmGCType, NullVsNonNullSupertype)
{
    auto* t1 = createSimpleFunctionType();
    auto* t2 = createSimpleFunctionType();
    auto* parent = createSimpleFunctionType();
    ASSERT_TRUE(t1 && t2 && parent);

    parent->setIsFinal(false);

    // t1 has no supertype (default), t2 has a supertype.
    t2->setSupertype(parent);

    EXPECT_FALSE(WasmGCType::structurallyEqual(t1, t2));

    t1->destroy();
    t2->destroy();
    parent->destroy();
}

// --- I. Null/edge cases for structurallyEqual ---

TEST(WasmGCType, StructurallyEqualNullNull)
{
    EXPECT_TRUE(WasmGCType::structurallyEqual(nullptr, nullptr));
}

TEST(WasmGCType, StructurallyEqualNullNonNull)
{
    auto* func = createSimpleFunctionType();
    ASSERT_TRUE(func);

    EXPECT_FALSE(WasmGCType::structurallyEqual(nullptr, func));
    EXPECT_FALSE(WasmGCType::structurallyEqual(func, nullptr));

    func->destroy();
}

TEST(WasmGCType, StructurallyEqualSamePointer)
{
    auto* func = createSimpleFunctionType();
    ASSERT_TRUE(func);

    EXPECT_TRUE(WasmGCType::structurallyEqual(func, func));

    func->destroy();
}

// --- J. Destroy ---

TEST(WasmGCType, DestroyAllKinds)
{
    auto* func = WasmGCFunctionType::tryCreate(0, 0);
    ASSERT_TRUE(func);

    FieldType structFields[] = { { StorageType(Types::I32), Mutability::Mutable } };
    auto* structType = WasmGCStructType::tryCreate(structFields);
    ASSERT_TRUE(structType);

    FieldType elemType = { StorageType(Types::F64), Mutability::Immutable };
    auto* arrayType = WasmGCArrayType::tryCreate(elemType);
    ASSERT_TRUE(arrayType);

    // All three kinds should destroy without crashing.
    func->destroy();
    structType->destroy();
    arrayType->destroy();
}

} // namespace TestWebKitAPI

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(WEBASSEMBLY)
