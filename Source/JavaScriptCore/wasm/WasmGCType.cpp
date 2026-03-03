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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WasmGCType.h"

#if ENABLE(WEBASSEMBLY)

#include <wtf/CommaPrinter.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashFunctions.h>
#include <wtf/PrintStream.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace Wasm {

// Hash a single Type field in a cycle-aware manner: include TypeKind but
// omit TypeIndex to avoid infinite recursion on circular type references.
static unsigned hashTypeKindOnly(unsigned accumulator, Type type)
{
    accumulator = WTF::pairIntHash(accumulator, WTF::IntHash<uint8_t>::hash(static_cast<uint8_t>(type.kind)));
    return accumulator;
}

static unsigned computeGCFunctionTypeHash(const WasmGCFunctionType& func)
{
    unsigned accumulator = 0xa1bcedd8u;
    for (FunctionArgCount i = 0; i < func.argumentCount(); ++i)
        accumulator = hashTypeKindOnly(accumulator, func.argumentType(i));
    for (FunctionArgCount i = 0; i < func.returnCount(); ++i)
        accumulator = hashTypeKindOnly(accumulator, func.returnType(i));
    accumulator = WTF::pairIntHash(accumulator, WTF::IntHash<uint32_t>::hash(func.argumentCount()));
    accumulator = WTF::pairIntHash(accumulator, WTF::IntHash<uint32_t>::hash(func.returnCount()));
    return accumulator;
}

static unsigned computeGCStructTypeHash(const WasmGCStructType& structType)
{
    unsigned accumulator = 0x15d2546;
    for (StructFieldCount i = 0; i < structType.fieldCount(); ++i) {
        const auto& f = structType.field(i);
        accumulator = WTF::pairIntHash(accumulator, WTF::IntHash<int8_t>::hash(f.type.typeCode()));
        accumulator = WTF::pairIntHash(accumulator, WTF::IntHash<uint8_t>::hash(static_cast<uint8_t>(f.mutability)));
    }
    accumulator = WTF::pairIntHash(accumulator, WTF::IntHash<uint32_t>::hash(structType.fieldCount()));
    return accumulator;
}

static unsigned computeGCArrayTypeHash(const WasmGCArrayType& arrayType)
{
    unsigned accumulator = 0x7835ab;
    accumulator = WTF::pairIntHash(accumulator, WTF::IntHash<int8_t>::hash(arrayType.elementType().type.typeCode()));
    accumulator = WTF::pairIntHash(accumulator, WTF::IntHash<uint8_t>::hash(static_cast<uint8_t>(arrayType.elementType().mutability)));
    return accumulator;
}

unsigned WasmGCType::hash() const
{
    unsigned h;
    switch (m_kind) {
    case WasmGCTypeKind::FunctionType:
        h = computeGCFunctionTypeHash(*as<WasmGCFunctionType>());
        break;
    case WasmGCTypeKind::StructType:
        h = computeGCStructTypeHash(*as<WasmGCStructType>());
        break;
    case WasmGCTypeKind::ArrayType:
        h = computeGCArrayTypeHash(*as<WasmGCArrayType>());
        break;
    }
    // Mix in finality and whether a supertype exists. We deliberately do NOT
    // hash the supertype pointer identity because structurallyEqual compares
    // supertypes recursively (structurally). Including pointer identity would
    // violate the hash contract (equal objects must have equal hashes) when
    // a tentative type has a different supertype pointer than the canonical
    // type in the registry.
    h = WTF::pairIntHash(h, WTF::IntHash<uint8_t>::hash(static_cast<uint8_t>(m_isFinal)));
    h = WTF::pairIntHash(h, WTF::IntHash<uint8_t>::hash(m_supertype ? 1 : 0));
    if (m_supertype)
        h = WTF::pairIntHash(h, WTF::IntHash<uint8_t>::hash(static_cast<uint8_t>(m_supertype->typeKind())));
    return h;
}

// Bisimulation-based structural equality.
bool WasmGCType::structurallyEqual(const WasmGCType* a, const WasmGCType* b)
{
    if (a == b)
        return true;
    if (!a || !b)
        return false;
    HashSet<std::pair<const WasmGCType*, const WasmGCType*>> visited;
    return structurallyEqualImpl(a, b, visited);
}

bool typesStructurallyEqual(Type a, Type b, HashSet<std::pair<const WasmGCType*, const WasmGCType*>>& visited);

bool WasmGCType::structurallyEqualImpl(const WasmGCType* a, const WasmGCType* b, HashSet<std::pair<const WasmGCType*, const WasmGCType*>>& visited)
{
    if (a == b)
        return true;
    if (!a || !b)
        return false;
    if (a->m_kind != b->m_kind)
        return false;
    if (a->m_isFinal != b->m_isFinal)
        return false;

    // Coinductive step: if we've already assumed (a, b) are equal, return true.
    auto pair = std::make_pair(a, b);
    if (visited.contains(pair))
        return true;
    visited.add(pair);
    // Also add the symmetric pair so (b, a) works.
    visited.add(std::make_pair(b, a));

    // Compare supertypes recursively.
    if (!structurallyEqualImpl(a->m_supertype, b->m_supertype, visited))
        return false;

    switch (a->m_kind) {
    case WasmGCTypeKind::FunctionType: {
        const auto* fa = a->as<WasmGCFunctionType>();
        const auto* fb = b->as<WasmGCFunctionType>();
        if (fa->argumentCount() != fb->argumentCount())
            return false;
        if (fa->returnCount() != fb->returnCount())
            return false;
        for (FunctionArgCount i = 0; i < fa->argumentCount(); ++i) {
            if (!typesStructurallyEqual(fa->argumentType(i), fb->argumentType(i), visited))
                return false;
        }
        for (FunctionArgCount i = 0; i < fa->returnCount(); ++i) {
            if (!typesStructurallyEqual(fa->returnType(i), fb->returnType(i), visited))
                return false;
        }
        return true;
    }
    case WasmGCTypeKind::StructType: {
        const auto* sa = a->as<WasmGCStructType>();
        const auto* sb = b->as<WasmGCStructType>();
        if (sa->fieldCount() != sb->fieldCount())
            return false;
        for (StructFieldCount i = 0; i < sa->fieldCount(); ++i) {
            const auto& fa = sa->field(i);
            const auto& fb = sb->field(i);
            if (fa.mutability != fb.mutability)
                return false;
            // Compare storage types: for packed types, compare directly.
            if (fa.type.is<PackedType>() != fb.type.is<PackedType>())
                return false;
            if (fa.type.is<PackedType>()) {
                if (fa.type.as<PackedType>() != fb.type.as<PackedType>())
                    return false;
            } else {
                if (!typesStructurallyEqual(fa.type.as<Type>(), fb.type.as<Type>(), visited))
                    return false;
            }
        }
        return true;
    }
    case WasmGCTypeKind::ArrayType: {
        const auto* aa = a->as<WasmGCArrayType>();
        const auto* ab = b->as<WasmGCArrayType>();
        if (aa->elementType().mutability != ab->elementType().mutability)
            return false;
        if (aa->elementType().type.is<PackedType>() != ab->elementType().type.is<PackedType>())
            return false;
        if (aa->elementType().type.is<PackedType>())
            return aa->elementType().type.as<PackedType>() == ab->elementType().type.as<PackedType>();
        return typesStructurallyEqual(aa->elementType().type.as<Type>(), ab->elementType().type.as<Type>(), visited);
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
    return false;
}

bool typesStructurallyEqual(Type a, Type b, HashSet<std::pair<const WasmGCType*, const WasmGCType*>>& visited)
{
    if (a.kind != b.kind)
        return false;
    // For non-reference types or abstract heap types, compare indices directly.
    if (typeIndexIsType(a.index) || typeIndexIsType(b.index))
        return a.index == b.index;
    if (a.index == b.index)
        return true;
    // Both have concrete TypeIndex values that are pointers to types.
    // Check if they are WasmGCType pointers and recurse.
    // Since WasmGCType uses bit_cast<TypeIndex>(this) as its index, the
    // TypeIndex IS the pointer to the WasmGCType.
    const auto* gcA = std::bit_cast<const WasmGCType*>(a.index);
    const auto* gcB = std::bit_cast<const WasmGCType*>(b.index);
    return WasmGCType::structurallyEqualImpl(gcA, gcB, visited);
}

void WasmGCType::dump(PrintStream& out) const
{
    switch (m_kind) {
    case WasmGCTypeKind::FunctionType:
        return as<WasmGCFunctionType>()->dump(out);
    case WasmGCTypeKind::StructType:
        return as<WasmGCStructType>()->dump(out);
    case WasmGCTypeKind::ArrayType:
        return as<WasmGCArrayType>()->dump(out);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void WasmGCType::destroy()
{
    switch (m_kind) {
    case WasmGCTypeKind::FunctionType:
        as<WasmGCFunctionType>()->~WasmGCFunctionType();
        break;
    case WasmGCTypeKind::StructType:
        as<WasmGCStructType>()->~WasmGCStructType();
        break;
    case WasmGCTypeKind::ArrayType:
        as<WasmGCArrayType>()->~WasmGCArrayType();
        break;
    }
    fastFree(this);
}

// WasmGCFunctionType

WasmGCFunctionType::WasmGCFunctionType(FunctionArgCount argumentCount, FunctionArgCount returnCount)
    : WasmGCType(kind)
    , m_argCount(argumentCount)
    , m_retCount(returnCount)
{
}

WasmGCFunctionType* WasmGCFunctionType::tryCreate(FunctionArgCount returnCount, FunctionArgCount argumentCount)
{
    auto result = tryFastMalloc(allocationSize(returnCount, argumentCount));
    void* memory = nullptr;
    if (!result.getValue(memory))
        return nullptr;
    return new (NotNull, memory) WasmGCFunctionType(argumentCount, returnCount);
}

void WasmGCFunctionType::dump(PrintStream& out) const
{
    {
        out.print("("_s);
        CommaPrinter comma;
        for (FunctionArgCount arg = 0; arg < argumentCount(); ++arg)
            out.print(comma, makeString(argumentType(arg).kind));
        out.print(")"_s);
    }

    {
        CommaPrinter comma;
        out.print(" -> ["_s);
        for (FunctionArgCount ret = 0; ret < returnCount(); ++ret)
            out.print(comma, makeString(returnType(ret).kind));
        out.print("]"_s);
    }
}

// WasmGCStructType

WasmGCStructType::WasmGCStructType(std::span<const FieldType> fieldTypes)
    : WasmGCType(kind)
    , m_fieldCount(fieldTypes.size())
{
    unsigned currentFieldOffset = 0;
    auto fields = mutableFields();
    for (unsigned fieldIndex = 0; fieldIndex < fieldTypes.size(); ++fieldIndex) {
        const auto& fieldType = fieldTypes[fieldIndex];
        new (&fields[fieldIndex]) FieldType(fieldType);
        const auto& fieldStorageType = fields[fieldIndex].type;
        currentFieldOffset = WTF::roundUpToMultipleOf(typeAlignmentInBytes(fieldStorageType), currentFieldOffset);
        fieldOffsetFromInstancePayload(fieldIndex) = currentFieldOffset;
        currentFieldOffset += typeSizeInBytes(fieldStorageType);
    }

    m_instancePayloadSize = WTF::roundUpToMultipleOf<sizeof(uint64_t)>(currentFieldOffset);
}

WasmGCStructType* WasmGCStructType::tryCreate(std::span<const FieldType> fields)
{
    auto result = tryFastMalloc(allocationSize(fields.size()));
    void* memory = nullptr;
    if (!result.getValue(memory))
        return nullptr;
    return new (NotNull, memory) WasmGCStructType(fields);
}

void WasmGCStructType::dump(PrintStream& out) const
{
    out.print("("_s);
    CommaPrinter comma;
    for (StructFieldCount fieldIndex = 0; fieldIndex < fieldCount(); ++fieldIndex)
        out.print(comma, field(fieldIndex).mutability ? "immutable "_s : "mutable "_s, makeString(field(fieldIndex).type));
    out.print(")"_s);
}

// WasmGCArrayType

WasmGCArrayType::WasmGCArrayType(const FieldType& elementType)
    : WasmGCType(kind)
    , m_elementType(elementType)
{
}

WasmGCArrayType* WasmGCArrayType::tryCreate(const FieldType& elementType)
{
    auto result = tryFastMalloc(allocationSize());
    void* memory = nullptr;
    if (!result.getValue(memory))
        return nullptr;
    return new (NotNull, memory) WasmGCArrayType(elementType);
}

void WasmGCArrayType::dump(PrintStream& out) const
{
    out.print("("_s);
    CommaPrinter comma;
    out.print(comma, elementType().mutability ? "immutable "_s : "mutable "_s, makeString(elementType().type));
    out.print(")"_s);
}

} } // namespace JSC::Wasm

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(WEBASSEMBLY)
