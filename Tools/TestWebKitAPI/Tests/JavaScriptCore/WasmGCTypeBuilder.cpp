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
#include <JavaScriptCore/WasmGCTypeBuilder.h>
#include <JavaScriptCore/WasmGCTypeRegistry.h>
#include <JavaScriptCore/WasmOps.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace TestWebKitAPI {

using namespace JSC::Wasm;

// ---------------------------------------------------------------------------
// Test plan
//
// A. Placeholder patching — patchPlaceholders resolves forward references
//    in all type kinds (struct field, function arg, function return, array
//    element), handles self-recursive and mutually recursive groups, skips
//    non-ref fields and abstract heap type refs.
//
// B. walkTypeReferences — visits exactly the concrete TypeIndex references
//    in each type kind, skips non-ref and abstract refs, callback mutations
//    are written back correctly (including the StorageType roundtrip for
//    struct/array).
//
// C. Deduplication — structurally equal types are replaced with canonical
//    pointers, novel types survive, surviving types' refs and supertypes
//    are patched when their referent was deduplicated. Covers all three
//    type kinds and cross-kind non-matching.
//
// D. Root set lifecycle — types are findable after registration, collected
//    after deregistration. Multiple groups accumulate into one root set.
//
// E. Simulated concurrent scenarios — interleaved operations that model
//    races between multiple parsers registering the same type, a module
//    unloading while a parser is between groups, deduplication followed by
//    unload of the original provider, and recursive group dedup across
//    modules. Verifies that the atomicity of deduplicateAndRegister
//    prevents dangling pointers and duplicate canonical types.
//
// F. RTT registration — RTTs are created for all type kinds after dedup,
//    registration is idempotent, supertype RTT hierarchy is correct.
//
// G. Placeholder utilities — round-trip consistency, zero is not a
//    placeholder, builtin TypeKind indices are not placeholders.
//
// H. Edge cases — empty groups, all-match groups, no-match groups,
//    gcTypeSignatures accumulation across groups.
//
// I. Combinatorial interactions — dependency chains between operations:
//    cross-group ref to a deduplicated type, cross-group ref surviving
//    provider unload, supertype from earlier group with provider unload,
//    supertype chain with partial dedup, recursive group with partial
//    dedup and mixed ref patching, two modules sharing recursive groups
//    with sequential unload, array/function refs to deduplicated types,
//    independent parsers with no crosstalk.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static WasmGCFunctionType* createFuncType_I32_to_I32()
{
    auto* func = WasmGCFunctionType::tryCreate(1, 1);
    EXPECT_TRUE(func);
    func->getReturnType(0) = Types::I32;
    func->getArgumentType(0) = Types::I32;
    return func;
}

static WasmGCFunctionType* createFuncType_I32I32_to_I32()
{
    auto* func = WasmGCFunctionType::tryCreate(1, 2);
    EXPECT_TRUE(func);
    func->getReturnType(0) = Types::I32;
    func->getArgumentType(0) = Types::I32;
    func->getArgumentType(1) = Types::I32;
    return func;
}

static WasmGCFunctionType* createFuncType_F64_to_F64()
{
    auto* func = WasmGCFunctionType::tryCreate(1, 1);
    EXPECT_TRUE(func);
    func->getReturnType(0) = Types::F64;
    func->getArgumentType(0) = Types::F64;
    return func;
}

static WasmGCStructType* createRefStruct(TypeIndex refTarget)
{
    FieldType fields[] = {
        { StorageType(Type { TypeKind::Ref, refTarget }), Mutability::Mutable },
    };
    return WasmGCStructType::tryCreate(fields);
}

static WasmGCStructType* createI32Struct()
{
    FieldType fields[] = {
        { StorageType(Types::I32), Mutability::Mutable },
    };
    return WasmGCStructType::tryCreate(fields);
}

static WasmGCStructType* createI32F64Struct()
{
    FieldType fields[] = {
        { StorageType(Types::I32), Mutability::Mutable },
        { StorageType(Types::F64), Mutability::Immutable },
    };
    return WasmGCStructType::tryCreate(fields);
}

static WasmGCArrayType* createI32Array()
{
    FieldType elementType = { StorageType(Types::I32), Mutability::Mutable };
    return WasmGCArrayType::tryCreate(elementType);
}

static WasmGCArrayType* createRefArray(TypeIndex refTarget)
{
    FieldType elementType = { StorageType(Type { TypeKind::Ref, refTarget }), Mutability::Mutable };
    return WasmGCArrayType::tryCreate(elementType);
}

// Simulate what a parser does for a single group: build types, patch
// placeholders, set metadata, then deduplicateAndRegister.
static void registerSingletonGroup(WasmGCType* type, Vector<WasmGCType*>& sigs, WasmGCTypeRootSet& rootSet)
{
    WasmGCType* group[] = { type };
    WasmGCTypeBuilder::patchPlaceholders(group);
    WasmGCTypeBuilder::deduplicateAndRegister(group, sigs, rootSet);
}

// =========================================================================
// A. Placeholder patching
// =========================================================================

TEST(WasmGCTypeBuilder, PatchPlaceholders)
{
    // Verify that a placeholder TypeIndex in a struct field is replaced with
    // the actual pointer of the referenced group member after patching.
    auto* func = createFuncType_I32_to_I32();
    ASSERT_TRUE(func);

    TypeIndex placeholder = WasmGCTypeBuilder::placeholderForGroupIndex(0);
    auto* structType = createRefStruct(placeholder);
    ASSERT_TRUE(structType);

    WasmGCType* groupTypes[] = { func, structType };
    WasmGCTypeBuilder::patchPlaceholders(groupTypes);

    ASSERT_TRUE(structType->field(0).type.is<Type>());
    EXPECT_EQ(structType->field(0).type.as<Type>().index, func->index());

    func->destroy();
    structType->destroy();
}

TEST(WasmGCTypeBuilder, PatchPlaceholderInFunctionArg)
{
    // Function type: (ref <placeholder 0>) -> i32
    // Group: [struct{i32}, func]
    auto* structType = createI32Struct();
    ASSERT_TRUE(structType);

    auto* func = WasmGCFunctionType::tryCreate(1, 1);
    ASSERT_TRUE(func);
    func->getReturnType(0) = Types::I32;
    func->getArgumentType(0) = Type { TypeKind::Ref, WasmGCTypeBuilder::placeholderForGroupIndex(0) };

    WasmGCType* groupTypes[] = { structType, func };
    WasmGCTypeBuilder::patchPlaceholders(groupTypes);

    EXPECT_EQ(func->argumentType(0).index, structType->index());

    structType->destroy();
    func->destroy();
}

TEST(WasmGCTypeBuilder, PatchPlaceholderInFunctionReturn)
{
    // Function type: () -> ref <placeholder 0>
    auto* structType = createI32Struct();
    ASSERT_TRUE(structType);

    auto* func = WasmGCFunctionType::tryCreate(1, 0);
    ASSERT_TRUE(func);
    func->getReturnType(0) = Type { TypeKind::Ref, WasmGCTypeBuilder::placeholderForGroupIndex(0) };

    WasmGCType* groupTypes[] = { structType, func };
    WasmGCTypeBuilder::patchPlaceholders(groupTypes);

    EXPECT_EQ(func->returnType(0).index, structType->index());

    structType->destroy();
    func->destroy();
}

TEST(WasmGCTypeBuilder, PatchPlaceholderInArrayElement)
{
    // Array of ref <placeholder 0>, group: [struct{i32}, array]
    auto* structType = createI32Struct();
    ASSERT_TRUE(structType);

    auto* arrayType = createRefArray(WasmGCTypeBuilder::placeholderForGroupIndex(0));
    ASSERT_TRUE(arrayType);

    WasmGCType* groupTypes[] = { structType, arrayType };
    WasmGCTypeBuilder::patchPlaceholders(groupTypes);

    ASSERT_TRUE(arrayType->elementType().type.is<Type>());
    EXPECT_EQ(arrayType->elementType().type.as<Type>().index, structType->index());

    structType->destroy();
    arrayType->destroy();
}

TEST(WasmGCTypeBuilder, SelfRecursiveStruct)
{
    // Verify that a struct with a ref field pointing to itself (via placeholder
    // index 0) has the placeholder resolved to its own pointer after patching.
    TypeIndex placeholder = WasmGCTypeBuilder::placeholderForGroupIndex(0);
    auto* s = createRefStruct(placeholder);
    ASSERT_TRUE(s);

    WasmGCType* groupTypes[] = { s };
    WasmGCTypeBuilder::patchPlaceholders(groupTypes);

    ASSERT_TRUE(s->field(0).type.is<Type>());
    EXPECT_EQ(s->field(0).type.as<Type>().index, s->index());

    s->destroy();
}

TEST(WasmGCTypeBuilder, MutuallyRecursiveStructs)
{
    // Verify that two structs referencing each other via placeholders
    // (A{ref B}, B{ref A}) are correctly cross-linked after patching.
    TypeIndex phA = WasmGCTypeBuilder::placeholderForGroupIndex(0);
    TypeIndex phB = WasmGCTypeBuilder::placeholderForGroupIndex(1);

    auto* a = createRefStruct(phB);
    auto* b = createRefStruct(phA);
    ASSERT_TRUE(a && b);

    WasmGCType* groupTypes[] = { a, b };
    WasmGCTypeBuilder::patchPlaceholders(groupTypes);

    EXPECT_EQ(a->field(0).type.as<Type>().index, b->index());
    EXPECT_EQ(b->field(0).type.as<Type>().index, a->index());

    a->destroy();
    b->destroy();
}

TEST(WasmGCTypeBuilder, ThreeWayRecursiveGroup)
{
    // A{ref B}, B{ref C}, C{ref A}
    TypeIndex phA = WasmGCTypeBuilder::placeholderForGroupIndex(0);
    TypeIndex phB = WasmGCTypeBuilder::placeholderForGroupIndex(1);
    TypeIndex phC = WasmGCTypeBuilder::placeholderForGroupIndex(2);

    auto* a = createRefStruct(phB);
    auto* b = createRefStruct(phC);
    auto* c = createRefStruct(phA);
    ASSERT_TRUE(a && b && c);

    WasmGCType* groupTypes[] = { a, b, c };
    WasmGCTypeBuilder::patchPlaceholders(groupTypes);

    EXPECT_EQ(a->field(0).type.as<Type>().index, b->index());
    EXPECT_EQ(b->field(0).type.as<Type>().index, c->index());
    EXPECT_EQ(c->field(0).type.as<Type>().index, a->index());

    a->destroy();
    b->destroy();
    c->destroy();
}

TEST(WasmGCTypeBuilder, PatchSkipsNonRefFields)
{
    // Struct with i32 and f64 fields (no ref types). Patching should be a no-op.
    auto* s = createI32F64Struct();
    ASSERT_TRUE(s);

    WasmGCType* groupTypes[] = { s };
    WasmGCTypeBuilder::patchPlaceholders(groupTypes);

    EXPECT_TRUE(s->field(0).type.is<Type>());
    EXPECT_EQ(s->field(0).type.as<Type>().index, Types::I32.index);
    EXPECT_TRUE(s->field(1).type.is<Type>());
    EXPECT_EQ(s->field(1).type.as<Type>().index, Types::F64.index);

    s->destroy();
}

TEST(WasmGCTypeBuilder, PatchSkipsAbstractHeapTypeRefs)
{
    // Function with funcref argument — this is a ref type but its index is
    // a builtin TypeKind, not a placeholder. Patching must not touch it.
    auto* func = WasmGCFunctionType::tryCreate(0, 1);
    ASSERT_TRUE(func);
    func->getArgumentType(0) = Types::Funcref;

    WasmGCType* groupTypes[] = { func };
    WasmGCTypeBuilder::patchPlaceholders(groupTypes);

    EXPECT_EQ(func->argumentType(0).index, Types::Funcref.index);

    func->destroy();
}

// =========================================================================
// B. walkTypeReferences
// =========================================================================

TEST(WasmGCTypeBuilder, WalkFunctionTypeReferences)
{
    // Verify that walkTypeReferences visits both the return type and argument
    // type references of a function type, and nothing else.
    // (ref <someStruct>) -> ref <someStruct>
    auto* structType = createI32Struct();
    ASSERT_TRUE(structType);

    auto* func = WasmGCFunctionType::tryCreate(1, 1);
    ASSERT_TRUE(func);
    func->getReturnType(0) = Type { TypeKind::Ref, structType->index() };
    func->getArgumentType(0) = Type { TypeKind::Ref, structType->index() };

    unsigned count = 0;
    WasmGCTypeBuilder::walkTypeReferences(func, [&](TypeIndex& idx) {
        EXPECT_EQ(idx, structType->index());
        ++count;
    });
    EXPECT_EQ(count, 2u); // one return + one argument

    structType->destroy();
    func->destroy();
}

TEST(WasmGCTypeBuilder, WalkStructTypeReferences)
{
    // Verify that walkTypeReferences visits only ref-typed fields in a struct,
    // skipping non-ref fields like i32 and f64.
    auto* target = createI32Struct();
    ASSERT_TRUE(target);

    // Struct with: i32, ref <target>, f64
    FieldType fields[] = {
        { StorageType(Types::I32), Mutability::Mutable },
        { StorageType(Type { TypeKind::Ref, target->index() }), Mutability::Mutable },
        { StorageType(Types::F64), Mutability::Immutable },
    };
    auto* s = WasmGCStructType::tryCreate(fields);
    ASSERT_TRUE(s);

    unsigned count = 0;
    WasmGCTypeBuilder::walkTypeReferences(s, [&](TypeIndex& idx) {
        EXPECT_EQ(idx, target->index());
        ++count;
    });
    EXPECT_EQ(count, 1u); // only the ref field

    target->destroy();
    s->destroy();
}

TEST(WasmGCTypeBuilder, WalkArrayTypeReferences)
{
    // Verify that walkTypeReferences visits the element type reference
    // of an array with a ref element type.
    auto* target = createI32Struct();
    ASSERT_TRUE(target);

    auto* arr = createRefArray(target->index());
    ASSERT_TRUE(arr);

    unsigned count = 0;
    WasmGCTypeBuilder::walkTypeReferences(arr, [&](TypeIndex& idx) {
        EXPECT_EQ(idx, target->index());
        ++count;
    });
    EXPECT_EQ(count, 1u);

    target->destroy();
    arr->destroy();
}

TEST(WasmGCTypeBuilder, WalkDoesNotVisitNonRefFields)
{
    // Verify that walkTypeReferences does not invoke the callback for a struct
    // whose fields are all non-ref types (i32, f64).
    auto* s = createI32F64Struct();
    ASSERT_TRUE(s);

    unsigned count = 0;
    WasmGCTypeBuilder::walkTypeReferences(s, [&](TypeIndex&) {
        ++count;
    });
    EXPECT_EQ(count, 0u);

    s->destroy();
}

TEST(WasmGCTypeBuilder, WalkMutatesIndex)
{
    // Verify that mutations to TypeIndex& in the walk callback are correctly
    // written back to the type, including the StorageType roundtrip for arrays.
    auto* target1 = createI32Struct();
    auto* target2 = createI32F64Struct();
    ASSERT_TRUE(target1 && target2);

    auto* arr = createRefArray(target1->index());
    ASSERT_TRUE(arr);

    // Replace target1 with target2 via the walk callback.
    WasmGCTypeBuilder::walkTypeReferences(arr, [&](TypeIndex& idx) {
        if (idx == target1->index())
            idx = target2->index();
    });

    EXPECT_EQ(arr->elementType().type.as<Type>().index, target2->index());

    target1->destroy();
    target2->destroy();
    arr->destroy();
}

// =========================================================================
// C. Deduplication basics
// =========================================================================

TEST(WasmGCTypeBuilder, DeduplicationFindsMatch)
{
    // Verify that deduplicateAndRegister replaces a tentative type with the
    // canonical pointer when a structurally equal type is already registered.
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* canonical = createFuncType_I32_to_I32();
    ASSERT_TRUE(canonical);

    WasmGCTypeRootSet canonicalRootSet;
    canonicalRootSet.append(canonical);
    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&canonicalRootSet);
    }

    auto* tentative = createFuncType_I32_to_I32();
    ASSERT_TRUE(tentative);
    ASSERT_NE(tentative, canonical);

    Vector<WasmGCType*> sigs;
    WasmGCTypeRootSet rootSet;
    WasmGCType* group[] = { tentative };
    WasmGCTypeBuilder::deduplicateAndRegister(group, sigs, rootSet);

    EXPECT_EQ(group[0], canonical);
    EXPECT_EQ(sigs.size(), 1u);
    EXPECT_EQ(sigs[0], canonical);

    registry.deregisterRootSet(&rootSet);
    registry.deregisterRootSet(&canonicalRootSet);
}

TEST(WasmGCTypeBuilder, DeduplicationNewType)
{
    // Verify that a novel type (no match in the registry) survives deduplication,
    // is registered, and can be found by a subsequent lookup.
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* novel = createFuncType_I32I32_to_I32();
    ASSERT_TRUE(novel);

    Vector<WasmGCType*> sigs;
    WasmGCTypeRootSet rootSet;
    WasmGCType* group[] = { novel };
    WasmGCTypeBuilder::deduplicateAndRegister(group, sigs, rootSet);

    EXPECT_EQ(group[0], novel);
    EXPECT_EQ(sigs.size(), 1u);
    EXPECT_EQ(sigs[0], novel);

    auto* probe = createFuncType_I32I32_to_I32();
    {
        Locker locker { registry.lock() };
        EXPECT_EQ(registry.findType(probe), novel);
    }
    probe->destroy();

    registry.deregisterRootSet(&rootSet);
}

TEST(WasmGCTypeBuilder, DeduplicationPatchesSurvivors)
{
    // Verify that when a group member is deduplicated, surviving members that
    // reference it via TypeIndex have their references patched to the canonical.
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* canonical = createFuncType_I32_to_I32();
    ASSERT_TRUE(canonical);
    WasmGCTypeRootSet canonicalRootSet;
    canonicalRootSet.append(canonical);
    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&canonicalRootSet);
    }

    // Group: [tentativeFunc, struct{ ref tentativeFunc }]
    auto* tentativeFunc = createFuncType_I32_to_I32();
    ASSERT_TRUE(tentativeFunc);

    FieldType fields[] = {
        { StorageType(Type { TypeKind::Ref, tentativeFunc->index() }), Mutability::Mutable },
    };
    auto* structType = WasmGCStructType::tryCreate(fields);
    ASSERT_TRUE(structType);

    Vector<WasmGCType*> sigs;
    WasmGCTypeRootSet rootSet;
    WasmGCType* group[] = { tentativeFunc, structType };
    WasmGCTypeBuilder::deduplicateAndRegister(group, sigs, rootSet);

    // tentativeFunc replaced with canonical.
    EXPECT_EQ(group[0], canonical);
    // structType survives; its ref field now points to canonical.
    EXPECT_EQ(group[1], structType);
    ASSERT_TRUE(structType->field(0).type.is<Type>());
    EXPECT_EQ(structType->field(0).type.as<Type>().index, canonical->index());

    registry.deregisterRootSet(&rootSet);
    registry.deregisterRootSet(&canonicalRootSet);
}

TEST(WasmGCTypeBuilder, DeduplicationPatchesSupertype)
{
    // If a type's supertype is deduplicated, the surviving type should have
    // its supertype pointer patched to the canonical one.
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* canonicalParent = createFuncType_I32_to_I32();
    ASSERT_TRUE(canonicalParent);
    WasmGCTypeRootSet parentRootSet;
    parentRootSet.append(canonicalParent);
    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&parentRootSet);
    }

    // Create a tentative copy of parent + a child with supertype = tentative parent.
    auto* tentativeParent = createFuncType_I32_to_I32();
    ASSERT_TRUE(tentativeParent);

    auto* child = createFuncType_I32_to_I32();
    ASSERT_TRUE(child);
    child->setSupertype(tentativeParent);
    child->setIsFinal(false);

    Vector<WasmGCType*> sigs;
    WasmGCTypeRootSet rootSet;
    WasmGCType* group[] = { tentativeParent, child };
    WasmGCTypeBuilder::deduplicateAndRegister(group, sigs, rootSet);

    // tentativeParent replaced with canonicalParent.
    EXPECT_EQ(group[0], canonicalParent);
    // child survives but its supertype should now point to canonicalParent.
    EXPECT_EQ(child->supertype(), canonicalParent);

    registry.deregisterRootSet(&rootSet);
    registry.deregisterRootSet(&parentRootSet);
}

TEST(WasmGCTypeBuilder, DeduplicationStructTypes)
{
    // Verify that struct type deduplication works: a tentative struct{i32}
    // is replaced by a canonical struct{i32} already in the registry.
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* canonical = createI32Struct();
    ASSERT_TRUE(canonical);

    Vector<WasmGCType*> sigs1;
    WasmGCTypeRootSet rootSet1;
    registerSingletonGroup(canonical, sigs1, rootSet1);

    auto* tentative = createI32Struct();
    ASSERT_TRUE(tentative);
    ASSERT_NE(tentative, canonical);

    Vector<WasmGCType*> sigs2;
    WasmGCTypeRootSet rootSet2;
    WasmGCType* group[] = { tentative };
    WasmGCTypeBuilder::deduplicateAndRegister(group, sigs2, rootSet2);

    EXPECT_EQ(group[0], canonical);

    registry.deregisterRootSet(&rootSet2);
    registry.deregisterRootSet(&rootSet1);
}

TEST(WasmGCTypeBuilder, DeduplicationArrayTypes)
{
    // Verify that array type deduplication works: a tentative array[i32]
    // is replaced by a canonical array[i32] already in the registry.
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* canonical = createI32Array();
    ASSERT_TRUE(canonical);

    Vector<WasmGCType*> sigs1;
    WasmGCTypeRootSet rootSet1;
    registerSingletonGroup(canonical, sigs1, rootSet1);

    auto* tentative = createI32Array();
    ASSERT_TRUE(tentative);
    ASSERT_NE(tentative, canonical);

    Vector<WasmGCType*> sigs2;
    WasmGCTypeRootSet rootSet2;
    WasmGCType* group[] = { tentative };
    WasmGCTypeBuilder::deduplicateAndRegister(group, sigs2, rootSet2);

    EXPECT_EQ(group[0], canonical);

    registry.deregisterRootSet(&rootSet2);
    registry.deregisterRootSet(&rootSet1);
}

TEST(WasmGCTypeBuilder, DeduplicationDoesNotMatchDifferentKinds)
{
    // A struct{i32} should not match an array{i32} even though they are both
    // single-element i32 types.
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* structType = createI32Struct();
    ASSERT_TRUE(structType);

    Vector<WasmGCType*> sigs1;
    WasmGCTypeRootSet rootSet1;
    registerSingletonGroup(structType, sigs1, rootSet1);

    auto* arrayType = createI32Array();
    ASSERT_TRUE(arrayType);

    Vector<WasmGCType*> sigs2;
    WasmGCTypeRootSet rootSet2;
    WasmGCType* group[] = { arrayType };
    WasmGCTypeBuilder::deduplicateAndRegister(group, sigs2, rootSet2);

    // Should NOT be deduplicated — different type kinds.
    EXPECT_EQ(group[0], arrayType);
    EXPECT_NE(group[0], structType);

    registry.deregisterRootSet(&rootSet2);
    registry.deregisterRootSet(&rootSet1);
}

// =========================================================================
// D. Root set lifecycle
// =========================================================================

TEST(WasmGCTypeBuilder, RootSetLifecycle)
{
    // Verify that a type is findable in the registry after registration and
    // is collected (not findable) after its root set is deregistered.
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* func = createFuncType_I32_to_I32();
    ASSERT_TRUE(func);

    Vector<WasmGCType*> sigs;
    WasmGCTypeRootSet rootSet;
    registerSingletonGroup(func, sigs, rootSet);

    // Type should be findable.
    auto* probe1 = createFuncType_I32_to_I32();
    {
        Locker locker { registry.lock() };
        EXPECT_EQ(registry.findType(probe1), func);
    }
    probe1->destroy();

    // After deregistration, type should be collected.
    registry.deregisterRootSet(&rootSet);

    auto* probe2 = createFuncType_I32_to_I32();
    {
        Locker locker { registry.lock() };
        EXPECT_EQ(registry.findType(probe2), nullptr);
    }
    probe2->destroy();
}

TEST(WasmGCTypeBuilder, MultipleGroupsInOneRootSet)
{
    // Simulate a module registering multiple groups sequentially into the
    // same root set, as parseType does.
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* func = createFuncType_I32_to_I32();
    auto* structType = createI32Struct();
    auto* arrayType = createI32Array();
    ASSERT_TRUE(func && structType && arrayType);

    Vector<WasmGCType*> sigs;
    WasmGCTypeRootSet rootSet;

    // Group 1: function type
    registerSingletonGroup(func, sigs, rootSet);
    EXPECT_EQ(sigs.size(), 1u);

    // Group 2: struct type
    registerSingletonGroup(structType, sigs, rootSet);
    EXPECT_EQ(sigs.size(), 2u);

    // Group 3: array type
    registerSingletonGroup(arrayType, sigs, rootSet);
    EXPECT_EQ(sigs.size(), 3u);

    // All three should be findable.
    {
        Locker locker { registry.lock() };
        auto* probeFunc = createFuncType_I32_to_I32();
        EXPECT_EQ(registry.findType(probeFunc), func);
        probeFunc->destroy();

        auto* probeStruct = createI32Struct();
        EXPECT_EQ(registry.findType(probeStruct), structType);
        probeStruct->destroy();

        auto* probeArray = createI32Array();
        EXPECT_EQ(registry.findType(probeArray), arrayType);
        probeArray->destroy();
    }

    // Deregistering the root set collects all three.
    registry.deregisterRootSet(&rootSet);

    {
        Locker locker { registry.lock() };
        auto* probeFunc = createFuncType_I32_to_I32();
        EXPECT_EQ(registry.findType(probeFunc), nullptr);
        probeFunc->destroy();
    }
}

// =========================================================================
// E. Simulated concurrent scenarios
//
// These tests simulate race scenarios that would occur if multiple parsers
// or a parser and a module unload were happening concurrently. We interleave
// the relevant operations within a single thread to verify the invariants
// hold at each step.
// =========================================================================

TEST(WasmGCTypeBuilder, SimulatedRaceTwoParsersRegisterSameType)
{
    // Scenario: Two parsers (A and B) both try to register the same type.
    // With proper atomicity of deduplicateAndRegister, the second parser
    // should get the canonical pointer from the first.

    auto& registry = WasmGCTypeRegistry::singleton();

    // Parser A registers its type.
    auto* typeA = createFuncType_F64_to_F64();
    ASSERT_TRUE(typeA);
    Vector<WasmGCType*> sigsA;
    WasmGCTypeRootSet rootSetA;
    registerSingletonGroup(typeA, sigsA, rootSetA);

    // typeA is now canonical in the registry.
    EXPECT_EQ(sigsA[0], typeA);

    // Parser B creates a structurally identical type and registers it.
    auto* typeB = createFuncType_F64_to_F64();
    ASSERT_TRUE(typeB);
    ASSERT_NE(typeB, typeA);

    Vector<WasmGCType*> sigsB;
    WasmGCTypeRootSet rootSetB;
    WasmGCType* groupB[] = { typeB };
    WasmGCTypeBuilder::deduplicateAndRegister(groupB, sigsB, rootSetB);

    // B should have been deduplicated to A's canonical pointer.
    EXPECT_EQ(groupB[0], typeA);
    EXPECT_EQ(sigsB[0], typeA);

    // Both root sets reference the same canonical type.
    // Deregister A — type should survive because B still references it.
    registry.deregisterRootSet(&rootSetA);

    {
        Locker locker { registry.lock() };
        auto* probe = createFuncType_F64_to_F64();
        EXPECT_EQ(registry.findType(probe), typeA);
        probe->destroy();
    }

    // Deregister B — now the type should be collected.
    registry.deregisterRootSet(&rootSetB);

    {
        Locker locker { registry.lock() };
        auto* probe = createFuncType_F64_to_F64();
        EXPECT_EQ(registry.findType(probe), nullptr);
        probe->destroy();
    }
}

TEST(WasmGCTypeBuilder, SimulatedRaceModuleUnloadDuringParsing)
{
    // Scenario: Parser registers group 1 (type X) into rootSetParser.
    // Before parser finishes group 2, another module that also referenced
    // type X unloads and deregisters its root set.
    // Type X should survive because rootSetParser still references it.

    auto& registry = WasmGCTypeRegistry::singleton();

    // Existing module has type X registered.
    auto* typeX = createI32Struct();
    ASSERT_TRUE(typeX);
    Vector<WasmGCType*> existingSigs;
    WasmGCTypeRootSet existingRootSet;
    registerSingletonGroup(typeX, existingSigs, existingRootSet);

    // Parser starts, registers group 1 which deduplicates to typeX.
    auto* tentativeX = createI32Struct();
    ASSERT_TRUE(tentativeX);
    Vector<WasmGCType*> parserSigs;
    WasmGCTypeRootSet parserRootSet;
    WasmGCType* group1[] = { tentativeX };
    WasmGCTypeBuilder::deduplicateAndRegister(group1, parserSigs, parserRootSet);
    EXPECT_EQ(group1[0], typeX); // deduplicated

    // --- Simulated race point ---
    // Between group 1 and group 2 processing, the existing module unloads.
    registry.deregisterRootSet(&existingRootSet);

    // typeX should still be alive because parserRootSet references it.
    {
        Locker locker { registry.lock() };
        auto* probe = createI32Struct();
        EXPECT_EQ(registry.findType(probe), typeX);
        probe->destroy();
    }

    // Parser continues with group 2 which references typeX.
    // Create a struct whose field points to typeX (simulating cross-group ref).
    FieldType fields[] = {
        { StorageType(Type { TypeKind::Ref, typeX->index() }), Mutability::Mutable },
    };
    auto* structWithRef = WasmGCStructType::tryCreate(fields);
    ASSERT_TRUE(structWithRef);

    WasmGCType* group2[] = { structWithRef };
    WasmGCTypeBuilder::patchPlaceholders(group2); // no placeholders, no-op
    WasmGCTypeBuilder::deduplicateAndRegister(group2, parserSigs, parserRootSet);

    EXPECT_EQ(parserSigs.size(), 2u);
    EXPECT_EQ(parserSigs[0], typeX);
    EXPECT_EQ(parserSigs[1], structWithRef);

    // The struct's ref still points to typeX.
    EXPECT_EQ(structWithRef->field(0).type.as<Type>().index, typeX->index());

    registry.deregisterRootSet(&parserRootSet);
}

TEST(WasmGCTypeBuilder, SimulatedRaceDeduplicateThenUnloadOriginal)
{
    // Scenario: Parser A registers type X. Parser B deduplicates against it
    // (gets canonical pointer to X). Then parser A's module unloads.
    // Parser B's type X reference must remain valid.

    auto& registry = WasmGCTypeRegistry::singleton();

    auto* typeX = createI32F64Struct();
    ASSERT_TRUE(typeX);
    Vector<WasmGCType*> sigsA;
    WasmGCTypeRootSet rootSetA;
    registerSingletonGroup(typeX, sigsA, rootSetA);

    // Parser B deduplicates, getting canonical pointer to typeX.
    auto* tentativeX = createI32F64Struct();
    ASSERT_TRUE(tentativeX);
    Vector<WasmGCType*> sigsB;
    WasmGCTypeRootSet rootSetB;
    WasmGCType* groupB[] = { tentativeX };
    WasmGCTypeBuilder::deduplicateAndRegister(groupB, sigsB, rootSetB);
    EXPECT_EQ(groupB[0], typeX);

    // Module A unloads.
    registry.deregisterRootSet(&rootSetA);

    // typeX must survive — rootSetB still holds it.
    {
        Locker locker { registry.lock() };
        auto* probe = createI32F64Struct();
        EXPECT_EQ(registry.findType(probe), typeX);
        probe->destroy();
    }

    // Module B unloads.
    registry.deregisterRootSet(&rootSetB);

    {
        Locker locker { registry.lock() };
        auto* probe = createI32F64Struct();
        EXPECT_EQ(registry.findType(probe), nullptr);
        probe->destroy();
    }
}

TEST(WasmGCTypeBuilder, SimulatedRaceGroupWithSharedAndUniqueTypes)
{
    // Scenario: Parser A has registered type F = (i32)->i32.
    // Parser B registers a group of [F_copy, struct{ref F_copy}].
    // F_copy deduplicates to A's canonical F. The struct survives and its
    // ref should be patched to canonical F.
    // Then module A unloads. Both F and the struct should survive via B.

    auto& registry = WasmGCTypeRegistry::singleton();

    auto* canonicalF = createFuncType_I32_to_I32();
    ASSERT_TRUE(canonicalF);
    Vector<WasmGCType*> sigsA;
    WasmGCTypeRootSet rootSetA;
    registerSingletonGroup(canonicalF, sigsA, rootSetA);

    // Parser B: tentative F and struct{ref tentativeF}.
    auto* tentativeF = createFuncType_I32_to_I32();
    ASSERT_TRUE(tentativeF);
    FieldType fields[] = {
        { StorageType(Type { TypeKind::Ref, tentativeF->index() }), Mutability::Mutable },
    };
    auto* structType = WasmGCStructType::tryCreate(fields);
    ASSERT_TRUE(structType);

    Vector<WasmGCType*> sigsB;
    WasmGCTypeRootSet rootSetB;
    WasmGCType* groupB[] = { tentativeF, structType };
    WasmGCTypeBuilder::deduplicateAndRegister(groupB, sigsB, rootSetB);

    EXPECT_EQ(groupB[0], canonicalF);
    EXPECT_EQ(groupB[1], structType);
    EXPECT_EQ(structType->field(0).type.as<Type>().index, canonicalF->index());

    // Module A unloads.
    registry.deregisterRootSet(&rootSetA);

    // canonicalF should survive — B's root set references it (via the struct's
    // ref field, and also directly since deduplicateAndRegister appended it).
    {
        Locker locker { registry.lock() };
        auto* probeF = createFuncType_I32_to_I32();
        EXPECT_EQ(registry.findType(probeF), canonicalF);
        probeF->destroy();
    }

    registry.deregisterRootSet(&rootSetB);
}

TEST(WasmGCTypeBuilder, SimulatedRaceThreeParsersSequentialDedup)
{
    // Scenario: Three parsers register the same type sequentially.
    // Each subsequent parser should deduplicate to the first's canonical.
    // As modules unload one by one, the type survives until the last one goes.

    auto& registry = WasmGCTypeRegistry::singleton();

    auto* type1 = createI32Array();
    ASSERT_TRUE(type1);
    Vector<WasmGCType*> sigs1;
    WasmGCTypeRootSet rootSet1;
    registerSingletonGroup(type1, sigs1, rootSet1);

    auto* type2 = createI32Array();
    ASSERT_TRUE(type2);
    Vector<WasmGCType*> sigs2;
    WasmGCTypeRootSet rootSet2;
    WasmGCType* group2[] = { type2 };
    WasmGCTypeBuilder::deduplicateAndRegister(group2, sigs2, rootSet2);
    EXPECT_EQ(group2[0], type1);

    auto* type3 = createI32Array();
    ASSERT_TRUE(type3);
    Vector<WasmGCType*> sigs3;
    WasmGCTypeRootSet rootSet3;
    WasmGCType* group3[] = { type3 };
    WasmGCTypeBuilder::deduplicateAndRegister(group3, sigs3, rootSet3);
    EXPECT_EQ(group3[0], type1);

    // Unload first module.
    registry.deregisterRootSet(&rootSet1);
    {
        Locker locker { registry.lock() };
        auto* probe = createI32Array();
        EXPECT_EQ(registry.findType(probe), type1); // still alive via rootSet2, rootSet3
        probe->destroy();
    }

    // Unload second module.
    registry.deregisterRootSet(&rootSet2);
    {
        Locker locker { registry.lock() };
        auto* probe = createI32Array();
        EXPECT_EQ(registry.findType(probe), type1); // still alive via rootSet3
        probe->destroy();
    }

    // Unload third module.
    registry.deregisterRootSet(&rootSet3);
    {
        Locker locker { registry.lock() };
        auto* probe = createI32Array();
        EXPECT_EQ(registry.findType(probe), nullptr); // now collected
        probe->destroy();
    }
}

TEST(WasmGCTypeBuilder, SimulatedRaceUnloadBetweenGroupsDoesNotAffectUniqueTypes)
{
    // Scenario: Parser registers group 1 with type A (unique). Between
    // groups, another module unloads its root set which had an unrelated type B.
    // Type A must not be collected.

    auto& registry = WasmGCTypeRegistry::singleton();

    // Other module has type B.
    auto* typeB = createFuncType_F64_to_F64();
    ASSERT_TRUE(typeB);
    Vector<WasmGCType*> otherSigs;
    WasmGCTypeRootSet otherRootSet;
    registerSingletonGroup(typeB, otherSigs, otherRootSet);

    // Parser registers type A in group 1.
    auto* typeA = createI32Struct();
    ASSERT_TRUE(typeA);
    Vector<WasmGCType*> parserSigs;
    WasmGCTypeRootSet parserRootSet;
    registerSingletonGroup(typeA, parserSigs, parserRootSet);

    // --- Simulated race: other module unloads ---
    registry.deregisterRootSet(&otherRootSet);

    // typeA is untouched.
    {
        Locker locker { registry.lock() };
        auto* probe = createI32Struct();
        EXPECT_EQ(registry.findType(probe), typeA);
        probe->destroy();

        // typeB should be gone.
        auto* probeB = createFuncType_F64_to_F64();
        EXPECT_EQ(registry.findType(probeB), nullptr);
        probeB->destroy();
    }

    registry.deregisterRootSet(&parserRootSet);
}

TEST(WasmGCTypeBuilder, SimulatedRaceRecursiveGroupDedup)
{
    // Scenario: Module A has a recursive pair A1{ref A2}, A2{ref A1}.
    // Module B parses a structurally identical pair B1{ref B2}, B2{ref B1}.
    // Both types in B's group should be deduplicated to A's canonicals.

    auto& registry = WasmGCTypeRegistry::singleton();

    // Module A: register recursive pair.
    TypeIndex phA0 = WasmGCTypeBuilder::placeholderForGroupIndex(0);
    TypeIndex phA1 = WasmGCTypeBuilder::placeholderForGroupIndex(1);
    auto* a1 = createRefStruct(phA1);
    auto* a2 = createRefStruct(phA0);
    ASSERT_TRUE(a1 && a2);

    WasmGCType* groupA[] = { a1, a2 };
    WasmGCTypeBuilder::patchPlaceholders(groupA);

    Vector<WasmGCType*> sigsA;
    WasmGCTypeRootSet rootSetA;
    WasmGCTypeBuilder::deduplicateAndRegister(groupA, sigsA, rootSetA);

    EXPECT_EQ(sigsA[0], a1);
    EXPECT_EQ(sigsA[1], a2);

    // Module B: structurally identical recursive pair.
    TypeIndex phB0 = WasmGCTypeBuilder::placeholderForGroupIndex(0);
    TypeIndex phB1 = WasmGCTypeBuilder::placeholderForGroupIndex(1);
    auto* b1 = createRefStruct(phB1);
    auto* b2 = createRefStruct(phB0);
    ASSERT_TRUE(b1 && b2);

    WasmGCType* groupB[] = { b1, b2 };
    WasmGCTypeBuilder::patchPlaceholders(groupB);

    Vector<WasmGCType*> sigsB;
    WasmGCTypeRootSet rootSetB;
    WasmGCTypeBuilder::deduplicateAndRegister(groupB, sigsB, rootSetB);

    // Both should be deduplicated to A's canonicals. Since a1 and a2 are
    // structurally equal to each other (both are struct{ref <partner>}),
    // bisimulation can match either b1 or b2 to either a1 or a2. We can
    // only assert that both were deduplicated to some canonical from A.
    EXPECT_TRUE(groupB[0] == a1 || groupB[0] == a2);
    EXPECT_TRUE(groupB[1] == a1 || groupB[1] == a2);

    registry.deregisterRootSet(&rootSetB);
    registry.deregisterRootSet(&rootSetA);
}

TEST(WasmGCTypeBuilder, SimulatedRacePartialGroupDedup)
{
    // Scenario: Module A has type F = (i32)->i32.
    // Module B parses a recursion group [F_copy, S{ref F_copy}].
    // F_copy deduplicates; S is novel. S's ref should be patched.
    // After A unloads, both F and S survive via B's root set.

    auto& registry = WasmGCTypeRegistry::singleton();

    auto* canonicalF = createFuncType_I32_to_I32();
    ASSERT_TRUE(canonicalF);
    Vector<WasmGCType*> sigsA;
    WasmGCTypeRootSet rootSetA;
    registerSingletonGroup(canonicalF, sigsA, rootSetA);

    // Module B: group [tentativeF, S{ref tentativeF}]
    auto* tentativeF = createFuncType_I32_to_I32();
    ASSERT_TRUE(tentativeF);

    // S's ref uses tentativeF's pointer directly (backward reference).
    FieldType fields[] = {
        { StorageType(Type { TypeKind::Ref, tentativeF->index() }), Mutability::Mutable },
    };
    auto* structS = WasmGCStructType::tryCreate(fields);
    ASSERT_TRUE(structS);

    Vector<WasmGCType*> sigsB;
    WasmGCTypeRootSet rootSetB;
    WasmGCType* groupB[] = { tentativeF, structS };
    WasmGCTypeBuilder::deduplicateAndRegister(groupB, sigsB, rootSetB);

    EXPECT_EQ(groupB[0], canonicalF); // deduplicated
    EXPECT_EQ(groupB[1], structS);    // novel, survives
    EXPECT_EQ(structS->field(0).type.as<Type>().index, canonicalF->index()); // patched

    // Module A unloads. F and S survive via B.
    registry.deregisterRootSet(&rootSetA);

    {
        Locker locker { registry.lock() };
        auto* probeF = createFuncType_I32_to_I32();
        EXPECT_EQ(registry.findType(probeF), canonicalF);
        probeF->destroy();
    }

    registry.deregisterRootSet(&rootSetB);
}

// =========================================================================
// F. RTT registration through the builder pipeline
// =========================================================================

TEST(WasmGCTypeBuilder, RTTRegisteredAfterDedup)
{
    // After deduplicateAndRegister, calling registerCanonicalRTTForType
    // should produce valid RTTs.
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* func = createFuncType_I32_to_I32();
    auto* structType = createI32Struct();
    auto* arrayType = createI32Array();
    ASSERT_TRUE(func && structType && arrayType);

    Vector<WasmGCType*> sigs;
    WasmGCTypeRootSet rootSet;
    registerSingletonGroup(func, sigs, rootSet);
    registerSingletonGroup(structType, sigs, rootSet);
    registerSingletonGroup(arrayType, sigs, rootSet);

    // Register RTTs (as processGCTypeGroup does).
    for (auto* type : sigs)
        registry.registerCanonicalRTTForType(type);

    auto funcRTT = registry.getCanonicalRTT(func);
    EXPECT_EQ(funcRTT->kind(), RTTKind::Function);

    auto structRTT = registry.getCanonicalRTT(structType);
    EXPECT_EQ(structRTT->kind(), RTTKind::Struct);

    auto arrayRTT = registry.getCanonicalRTT(arrayType);
    EXPECT_EQ(arrayRTT->kind(), RTTKind::Array);

    registry.deregisterRootSet(&rootSet);
}

TEST(WasmGCTypeBuilder, RTTIdempotentRegistration)
{
    // Calling registerCanonicalRTTForType twice should produce the same RTT.
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* func = createFuncType_I32_to_I32();
    ASSERT_TRUE(func);

    Vector<WasmGCType*> sigs;
    WasmGCTypeRootSet rootSet;
    registerSingletonGroup(func, sigs, rootSet);

    registry.registerCanonicalRTTForType(func);
    auto rtt1 = registry.getCanonicalRTT(func);

    registry.registerCanonicalRTTForType(func);
    auto rtt2 = registry.getCanonicalRTT(func);

    EXPECT_EQ(rtt1.ptr(), rtt2.ptr());

    registry.deregisterRootSet(&rootSet);
}

TEST(WasmGCTypeBuilder, RTTWithSupertypeAfterDedup)
{
    // Register a parent type, then a child with supertype = parent.
    // The child's RTT should have the parent's RTT in its display.
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* parent = createFuncType_I32_to_I32();
    ASSERT_TRUE(parent);
    parent->setIsFinal(false);

    Vector<WasmGCType*> sigs;
    WasmGCTypeRootSet rootSet;
    registerSingletonGroup(parent, sigs, rootSet);
    registry.registerCanonicalRTTForType(parent);

    auto* child = createFuncType_I32_to_I32();
    ASSERT_TRUE(child);
    child->setSupertype(parent);
    child->setIsFinal(true);

    registerSingletonGroup(child, sigs, rootSet);
    registry.registerCanonicalRTTForType(child);

    auto parentRTT = registry.getCanonicalRTT(parent);
    auto childRTT = registry.getCanonicalRTT(child);

    // Child's RTT display should include the parent's RTT.
    EXPECT_EQ(parentRTT->displaySizeExcludingThis(), 0u);
    EXPECT_EQ(childRTT->displaySizeExcludingThis(), 1u);
    EXPECT_EQ(childRTT->displayEntry(0), parentRTT.ptr());

    registry.deregisterRootSet(&rootSet);
}

// =========================================================================
// G. Placeholder utilities
// =========================================================================

TEST(WasmGCTypeBuilder, PlaceholderRoundTrip)
{
    // Verify that placeholderForGroupIndex and placeholderToGroupIndex are
    // inverses, and that all generated placeholders pass isPlaceholder.
    for (uint32_t i = 0; i < 100; ++i) {
        TypeIndex placeholder = WasmGCTypeBuilder::placeholderForGroupIndex(i);
        EXPECT_TRUE(WasmGCTypeBuilder::isPlaceholder(placeholder));
        EXPECT_EQ(WasmGCTypeBuilder::placeholderToGroupIndex(placeholder), i);
    }
}

TEST(WasmGCTypeBuilder, PlaceholderZeroIsNotPlaceholder)
{
    // TypeIndex 0 is not a placeholder (placeholders start at 1).
    EXPECT_FALSE(WasmGCTypeBuilder::isPlaceholder(0));
}

TEST(WasmGCTypeBuilder, BuiltinTypeIndicesAreNotPlaceholders)
{
    // Builtin TypeIndex values (negative when cast from TypeKind) should not
    // be recognized as placeholders.
    EXPECT_FALSE(WasmGCTypeBuilder::isPlaceholder(static_cast<TypeIndex>(TypeKind::I32)));
    EXPECT_FALSE(WasmGCTypeBuilder::isPlaceholder(static_cast<TypeIndex>(TypeKind::F64)));
    EXPECT_FALSE(WasmGCTypeBuilder::isPlaceholder(static_cast<TypeIndex>(TypeKind::Funcref)));
    EXPECT_FALSE(WasmGCTypeBuilder::isPlaceholder(static_cast<TypeIndex>(TypeKind::Externref)));
}

// =========================================================================
// H. Edge cases
// =========================================================================

TEST(WasmGCTypeBuilder, EmptyGroupPatchPlaceholders)
{
    // Patching an empty group should not crash.
    std::span<WasmGCType* const> empty;
    WasmGCTypeBuilder::patchPlaceholders(empty);
}

TEST(WasmGCTypeBuilder, DeduplicationEmptyGroup)
{
    // Deduplicating an empty group should not crash.
    auto& registry = WasmGCTypeRegistry::singleton();

    Vector<WasmGCType*> sigs;
    WasmGCTypeRootSet rootSet;
    std::span<WasmGCType*> empty;
    WasmGCTypeBuilder::deduplicateAndRegister(empty, sigs, rootSet);

    EXPECT_EQ(sigs.size(), 0u);

    registry.deregisterRootSet(&rootSet);
}

TEST(WasmGCTypeBuilder, DeduplicationGroupWithAllMatches)
{
    // Group where every type matches an existing canonical — all replaced.
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* canonical1 = createFuncType_I32_to_I32();
    auto* canonical2 = createI32Struct();
    ASSERT_TRUE(canonical1 && canonical2);

    Vector<WasmGCType*> existingSigs;
    WasmGCTypeRootSet existingRootSet;
    registerSingletonGroup(canonical1, existingSigs, existingRootSet);
    registerSingletonGroup(canonical2, existingSigs, existingRootSet);

    auto* dup1 = createFuncType_I32_to_I32();
    auto* dup2 = createI32Struct();
    ASSERT_TRUE(dup1 && dup2);

    Vector<WasmGCType*> sigs;
    WasmGCTypeRootSet rootSet;
    WasmGCType* group[] = { dup1, dup2 };
    WasmGCTypeBuilder::deduplicateAndRegister(group, sigs, rootSet);

    EXPECT_EQ(group[0], canonical1);
    EXPECT_EQ(group[1], canonical2);

    registry.deregisterRootSet(&rootSet);
    registry.deregisterRootSet(&existingRootSet);
}

TEST(WasmGCTypeBuilder, DeduplicationGroupWithNoMatches)
{
    // Group where no type matches — all survive.
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* novel1 = createFuncType_F64_to_F64();
    auto* novel2 = createI32F64Struct();
    ASSERT_TRUE(novel1 && novel2);

    Vector<WasmGCType*> sigs;
    WasmGCTypeRootSet rootSet;
    WasmGCType* group[] = { novel1, novel2 };
    WasmGCTypeBuilder::deduplicateAndRegister(group, sigs, rootSet);

    EXPECT_EQ(group[0], novel1);
    EXPECT_EQ(group[1], novel2);
    EXPECT_EQ(sigs.size(), 2u);

    registry.deregisterRootSet(&rootSet);
}

TEST(WasmGCTypeBuilder, GcTypeSignaturesAccumulateAcrossGroups)
{
    // Verify that gcTypeSignatures accumulates correctly as groups are
    // processed, matching the pattern in parseType.
    auto& registry = WasmGCTypeRegistry::singleton();

    Vector<WasmGCType*> sigs;
    WasmGCTypeRootSet rootSet;

    auto* t1 = createFuncType_I32_to_I32();
    auto* t2 = createI32Struct();
    auto* t3 = createI32Array();
    ASSERT_TRUE(t1 && t2 && t3);

    registerSingletonGroup(t1, sigs, rootSet);
    EXPECT_EQ(sigs.size(), 1u);
    EXPECT_EQ(sigs[0], t1);

    registerSingletonGroup(t2, sigs, rootSet);
    EXPECT_EQ(sigs.size(), 2u);
    EXPECT_EQ(sigs[1], t2);

    registerSingletonGroup(t3, sigs, rootSet);
    EXPECT_EQ(sigs.size(), 3u);
    EXPECT_EQ(sigs[2], t3);

    // Cross-group reference: a struct whose field references t1 (from group 1).
    FieldType fields[] = {
        { StorageType(Type { TypeKind::Ref, sigs[0]->index() }), Mutability::Mutable },
    };
    auto* structWithRef = WasmGCStructType::tryCreate(fields);
    ASSERT_TRUE(structWithRef);
    registerSingletonGroup(structWithRef, sigs, rootSet);
    EXPECT_EQ(sigs.size(), 4u);
    EXPECT_EQ(structWithRef->field(0).type.as<Type>().index, t1->index());

    registry.deregisterRootSet(&rootSet);
}

// =========================================================================
// I. Combinatorial interaction tests
//
// These test specific dependency chains between operations: what happens
// when operation B depends on the result of operation A, and an intervening
// event C (like a module unload) occurs.
// =========================================================================

TEST(WasmGCTypeBuilder, CrossGroupRefToDedupedType)
{
    // Group 1 has type F which deduplicates to an existing canonical.
    // Group 2 has a struct whose field references F via gcTypeSignatures.
    // After dedup, group 2's struct should reference the canonical F,
    // not the destroyed tentative F.

    auto& registry = WasmGCTypeRegistry::singleton();

    // Pre-existing canonical F.
    auto* canonicalF = createFuncType_I32_to_I32();
    ASSERT_TRUE(canonicalF);
    Vector<WasmGCType*> existingSigs;
    WasmGCTypeRootSet existingRootSet;
    registerSingletonGroup(canonicalF, existingSigs, existingRootSet);

    // Parser processes group 1: tentative F deduplicates.
    auto* tentativeF = createFuncType_I32_to_I32();
    ASSERT_TRUE(tentativeF);
    Vector<WasmGCType*> parserSigs;
    WasmGCTypeRootSet parserRootSet;
    WasmGCType* group1[] = { tentativeF };
    WasmGCTypeBuilder::deduplicateAndRegister(group1, parserSigs, parserRootSet);
    EXPECT_EQ(group1[0], canonicalF);
    // parserSigs[0] is now canonicalF.

    // Parser processes group 2: struct references parserSigs[0] (= canonicalF).
    FieldType fields[] = {
        { StorageType(Type { TypeKind::Ref, parserSigs[0]->index() }), Mutability::Mutable },
    };
    auto* structType = WasmGCStructType::tryCreate(fields);
    ASSERT_TRUE(structType);
    WasmGCType* group2[] = { structType };
    WasmGCTypeBuilder::patchPlaceholders(group2);
    WasmGCTypeBuilder::deduplicateAndRegister(group2, parserSigs, parserRootSet);

    // struct's field should reference canonicalF.
    EXPECT_EQ(structType->field(0).type.as<Type>().index, canonicalF->index());

    registry.deregisterRootSet(&parserRootSet);
    registry.deregisterRootSet(&existingRootSet);
}

TEST(WasmGCTypeBuilder, CrossGroupRefSurvivesProviderModuleUnload)
{
    // Group 1: type F (deduplicates to existing canonical).
    // Group 2: struct S{ref F} (cross-group reference).
    // Between groups 1 and 2, the module that owned the canonical F unloads.
    // F should survive via the parser's root set.
    // S should still reference F correctly.

    auto& registry = WasmGCTypeRegistry::singleton();

    auto* canonicalF = createFuncType_I32_to_I32();
    ASSERT_TRUE(canonicalF);
    Vector<WasmGCType*> existingSigs;
    WasmGCTypeRootSet existingRootSet;
    registerSingletonGroup(canonicalF, existingSigs, existingRootSet);

    // Parser: group 1 deduplicates to canonicalF.
    auto* tentativeF = createFuncType_I32_to_I32();
    ASSERT_TRUE(tentativeF);
    Vector<WasmGCType*> parserSigs;
    WasmGCTypeRootSet parserRootSet;
    WasmGCType* group1[] = { tentativeF };
    WasmGCTypeBuilder::deduplicateAndRegister(group1, parserSigs, parserRootSet);
    EXPECT_EQ(parserSigs[0], canonicalF);

    // --- Provider module unloads between groups ---
    registry.deregisterRootSet(&existingRootSet);

    // F must still be alive.
    {
        Locker locker { registry.lock() };
        auto* probe = createFuncType_I32_to_I32();
        EXPECT_EQ(registry.findType(probe), canonicalF);
        probe->destroy();
    }

    // Parser: group 2 references parserSigs[0] = canonicalF.
    FieldType fields[] = {
        { StorageType(Type { TypeKind::Ref, parserSigs[0]->index() }), Mutability::Mutable },
    };
    auto* structS = WasmGCStructType::tryCreate(fields);
    ASSERT_TRUE(structS);
    WasmGCType* group2[] = { structS };
    WasmGCTypeBuilder::patchPlaceholders(group2);
    WasmGCTypeBuilder::deduplicateAndRegister(group2, parserSigs, parserRootSet);

    EXPECT_EQ(structS->field(0).type.as<Type>().index, canonicalF->index());

    registry.deregisterRootSet(&parserRootSet);
}

TEST(WasmGCTypeBuilder, SupertypeFromEarlierGroupThenUnload)
{
    // Group 1: parent type P (registered as singleton).
    // Group 2: child type C with supertype = P.
    // Between groups, a module that also had P unloads.
    // P must survive, and C's supertype must still point to P.

    auto& registry = WasmGCTypeRegistry::singleton();

    // Existing module has P.
    auto* parentP = createFuncType_I32_to_I32();
    ASSERT_TRUE(parentP);
    parentP->setIsFinal(false);
    Vector<WasmGCType*> existingSigs;
    WasmGCTypeRootSet existingRootSet;
    registerSingletonGroup(parentP, existingSigs, existingRootSet);
    registry.registerCanonicalRTTForType(parentP);

    // Parser: group 1 deduplicates to parentP.
    auto* tentativeP = createFuncType_I32_to_I32();
    ASSERT_TRUE(tentativeP);
    tentativeP->setIsFinal(false);
    Vector<WasmGCType*> parserSigs;
    WasmGCTypeRootSet parserRootSet;
    WasmGCType* group1[] = { tentativeP };
    WasmGCTypeBuilder::deduplicateAndRegister(group1, parserSigs, parserRootSet);
    EXPECT_EQ(parserSigs[0], parentP);

    // --- Existing module unloads ---
    registry.deregisterRootSet(&existingRootSet);

    // Parser: group 2, child with supertype = parserSigs[0] = parentP.
    auto* childC = createFuncType_I32_to_I32();
    ASSERT_TRUE(childC);
    childC->setSupertype(parserSigs[0]);
    childC->setIsFinal(true);
    WasmGCType* group2[] = { childC };
    WasmGCTypeBuilder::patchPlaceholders(group2);
    WasmGCTypeBuilder::deduplicateAndRegister(group2, parserSigs, parserRootSet);
    registry.registerCanonicalRTTForType(childC);

    EXPECT_EQ(childC->supertype(), parentP);

    auto childRTT = registry.getCanonicalRTT(childC);
    auto parentRTT = registry.getCanonicalRTT(parentP);
    EXPECT_EQ(childRTT->displaySizeExcludingThis(), 1u);
    EXPECT_EQ(childRTT->displayEntry(0), parentRTT.ptr());

    registry.deregisterRootSet(&parserRootSet);
}

TEST(WasmGCTypeBuilder, SupertypeChainPartialDedup)
{
    // Grandparent G is pre-existing.
    // Parser's group contains: [G_copy, parent P (super=G_copy), child C (super=P)].
    // G_copy deduplicates to canonical G.
    // P and C survive; P's supertype should be patched to canonical G.
    // C's supertype is P which survived, so no patching needed.

    auto& registry = WasmGCTypeRegistry::singleton();

    auto* canonicalG = createFuncType_I32_to_I32();
    ASSERT_TRUE(canonicalG);
    canonicalG->setIsFinal(false);
    Vector<WasmGCType*> existingSigs;
    WasmGCTypeRootSet existingRootSet;
    registerSingletonGroup(canonicalG, existingSigs, existingRootSet);
    registry.registerCanonicalRTTForType(canonicalG);

    // Parser's group: [tentativeG, P, C].
    auto* tentativeG = createFuncType_I32_to_I32();
    ASSERT_TRUE(tentativeG);
    tentativeG->setIsFinal(false);

    auto* parentP = createFuncType_I32_to_I32();
    ASSERT_TRUE(parentP);
    parentP->setIsFinal(false);
    parentP->setSupertype(tentativeG);

    auto* childC = createFuncType_I32_to_I32();
    ASSERT_TRUE(childC);
    childC->setIsFinal(true);
    childC->setSupertype(parentP);

    Vector<WasmGCType*> parserSigs;
    WasmGCTypeRootSet parserRootSet;
    WasmGCType* group[] = { tentativeG, parentP, childC };
    WasmGCTypeBuilder::deduplicateAndRegister(group, parserSigs, parserRootSet);

    // tentativeG replaced with canonicalG.
    EXPECT_EQ(group[0], canonicalG);
    // P survives; its supertype should be patched to canonicalG.
    EXPECT_EQ(group[1], parentP);
    EXPECT_EQ(parentP->supertype(), canonicalG);
    // C survives; its supertype is P (not replaced, no patching needed).
    EXPECT_EQ(group[2], childC);
    EXPECT_EQ(childC->supertype(), parentP);

    registry.deregisterRootSet(&parserRootSet);
    registry.deregisterRootSet(&existingRootSet);
}

TEST(WasmGCTypeBuilder, RecursiveGroupPartialDedupWithRefs)
{
    // Pre-existing canonical F = (i32)->i32.
    // Parser's recursion group: [F_copy, S{ref F_copy, ref S}].
    // F_copy deduplicates. S survives but has two refs to patch:
    //   - ref to F_copy -> ref to canonicalF
    //   - ref to S -> stays as-is (self-reference, S is not in replacements)

    auto& registry = WasmGCTypeRegistry::singleton();

    auto* canonicalF = createFuncType_I32_to_I32();
    ASSERT_TRUE(canonicalF);
    Vector<WasmGCType*> existingSigs;
    WasmGCTypeRootSet existingRootSet;
    registerSingletonGroup(canonicalF, existingSigs, existingRootSet);

    // tentativeF and S{ref tentativeF, ref self}
    auto* tentativeF = createFuncType_I32_to_I32();
    ASSERT_TRUE(tentativeF);

    FieldType fields[] = {
        { StorageType(Type { TypeKind::Ref, tentativeF->index() }), Mutability::Mutable },
        { StorageType(Type { TypeKind::Ref, WasmGCTypeBuilder::placeholderForGroupIndex(1) }), Mutability::Mutable },
    };
    auto* structS = WasmGCStructType::tryCreate(fields);
    ASSERT_TRUE(structS);

    WasmGCType* group[] = { tentativeF, structS };
    WasmGCTypeBuilder::patchPlaceholders(group);

    // After patching, S's second field should point to S itself.
    EXPECT_EQ(structS->field(1).type.as<Type>().index, structS->index());
    // S's first field still points to tentativeF (patching only resolves placeholders).
    EXPECT_EQ(structS->field(0).type.as<Type>().index, tentativeF->index());

    Vector<WasmGCType*> parserSigs;
    WasmGCTypeRootSet parserRootSet;
    WasmGCTypeBuilder::deduplicateAndRegister(group, parserSigs, parserRootSet);

    // tentativeF replaced with canonicalF.
    EXPECT_EQ(group[0], canonicalF);
    // S survives.
    EXPECT_EQ(group[1], structS);
    // S's first field patched: tentativeF -> canonicalF.
    EXPECT_EQ(structS->field(0).type.as<Type>().index, canonicalF->index());
    // S's second field untouched: still self-reference.
    EXPECT_EQ(structS->field(1).type.as<Type>().index, structS->index());

    registry.deregisterRootSet(&parserRootSet);
    registry.deregisterRootSet(&existingRootSet);
}

TEST(WasmGCTypeBuilder, TwoModulesSameRecursiveGroupThenSequentialUnload)
{
    // Module A registers recursive pair [A1{ref A2}, A2{ref A1}].
    // Module B registers identical pair — both dedup to A's canonicals.
    // Module A unloads — pair survives via B.
    // Module B unloads — pair is collected.

    auto& registry = WasmGCTypeRegistry::singleton();

    // Module A
    TypeIndex ph0 = WasmGCTypeBuilder::placeholderForGroupIndex(0);
    TypeIndex ph1 = WasmGCTypeBuilder::placeholderForGroupIndex(1);
    auto* a1 = createRefStruct(ph1);
    auto* a2 = createRefStruct(ph0);
    ASSERT_TRUE(a1 && a2);
    WasmGCType* groupA[] = { a1, a2 };
    WasmGCTypeBuilder::patchPlaceholders(groupA);

    Vector<WasmGCType*> sigsA;
    WasmGCTypeRootSet rootSetA;
    WasmGCTypeBuilder::deduplicateAndRegister(groupA, sigsA, rootSetA);

    // Module B — identical pair.
    auto* b1 = createRefStruct(WasmGCTypeBuilder::placeholderForGroupIndex(1));
    auto* b2 = createRefStruct(WasmGCTypeBuilder::placeholderForGroupIndex(0));
    ASSERT_TRUE(b1 && b2);
    WasmGCType* groupB[] = { b1, b2 };
    WasmGCTypeBuilder::patchPlaceholders(groupB);

    Vector<WasmGCType*> sigsB;
    WasmGCTypeRootSet rootSetB;
    WasmGCTypeBuilder::deduplicateAndRegister(groupB, sigsB, rootSetB);

    // Both should be deduplicated to some canonical from A. As above,
    // a1 and a2 are structurally interchangeable, so we can't predict
    // which specific canonical each slot gets.
    EXPECT_TRUE(groupB[0] == a1 || groupB[0] == a2);
    EXPECT_TRUE(groupB[1] == a1 || groupB[1] == a2);

    // Module A unloads.
    registry.deregisterRootSet(&rootSetA);

    // Pair survives via B.
    {
        Locker locker { registry.lock() };
        EXPECT_NE(registry.findType(a1), nullptr);
    }

    // Module B unloads.
    registry.deregisterRootSet(&rootSetB);

    // Pair collected.
    {
        Locker locker { registry.lock() };
        // Create a fresh probe to test (a1 is destroyed at this point).
        auto* probe1 = createRefStruct(WasmGCTypeBuilder::placeholderForGroupIndex(1));
        auto* probe2 = createRefStruct(WasmGCTypeBuilder::placeholderForGroupIndex(0));
        WasmGCType* probeGroup[] = { probe1, probe2 };
        WasmGCTypeBuilder::patchPlaceholders(probeGroup);
        EXPECT_EQ(registry.findType(probe1), nullptr);
        probe1->destroy();
        probe2->destroy();
    }
}

TEST(WasmGCTypeBuilder, ArrayRefToDedupedStructThenUnload)
{
    // Group 1: struct S{i32} (deduplicates to existing canonical).
    // Group 2: array A[ref S] (cross-group reference to canonicalized S).
    // Existing module that owned canonical S unloads between groups.
    // S should survive via parser's root set, A should reference canonical S.

    auto& registry = WasmGCTypeRegistry::singleton();

    auto* canonicalS = createI32Struct();
    ASSERT_TRUE(canonicalS);
    Vector<WasmGCType*> existingSigs;
    WasmGCTypeRootSet existingRootSet;
    registerSingletonGroup(canonicalS, existingSigs, existingRootSet);

    // Parser: group 1.
    auto* tentativeS = createI32Struct();
    ASSERT_TRUE(tentativeS);
    Vector<WasmGCType*> parserSigs;
    WasmGCTypeRootSet parserRootSet;
    WasmGCType* group1[] = { tentativeS };
    WasmGCTypeBuilder::deduplicateAndRegister(group1, parserSigs, parserRootSet);
    EXPECT_EQ(parserSigs[0], canonicalS);

    // Existing module unloads.
    registry.deregisterRootSet(&existingRootSet);

    // Parser: group 2 — array[ref parserSigs[0]].
    auto* arrayA = createRefArray(parserSigs[0]->index());
    ASSERT_TRUE(arrayA);
    WasmGCType* group2[] = { arrayA };
    WasmGCTypeBuilder::patchPlaceholders(group2);
    WasmGCTypeBuilder::deduplicateAndRegister(group2, parserSigs, parserRootSet);

    EXPECT_EQ(arrayA->elementType().type.as<Type>().index, canonicalS->index());

    // Both S and A survive via parser root set.
    {
        Locker locker { registry.lock() };
        auto* probeS = createI32Struct();
        EXPECT_EQ(registry.findType(probeS), canonicalS);
        probeS->destroy();
    }

    registry.deregisterRootSet(&parserRootSet);
}

TEST(WasmGCTypeBuilder, FunctionArgRefToDedupedType)
{
    // Group 1: struct S{i32} deduplicates to existing canonical.
    // Group 2: function F(ref S) -> i32, where ref S uses the canonical
    // pointer obtained from group 1's deduplication.

    auto& registry = WasmGCTypeRegistry::singleton();

    auto* canonicalS = createI32Struct();
    ASSERT_TRUE(canonicalS);
    Vector<WasmGCType*> existingSigs;
    WasmGCTypeRootSet existingRootSet;
    registerSingletonGroup(canonicalS, existingSigs, existingRootSet);

    // Parser: group 1.
    auto* tentativeS = createI32Struct();
    ASSERT_TRUE(tentativeS);
    Vector<WasmGCType*> parserSigs;
    WasmGCTypeRootSet parserRootSet;
    WasmGCType* group1[] = { tentativeS };
    WasmGCTypeBuilder::deduplicateAndRegister(group1, parserSigs, parserRootSet);
    EXPECT_EQ(parserSigs[0], canonicalS);

    // Parser: group 2 — function (ref canonicalS) -> i32.
    auto* func = WasmGCFunctionType::tryCreate(1, 1);
    ASSERT_TRUE(func);
    func->getReturnType(0) = Types::I32;
    func->getArgumentType(0) = Type { TypeKind::Ref, parserSigs[0]->index() };

    WasmGCType* group2[] = { func };
    WasmGCTypeBuilder::patchPlaceholders(group2);
    WasmGCTypeBuilder::deduplicateAndRegister(group2, parserSigs, parserRootSet);

    EXPECT_EQ(func->argumentType(0).index, canonicalS->index());

    registry.deregisterRootSet(&parserRootSet);
    registry.deregisterRootSet(&existingRootSet);
}

TEST(WasmGCTypeBuilder, TwoParsersRegisterDifferentTypesNoCrosstalk)
{
    // Parser A registers type X. Parser B registers type Y.
    // Neither should interfere with the other. Unloading A should not
    // affect Y, and vice versa.

    auto& registry = WasmGCTypeRegistry::singleton();

    auto* typeX = createFuncType_I32_to_I32();
    auto* typeY = createI32Struct();
    ASSERT_TRUE(typeX && typeY);

    Vector<WasmGCType*> sigsA;
    WasmGCTypeRootSet rootSetA;
    registerSingletonGroup(typeX, sigsA, rootSetA);

    Vector<WasmGCType*> sigsB;
    WasmGCTypeRootSet rootSetB;
    registerSingletonGroup(typeY, sigsB, rootSetB);

    // Unload A.
    registry.deregisterRootSet(&rootSetA);

    // X gone, Y alive.
    {
        Locker locker { registry.lock() };
        auto* probeX = createFuncType_I32_to_I32();
        EXPECT_EQ(registry.findType(probeX), nullptr);
        probeX->destroy();

        auto* probeY = createI32Struct();
        EXPECT_EQ(registry.findType(probeY), typeY);
        probeY->destroy();
    }

    // Unload B.
    registry.deregisterRootSet(&rootSetB);

    // Y gone.
    {
        Locker locker { registry.lock() };
        auto* probeY = createI32Struct();
        EXPECT_EQ(registry.findType(probeY), nullptr);
        probeY->destroy();
    }
}

} // namespace TestWebKitAPI

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(WEBASSEMBLY)
