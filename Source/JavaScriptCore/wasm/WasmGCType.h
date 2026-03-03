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

#pragma once

#include <wtf/Platform.h>

#if ENABLE(WEBASSEMBLY)

#include <JavaScriptCore/JSExportMacros.h>
#include <JavaScriptCore/WasmOps.h>
#include <JavaScriptCore/WasmTypeDefinition.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashFunctions.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/HashTraits.h>
#include <wtf/Lock.h>
#include <wtf/PrintStream.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace Wasm {

class WasmGCFunctionType;
class WasmGCStructType;
class WasmGCArrayType;
class WasmGCTypeBuilder;

enum class WasmGCTypeKind : uint8_t {
    FunctionType,
    StructType,
    ArrayType
};

class WasmGCType {
    WTF_MAKE_NONCOPYABLE(WasmGCType);
    WTF_MAKE_NONMOVABLE(WasmGCType);
public:
    template <typename T>
    bool is() const { return m_kind == T::kind; }

    template <typename T>
    T* as() { ASSERT(is<T>()); return static_cast<T*>(this); }

    template <typename T>
    const T* as() const { ASSERT(is<T>()); return static_cast<const T*>(this); }

    WasmGCTypeKind typeKind() const { return m_kind; }
    TypeIndex index() const { return std::bit_cast<TypeIndex>(this); }

    static WasmGCType* fromIndex(TypeIndex index) { return std::bit_cast<WasmGCType*>(index); }

    bool isFinal() const { return m_isFinal; }
    void setIsFinal(bool value) { m_isFinal = value; }

    WasmGCType* supertype() const { return m_supertype; }
    void setSupertype(WasmGCType* supertype) { m_supertype = supertype; }

    bool marked() const { return m_marked; }
    void setMarked(bool value) { m_marked = value; }

    mutable Lock m_rttLock;
    mutable RefPtr<RTT> m_rtt;

    JS_EXPORT_PRIVATE unsigned hash() const;

    JS_EXPORT_PRIVATE static bool structurallyEqual(const WasmGCType* a, const WasmGCType* b);
    JS_EXPORT_PRIVATE static bool structurallyEqualImpl(const WasmGCType* a, const WasmGCType* b, HashSet<std::pair<const WasmGCType*, const WasmGCType*>>& visited);

    JS_EXPORT_PRIVATE void dump(WTF::PrintStream& out) const;

    JS_EXPORT_PRIVATE void destroy();

protected:
    WasmGCType(WasmGCTypeKind kind)
        : m_kind(kind)
    {
    }

    WasmGCTypeKind m_kind;
    bool m_isFinal { true };
    bool m_marked { false };
    WasmGCType* m_supertype { nullptr };
};

class WasmGCFunctionType final : public WasmGCType {
    WTF_MAKE_NONCOPYABLE(WasmGCFunctionType);
    WTF_MAKE_NONMOVABLE(WasmGCFunctionType);
public:
    static constexpr WasmGCTypeKind kind = WasmGCTypeKind::FunctionType;

    static size_t allocationSize(Checked<FunctionArgCount> retCount, Checked<FunctionArgCount> argCount)
    {
        return sizeof(WasmGCFunctionType) + (retCount + argCount) * sizeof(Type);
    }

    JS_EXPORT_PRIVATE static WasmGCFunctionType* tryCreate(FunctionArgCount returnCount, FunctionArgCount argumentCount);

    FunctionArgCount argumentCount() const { return m_argCount; }
    FunctionArgCount returnCount() const { return m_retCount; }
    bool returnsVoid() const { return !returnCount(); }

    Type returnType(FunctionArgCount i) const { ASSERT(i < returnCount()); return *storage(i); }
    Type argumentType(FunctionArgCount i) const { ASSERT(i < argumentCount()); return *storage(returnCount() + i); }

    Type& getReturnType(FunctionArgCount i) { ASSERT(i < returnCount()); return *storage(i); }
    Type& getArgumentType(FunctionArgCount i) { ASSERT(i < argumentCount()); return *storage(returnCount() + i); }

    Type* storage(FunctionArgCount i) { return payload() + i; }
    const Type* storage(FunctionArgCount i) const { return const_cast<WasmGCFunctionType*>(this)->storage(i); }

    bool hasReturnVector() const
    {
        for (size_t i = 0; i < returnCount(); ++i) {
            if (returnType(i).isV128())
                return true;
        }
        return false;
    }

    size_t numVectors() const
    {
        size_t n = 0;
        for (size_t i = 0; i < argumentCount(); ++i) {
            if (argumentType(i).isV128())
                ++n;
        }
        return n;
    }

    size_t numReturnVectors() const
    {
        size_t n = 0;
        for (size_t i = 0; i < returnCount(); ++i) {
            if (returnType(i).isV128())
                ++n;
        }
        return n;
    }

    bool argumentsOrResultsIncludeV128() const
    {
        for (size_t i = 0; i < argumentCount(); ++i) {
            if (argumentType(i).isV128())
                return true;
        }
        for (size_t i = 0; i < returnCount(); ++i) {
            if (returnType(i).isV128())
                return true;
        }
        return false;
    }

    bool argumentsOrResultsIncludeI64() const
    {
        for (size_t i = 0; i < argumentCount(); ++i) {
            if (argumentType(i).isI64())
                return true;
        }
        for (size_t i = 0; i < returnCount(); ++i) {
            if (returnType(i).isI64())
                return true;
        }
        return false;
    }

    bool argumentsOrResultsIncludeExnref() const
    {
        for (size_t i = 0; i < argumentCount(); ++i) {
            Type t = argumentType(i);
            if ((t.isRef() || t.isRefNull()) && t.index == static_cast<TypeIndex>(TypeKind::Exnref))
                return true;
        }
        for (size_t i = 0; i < returnCount(); ++i) {
            Type t = returnType(i);
            if ((t.isRef() || t.isRefNull()) && t.index == static_cast<TypeIndex>(TypeKind::Exnref))
                return true;
        }
        return false;
    }

#if ENABLE(JIT)
    CodePtr<JSEntryPtrTag> jsToWasmICEntrypoint() const;
    mutable RefPtr<JSToWasmICCallee> m_jsToWasmICCallee;
    mutable Lock m_jitCodeLock;
#endif

    JS_EXPORT_PRIVATE void dump(WTF::PrintStream& out) const;

private:
    WasmGCFunctionType(FunctionArgCount argumentCount, FunctionArgCount returnCount);

    Type* payload() { return std::bit_cast<Type*>(this + 1); }

    FunctionArgCount m_argCount;
    FunctionArgCount m_retCount;
};

class WasmGCStructType final : public WasmGCType {
    WTF_MAKE_NONCOPYABLE(WasmGCStructType);
    WTF_MAKE_NONMOVABLE(WasmGCStructType);
    friend class WasmGCTypeBuilder;
public:
    static constexpr WasmGCTypeKind kind = WasmGCTypeKind::StructType;

    static size_t allocationSize(Checked<StructFieldCount> fieldCount)
    {
        return sizeof(WasmGCStructType) + fieldCount * (sizeof(FieldType) + sizeof(unsigned));
    }

    JS_EXPORT_PRIVATE static WasmGCStructType* tryCreate(std::span<const FieldType> fields);

    StructFieldCount fieldCount() const { return m_fieldCount; }
    const FieldType& field(StructFieldCount i) const { return fields()[i]; }
    std::span<const FieldType> fields() const { return { payload(), m_fieldCount }; }

    bool hasRefFieldTypes() const
    {
        for (StructFieldCount i = 0; i < m_fieldCount; ++i) {
            auto storageType = fields()[i].type;
            if (storageType.is<Type>() && (storageType.as<Type>().isRef() || storageType.as<Type>().isRefNull()))
                return true;
        }
        return false;
    }

    unsigned offsetOfFieldInPayload(StructFieldCount i) const { return const_cast<WasmGCStructType*>(this)->fieldOffsetFromInstancePayload(i); }
    size_t instancePayloadSize() const { return m_instancePayloadSize; }

    JS_EXPORT_PRIVATE void dump(WTF::PrintStream& out) const;

private:
    explicit WasmGCStructType(std::span<const FieldType> fields);

    std::span<FieldType> mutableFields() { return { payload(), m_fieldCount }; }
    unsigned& fieldOffsetFromInstancePayload(StructFieldCount i) { ASSERT(i < fieldCount()); return *(std::bit_cast<unsigned*>(payload() + m_fieldCount) + i); }
    FieldType* payload() const { return std::bit_cast<FieldType*>(this + 1); }

    StructFieldCount m_fieldCount;
    size_t m_instancePayloadSize;
};

class WasmGCArrayType final : public WasmGCType {
    WTF_MAKE_NONCOPYABLE(WasmGCArrayType);
    WTF_MAKE_NONMOVABLE(WasmGCArrayType);
    friend class WasmGCTypeBuilder;
public:
    static constexpr WasmGCTypeKind kind = WasmGCTypeKind::ArrayType;

    static size_t allocationSize() { return sizeof(WasmGCArrayType); }

    JS_EXPORT_PRIVATE static WasmGCArrayType* tryCreate(const FieldType& elementType);

    const FieldType& elementType() const { return m_elementType; }

    JS_EXPORT_PRIVATE void dump(WTF::PrintStream& out) const;

private:
    WasmGCArrayType(const FieldType& elementType);

    FieldType m_elementType { };
};

struct WasmGCTypeHash {
    const WasmGCType* key { nullptr };
    WasmGCTypeHash() = default;
    explicit WasmGCTypeHash(const WasmGCType* key)
        : key(key)
    { }
    explicit WasmGCTypeHash(WTF::HashTableDeletedValueType)
        : key(std::bit_cast<const WasmGCType*>(static_cast<uintptr_t>(1)))
    { }
    bool operator==(const WasmGCTypeHash& rhs) const { return equal(*this, rhs); }
    static bool equal(const WasmGCTypeHash& lhs, const WasmGCTypeHash& rhs) { return WasmGCType::structurallyEqual(lhs.key, rhs.key); }
    static unsigned hash(const WasmGCTypeHash& typeHash) { return typeHash.key ? typeHash.key->hash() : 0; }
    static constexpr bool safeToCompareToEmptyOrDeleted = false;
    bool isHashTableDeletedValue() const { return key == std::bit_cast<const WasmGCType*>(static_cast<uintptr_t>(1)); }
};

} } // namespace JSC::Wasm


namespace WTF {

template<typename T> struct DefaultHash;
template<> struct DefaultHash<JSC::Wasm::WasmGCTypeHash> : JSC::Wasm::WasmGCTypeHash { };

template<typename T> struct HashTraits;
template<> struct HashTraits<JSC::Wasm::WasmGCTypeHash> : SimpleClassHashTraits<JSC::Wasm::WasmGCTypeHash> {
    static constexpr bool emptyValueIsZero = true;
};

} // namespace WTF

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(WEBASSEMBLY)
