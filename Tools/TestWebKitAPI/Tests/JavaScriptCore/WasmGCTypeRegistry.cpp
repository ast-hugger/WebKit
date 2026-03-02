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
#include <JavaScriptCore/WasmGCTypeRegistry.h>
#include <JavaScriptCore/WasmOps.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace TestWebKitAPI {

using namespace JSC::Wasm;

// Helper to create a simple function type: (i32) -> i32
static WasmGCFunctionType* createI32ToI32FunctionType()
{
    auto* func = WasmGCFunctionType::tryCreate(1, 1);
    EXPECT_TRUE(func);
    func->getReturnType(0) = Types::I32;
    func->getArgumentType(0) = Types::I32;
    return func;
}

// Helper to create a function type: (i64) -> i32
static WasmGCFunctionType* createI64ToI32FunctionType()
{
    auto* func = WasmGCFunctionType::tryCreate(1, 1);
    EXPECT_TRUE(func);
    func->getReturnType(0) = Types::I32;
    func->getArgumentType(0) = Types::I64;
    return func;
}

// Helper to create a function type: (i32, i32) -> i32
static WasmGCFunctionType* createI32I32ToI32FunctionType()
{
    auto* func = WasmGCFunctionType::tryCreate(1, 2);
    EXPECT_TRUE(func);
    func->getReturnType(0) = Types::I32;
    func->getArgumentType(0) = Types::I32;
    func->getArgumentType(1) = Types::I32;
    return func;
}

TEST(WasmGCTypeRegistry, FindTypeReturnsNullForUnknownType)
{
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* func = createI32ToI32FunctionType();
    ASSERT_TRUE(func);

    {
        Locker locker { registry.lock() };
        EXPECT_EQ(registry.findType(func), nullptr);
    }

    func->destroy();
}

TEST(WasmGCTypeRegistry, FindTypeReturnsCanonicalTypeAfterRegistration)
{
    auto& registry = WasmGCTypeRegistry::singleton();

    // Create and register one type via a root set.
    auto* original = createI32ToI32FunctionType();
    ASSERT_TRUE(original);

    WasmGCTypeRootSet rootSet;
    rootSet.append(original);

    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&rootSet);
    }

    // Create a structurally equal but different-address type.
    auto* duplicate = createI32ToI32FunctionType();
    ASSERT_TRUE(duplicate);
    ASSERT_NE(original, duplicate);

    {
        Locker locker { registry.lock() };
        auto* found = registry.findType(duplicate);
        EXPECT_EQ(found, original);
    }

    duplicate->destroy();

    // Cleanup: deregister root set (which will collect the types).
    registry.deregisterRootSet(&rootSet);
}

TEST(WasmGCTypeRegistry, FindTypeDistinguishesDifferentTypes)
{
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* funcI32 = createI32ToI32FunctionType();
    ASSERT_TRUE(funcI32);

    WasmGCTypeRootSet rootSet;
    rootSet.append(funcI32);

    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&rootSet);
    }

    // Create a different type: (i64) -> i32.
    auto* funcI64 = createI64ToI32FunctionType();
    ASSERT_TRUE(funcI64);

    {
        Locker locker { registry.lock() };
        EXPECT_EQ(registry.findType(funcI64), nullptr);
    }

    funcI64->destroy();
    registry.deregisterRootSet(&rootSet);
}

TEST(WasmGCTypeRegistry, RootSetRegistrationAddsTypes)
{
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* func1 = createI32ToI32FunctionType();
    auto* func2 = createI32I32ToI32FunctionType();
    ASSERT_TRUE(func1 && func2);

    WasmGCTypeRootSet rootSet;
    rootSet.append(func1);
    rootSet.append(func2);

    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&rootSet);
    }

    // Both should be findable via structurally equal duplicates.
    auto* dup1 = createI32ToI32FunctionType();
    auto* dup2 = createI32I32ToI32FunctionType();

    {
        Locker locker { registry.lock() };
        EXPECT_EQ(registry.findType(dup1), func1);
        EXPECT_EQ(registry.findType(dup2), func2);
    }

    dup1->destroy();
    dup2->destroy();
    registry.deregisterRootSet(&rootSet);
}

TEST(WasmGCTypeRegistry, MultiModuleRootSetLifecycle)
{
    auto& registry = WasmGCTypeRegistry::singleton();

    // Module A uses types T1, T2; Module B uses types T2copy, T3.
    // T2 and T2copy are structurally equal.
    auto* t1 = createI32ToI32FunctionType();   // unique to module A
    auto* t2 = createI32I32ToI32FunctionType(); // shared (in both modules)
    auto* t3 = createI64ToI32FunctionType();    // unique to module B
    ASSERT_TRUE(t1 && t2 && t3);

    WasmGCTypeRootSet rootSetA;
    rootSetA.append(t1);
    rootSetA.append(t2);

    WasmGCTypeRootSet rootSetB;
    rootSetB.append(t2); // same pointer — simulating that dedup happened at registration time
    rootSetB.append(t3);

    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&rootSetA);
        registry.registerRootSet(&rootSetB);
    }

    // Deregister module A — collection should remove T1 but keep T2, T3.
    registry.deregisterRootSet(&rootSetA);

    // Create probes to check which types survived.
    auto* probeT1 = createI32ToI32FunctionType();
    auto* probeT2 = createI32I32ToI32FunctionType();
    auto* probeT3 = createI64ToI32FunctionType();

    {
        Locker locker { registry.lock() };
        // T1 should be collected (only in module A).
        EXPECT_EQ(registry.findType(probeT1), nullptr);
        // T2 should survive (in module B).
        EXPECT_EQ(registry.findType(probeT2), t2);
        // T3 should survive (in module B).
        EXPECT_EQ(registry.findType(probeT3), t3);
    }

    probeT1->destroy();
    probeT2->destroy();
    probeT3->destroy();
    registry.deregisterRootSet(&rootSetB);
}

TEST(WasmGCTypeRegistry, CollectionWithNoRemainingRoots)
{
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* func = createI32ToI32FunctionType();
    ASSERT_TRUE(func);

    WasmGCTypeRootSet rootSet;
    rootSet.append(func);

    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&rootSet);
    }

    // Deregister the only root set — all types should be collected.
    registry.deregisterRootSet(&rootSet);

    auto* probe = createI32ToI32FunctionType();
    {
        Locker locker { registry.lock() };
        EXPECT_EQ(registry.findType(probe), nullptr);
    }

    probe->destroy();
}

TEST(WasmGCTypeRegistry, RTTRegistration)
{
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* func = createI32ToI32FunctionType();
    ASSERT_TRUE(func);

    WasmGCTypeRootSet rootSet;
    rootSet.append(func);

    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&rootSet);
    }

    registry.registerCanonicalRTTForType(func);
    auto rtt = registry.getCanonicalRTT(func);
    EXPECT_EQ(rtt->kind(), RTTKind::Function);

    registry.deregisterRootSet(&rootSet);
}

TEST(WasmGCTypeRegistry, RTTWithSupertype)
{
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* parent = createI32ToI32FunctionType();
    auto* child = createI32ToI32FunctionType();
    ASSERT_TRUE(parent && child);

    parent->setIsFinal(false);
    child->setSupertype(parent);
    child->setIsFinal(true);

    WasmGCTypeRootSet rootSet;
    rootSet.append(parent);
    rootSet.append(child);

    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&rootSet);
    }

    // Register parent RTT first, then child.
    registry.registerCanonicalRTTForType(parent);
    registry.registerCanonicalRTTForType(child);

    auto parentRTT = registry.getCanonicalRTT(parent);
    auto childRTT = registry.getCanonicalRTT(child);

    EXPECT_EQ(parentRTT->kind(), RTTKind::Function);
    EXPECT_EQ(childRTT->kind(), RTTKind::Function);

    // Verify the subtype relationship via the RTT display structure:
    // the parent has no ancestors, the child's display contains the parent.
    EXPECT_EQ(parentRTT->displaySizeExcludingThis(), 0u);
    EXPECT_EQ(childRTT->displaySizeExcludingThis(), 1u);
    EXPECT_EQ(childRTT->displayEntry(0), &parentRTT.get());

    registry.deregisterRootSet(&rootSet);
}

// --- A. All type kinds in registry ---

static WasmGCStructType* createI32StructType()
{
    FieldType fields[] = { { StorageType(Types::I32), Mutable } };
    return WasmGCStructType::tryCreate(fields);
}

static WasmGCArrayType* createI32ArrayType()
{
    FieldType elementType = { StorageType(Types::I32), Mutable };
    return WasmGCArrayType::tryCreate(elementType);
}

TEST(WasmGCTypeRegistry, FindTypeWorksForStructTypes)
{
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* original = createI32StructType();
    ASSERT_TRUE(original);

    WasmGCTypeRootSet rootSet;
    rootSet.append(original);

    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&rootSet);
    }

    auto* duplicate = createI32StructType();
    ASSERT_TRUE(duplicate);

    {
        Locker locker { registry.lock() };
        EXPECT_EQ(registry.findType(duplicate), original);
    }

    duplicate->destroy();
    registry.deregisterRootSet(&rootSet);
}

TEST(WasmGCTypeRegistry, FindTypeWorksForArrayTypes)
{
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* original = createI32ArrayType();
    ASSERT_TRUE(original);

    WasmGCTypeRootSet rootSet;
    rootSet.append(original);

    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&rootSet);
    }

    auto* duplicate = createI32ArrayType();
    ASSERT_TRUE(duplicate);

    {
        Locker locker { registry.lock() };
        EXPECT_EQ(registry.findType(duplicate), original);
    }

    duplicate->destroy();
    registry.deregisterRootSet(&rootSet);
}

TEST(WasmGCTypeRegistry, MixedTypeKindsInRootSet)
{
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* func = createI32ToI32FunctionType();
    auto* structType = createI32StructType();
    auto* arrayType = createI32ArrayType();
    ASSERT_TRUE(func && structType && arrayType);

    WasmGCTypeRootSet rootSet;
    rootSet.append(func);
    rootSet.append(structType);
    rootSet.append(arrayType);

    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&rootSet);
    }

    auto* probeFunc = createI32ToI32FunctionType();
    auto* probeStruct = createI32StructType();
    auto* probeArray = createI32ArrayType();

    {
        Locker locker { registry.lock() };
        EXPECT_EQ(registry.findType(probeFunc), func);
        EXPECT_EQ(registry.findType(probeStruct), structType);
        EXPECT_EQ(registry.findType(probeArray), arrayType);
    }

    probeFunc->destroy();
    probeStruct->destroy();
    probeArray->destroy();
    registry.deregisterRootSet(&rootSet);
}

// --- B. Collection preserves types referenced by fields ---

TEST(WasmGCTypeRegistry, CollectionPreservesTypeReferencedByStructField)
{
    auto& registry = WasmGCTypeRegistry::singleton();

    // Create an inner type and a struct that references it.
    auto* inner = createI32ToI32FunctionType();
    ASSERT_TRUE(inner);

    Type refToInner = { TypeKind::Ref, inner->index() };
    FieldType fields[] = { { StorageType(refToInner), Mutable } };
    auto* outer = WasmGCStructType::tryCreate(fields);
    ASSERT_TRUE(outer);

    // Register both via root set A.
    WasmGCTypeRootSet rootSetA;
    rootSetA.append(inner);
    rootSetA.append(outer);

    // Register only the outer type via root set B (inner is reachable via field).
    WasmGCTypeRootSet rootSetB;
    rootSetB.append(outer);

    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&rootSetA);
        registry.registerRootSet(&rootSetB);
    }

    // Deregister root set A. The inner type is only reachable through
    // outer's field — the mark traversal must follow it.
    registry.deregisterRootSet(&rootSetA);

    auto* probeInner = createI32ToI32FunctionType();
    {
        Locker locker { registry.lock() };
        // inner should survive because outer's field references it.
        EXPECT_EQ(registry.findType(probeInner), inner);
    }

    probeInner->destroy();
    registry.deregisterRootSet(&rootSetB);
}

TEST(WasmGCTypeRegistry, CollectionPreservesTypeReferencedByArrayElement)
{
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* inner = createI32ToI32FunctionType();
    ASSERT_TRUE(inner);

    Type refToInner = { TypeKind::RefNull, inner->index() };
    FieldType elementType = { StorageType(refToInner), Mutable };
    auto* outer = WasmGCArrayType::tryCreate(elementType);
    ASSERT_TRUE(outer);

    WasmGCTypeRootSet rootSetA;
    rootSetA.append(inner);
    rootSetA.append(outer);

    WasmGCTypeRootSet rootSetB;
    rootSetB.append(outer);

    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&rootSetA);
        registry.registerRootSet(&rootSetB);
    }

    registry.deregisterRootSet(&rootSetA);

    auto* probeInner = createI32ToI32FunctionType();
    {
        Locker locker { registry.lock() };
        EXPECT_EQ(registry.findType(probeInner), inner);
    }

    probeInner->destroy();
    registry.deregisterRootSet(&rootSetB);
}

TEST(WasmGCTypeRegistry, CollectionPreservesTypeReferencedByFunctionSignature)
{
    auto& registry = WasmGCTypeRegistry::singleton();

    // Create an inner struct type referenced by a function's argument.
    auto* inner = createI32StructType();
    ASSERT_TRUE(inner);

    Type refToInner = { TypeKind::Ref, inner->index() };
    auto* func = WasmGCFunctionType::tryCreate(1, 1);
    ASSERT_TRUE(func);
    func->getReturnType(0) = Types::I32;
    func->getArgumentType(0) = refToInner;

    WasmGCTypeRootSet rootSetA;
    rootSetA.append(inner);
    rootSetA.append(func);

    WasmGCTypeRootSet rootSetB;
    rootSetB.append(func);

    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&rootSetA);
        registry.registerRootSet(&rootSetB);
    }

    registry.deregisterRootSet(&rootSetA);

    auto* probeInner = createI32StructType();
    {
        Locker locker { registry.lock() };
        EXPECT_EQ(registry.findType(probeInner), inner);
    }

    probeInner->destroy();
    registry.deregisterRootSet(&rootSetB);
}

// --- C. Collection of recursive types ---

TEST(WasmGCTypeRegistry, CollectionHandlesSelfReferentialStruct)
{
    auto& registry = WasmGCTypeRegistry::singleton();

    // Create struct S { field: ref S } by patching after construction.
    Type placeholder = { TypeKind::Ref, static_cast<TypeIndex>(0) };
    FieldType fields[] = { { StorageType(placeholder), Mutable } };
    auto* s = WasmGCStructType::tryCreate(fields);
    ASSERT_TRUE(s);

    // Patch the field to point back to itself.
    Type selfRef = { TypeKind::Ref, s->index() };
    const_cast<FieldType&>(s->field(0)).type = StorageType(selfRef);

    WasmGCTypeRootSet rootSet;
    rootSet.append(s);

    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&rootSet);
    }

    // Deregister — marking must handle the cycle and sweep correctly.
    registry.deregisterRootSet(&rootSet);

    // The self-referential struct should have been collected.
    // Create a probe to verify.
    Type placeholder2 = { TypeKind::Ref, static_cast<TypeIndex>(0) };
    FieldType fields2[] = { { StorageType(placeholder2), Mutable } };
    auto* probe = WasmGCStructType::tryCreate(fields2);
    ASSERT_TRUE(probe);
    Type probeRef = { TypeKind::Ref, probe->index() };
    const_cast<FieldType&>(probe->field(0)).type = StorageType(probeRef);

    {
        Locker locker { registry.lock() };
        EXPECT_EQ(registry.findType(probe), nullptr);
    }

    probe->destroy();
}

TEST(WasmGCTypeRegistry, SelfReferentialStructSurvivesWithRoot)
{
    auto& registry = WasmGCTypeRegistry::singleton();

    Type placeholder = { TypeKind::Ref, static_cast<TypeIndex>(0) };
    FieldType fields[] = { { StorageType(placeholder), Mutable } };
    auto* s = WasmGCStructType::tryCreate(fields);
    ASSERT_TRUE(s);

    Type selfRef = { TypeKind::Ref, s->index() };
    const_cast<FieldType&>(s->field(0)).type = StorageType(selfRef);

    WasmGCTypeRootSet rootSetA;
    rootSetA.append(s);
    WasmGCTypeRootSet rootSetB;
    rootSetB.append(s);

    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&rootSetA);
        registry.registerRootSet(&rootSetB);
    }

    // Deregister one — s should survive because rootSetB still holds it.
    registry.deregisterRootSet(&rootSetA);

    Type placeholder2 = { TypeKind::Ref, static_cast<TypeIndex>(0) };
    FieldType fields2[] = { { StorageType(placeholder2), Mutable } };
    auto* probe = WasmGCStructType::tryCreate(fields2);
    ASSERT_TRUE(probe);
    Type probeRef = { TypeKind::Ref, probe->index() };
    const_cast<FieldType&>(probe->field(0)).type = StorageType(probeRef);

    {
        Locker locker { registry.lock() };
        EXPECT_EQ(registry.findType(probe), s);
    }

    probe->destroy();
    registry.deregisterRootSet(&rootSetB);
}

// --- D. Supertype keeps referenced type alive ---

TEST(WasmGCTypeRegistry, CollectionPreservesSupertypeChain)
{
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* parent = createI32ToI32FunctionType();
    auto* child = createI32ToI32FunctionType();
    ASSERT_TRUE(parent && child);

    parent->setIsFinal(false);
    child->setSupertype(parent);

    // Register both via rootSetA, only child via rootSetB.
    WasmGCTypeRootSet rootSetA;
    rootSetA.append(parent);
    rootSetA.append(child);

    WasmGCTypeRootSet rootSetB;
    rootSetB.append(child);

    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&rootSetA);
        registry.registerRootSet(&rootSetB);
    }

    // Deregister rootSetA. Parent is reachable via child's m_supertype.
    registry.deregisterRootSet(&rootSetA);

    // Parent should still be alive because child's supertype points to it.
    auto* probeParent = createI32ToI32FunctionType();
    probeParent->setIsFinal(false);
    {
        Locker locker { registry.lock() };
        EXPECT_EQ(registry.findType(probeParent), parent);
    }

    probeParent->destroy();
    registry.deregisterRootSet(&rootSetB);
}

// --- E. RTT for struct and array types ---

TEST(WasmGCTypeRegistry, RTTForStructType)
{
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* structType = createI32StructType();
    ASSERT_TRUE(structType);

    WasmGCTypeRootSet rootSet;
    rootSet.append(structType);

    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&rootSet);
    }

    registry.registerCanonicalRTTForType(structType);
    auto rtt = registry.getCanonicalRTT(structType);
    EXPECT_EQ(rtt->kind(), RTTKind::Struct);
    EXPECT_EQ(rtt->fieldCount(), 1u);

    registry.deregisterRootSet(&rootSet);
}

TEST(WasmGCTypeRegistry, RTTForArrayType)
{
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* arrayType = createI32ArrayType();
    ASSERT_TRUE(arrayType);

    WasmGCTypeRootSet rootSet;
    rootSet.append(arrayType);

    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&rootSet);
    }

    registry.registerCanonicalRTTForType(arrayType);
    auto rtt = registry.getCanonicalRTT(arrayType);
    EXPECT_EQ(rtt->kind(), RTTKind::Array);

    registry.deregisterRootSet(&rootSet);
}

// --- F. RTT idempotence ---

TEST(WasmGCTypeRegistry, RTTRegistrationIsIdempotent)
{
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* func = createI32ToI32FunctionType();
    ASSERT_TRUE(func);

    WasmGCTypeRootSet rootSet;
    rootSet.append(func);

    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&rootSet);
    }

    registry.registerCanonicalRTTForType(func);
    auto rtt1 = registry.getCanonicalRTT(func);

    // Register again — should be a no-op.
    registry.registerCanonicalRTTForType(func);
    auto rtt2 = registry.getCanonicalRTT(func);

    EXPECT_EQ(&rtt1.get(), &rtt2.get());

    registry.deregisterRootSet(&rootSet);
}

// --- G. deregisterRootSet clears the root set ---

TEST(WasmGCTypeRegistry, DeregisterClearsRootSet)
{
    auto& registry = WasmGCTypeRegistry::singleton();

    auto* func = createI32ToI32FunctionType();
    ASSERT_TRUE(func);

    WasmGCTypeRootSet rootSet;
    rootSet.append(func);
    EXPECT_EQ(rootSet.size(), 1u);

    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&rootSet);
    }

    registry.deregisterRootSet(&rootSet);

    // Root set should be cleared after deregistration.
    EXPECT_EQ(rootSet.size(), 0u);
}

// --- H. Re-registration after full collection ---

TEST(WasmGCTypeRegistry, ReregistrationAfterFullCollection)
{
    auto& registry = WasmGCTypeRegistry::singleton();

    // First cycle: register and deregister.
    auto* func1 = createI32ToI32FunctionType();
    ASSERT_TRUE(func1);

    WasmGCTypeRootSet rootSet1;
    rootSet1.append(func1);

    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&rootSet1);
    }

    registry.deregisterRootSet(&rootSet1);

    // Second cycle: register new types.
    auto* func2 = createI64ToI32FunctionType();
    ASSERT_TRUE(func2);

    WasmGCTypeRootSet rootSet2;
    rootSet2.append(func2);

    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&rootSet2);
    }

    auto* probe = createI64ToI32FunctionType();
    {
        Locker locker { registry.lock() };
        // Old type should be gone.
        auto* probeOld = createI32ToI32FunctionType();
        EXPECT_EQ(registry.findType(probeOld), nullptr);
        probeOld->destroy();

        // New type should be found.
        EXPECT_EQ(registry.findType(probe), func2);
    }

    probe->destroy();
    registry.deregisterRootSet(&rootSet2);
}

// --- I. Empty root set ---

TEST(WasmGCTypeRegistry, EmptyRootSetRegistration)
{
    auto& registry = WasmGCTypeRegistry::singleton();

    WasmGCTypeRootSet rootSet;

    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&rootSet);
    }

    // Deregistering an empty root set should not crash.
    registry.deregisterRootSet(&rootSet);
    EXPECT_EQ(rootSet.size(), 0u);
}

// --- J. Mutually recursive types survive and are collected together ---

TEST(WasmGCTypeRegistry, MutuallyRecursiveTypesSurviveViaRootSet)
{
    auto& registry = WasmGCTypeRegistry::singleton();

    // Create A { ref B } and B { ref A }.
    Type placeholderA = { TypeKind::Ref, static_cast<TypeIndex>(0) };
    Type placeholderB = { TypeKind::Ref, static_cast<TypeIndex>(0) };
    FieldType fieldsA[] = { { StorageType(placeholderA), Mutable } };
    FieldType fieldsB[] = { { StorageType(placeholderB), Mutable } };

    auto* a = WasmGCStructType::tryCreate(fieldsA);
    auto* b = WasmGCStructType::tryCreate(fieldsB);
    ASSERT_TRUE(a && b);

    // Patch: A's field points to B, B's field points to A.
    Type refToB = { TypeKind::Ref, b->index() };
    Type refToA = { TypeKind::Ref, a->index() };
    const_cast<FieldType&>(a->field(0)).type = StorageType(refToB);
    const_cast<FieldType&>(b->field(0)).type = StorageType(refToA);

    // rootSetA has both types (simulating initial module load).
    // rootSetB also has both (simulating a second module sharing the same types).
    // After deregistering rootSetA, both types survive via rootSetB.
    WasmGCTypeRootSet rootSetA;
    rootSetA.append(a);
    rootSetA.append(b);

    WasmGCTypeRootSet rootSetB;
    rootSetB.append(a);
    rootSetB.append(b);

    {
        Locker locker { registry.lock() };
        registry.registerRootSet(&rootSetA);
        registry.registerRootSet(&rootSetB);
    }

    registry.deregisterRootSet(&rootSetA);

    // Both should survive — both are rooted by rootSetB, and the mark
    // traversal correctly handles the mutual references.
    Type ph1 = { TypeKind::Ref, static_cast<TypeIndex>(0) };
    Type ph2 = { TypeKind::Ref, static_cast<TypeIndex>(0) };
    FieldType probeFieldsA[] = { { StorageType(ph1), Mutable } };
    FieldType probeFieldsB[] = { { StorageType(ph2), Mutable } };
    auto* probeA = WasmGCStructType::tryCreate(probeFieldsA);
    auto* probeB = WasmGCStructType::tryCreate(probeFieldsB);
    ASSERT_TRUE(probeA && probeB);
    Type probeRefToB = { TypeKind::Ref, probeB->index() };
    Type probeRefToA = { TypeKind::Ref, probeA->index() };
    const_cast<FieldType&>(probeA->field(0)).type = StorageType(probeRefToB);
    const_cast<FieldType&>(probeB->field(0)).type = StorageType(probeRefToA);

    {
        Locker locker { registry.lock() };
        EXPECT_NE(registry.findType(probeA), nullptr);
        EXPECT_NE(registry.findType(probeB), nullptr);
    }

    probeA->destroy();
    probeB->destroy();
    registry.deregisterRootSet(&rootSetB);
}

} // namespace TestWebKitAPI

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(WEBASSEMBLY)
