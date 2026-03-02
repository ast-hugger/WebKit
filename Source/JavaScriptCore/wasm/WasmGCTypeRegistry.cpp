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
#include "WasmGCTypeRegistry.h"

#if ENABLE(WEBASSEMBLY)

#include <mutex>
#include <wtf/Atomics.h>
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace Wasm {

WTF_MAKE_TZONE_ALLOCATED_IMPL(WasmGCTypeRegistry);

WasmGCTypeRegistry::WasmGCTypeRegistry() = default;

WasmGCTypeRegistry& WasmGCTypeRegistry::singleton()
{
    static WasmGCTypeRegistry* theOne;
    static std::once_flag registryFlag;

    std::call_once(registryFlag, [] () {
        theOne = new WasmGCTypeRegistry;
    });
    return *theOne;
}

WasmGCType* WasmGCTypeRegistry::findType(const WasmGCType* candidate)
{
    assertIsHeld(m_lock);
    auto it = m_typeSet.find(WasmGCTypeHash { candidate });
    if (it != m_typeSet.end())
        return const_cast<WasmGCType*>(it->key);
    return nullptr;
}

void WasmGCTypeRegistry::registerRootSet(WasmGCTypeRootSet* rootSet)
{
    assertIsHeld(m_lock);
    for (auto* type : rootSet->types())
        m_typeSet.add(WasmGCTypeHash { type });
    m_rootSets.add(rootSet);
}

void WasmGCTypeRegistry::deregisterRootSet(WasmGCTypeRootSet* rootSet)
{
    Locker locker { m_lock };
    m_rootSets.remove(rootSet);
    collect();
    rootSet->clear();
}

void WasmGCTypeRegistry::markTypeFromIndex(TypeIndex index)
{
    if (typeIndexIsType(index))
        return;
    auto* type = std::bit_cast<WasmGCType*>(index);
    markType(type);
}

void WasmGCTypeRegistry::markStorageType(const StorageType& storage)
{
    if (storage.is<Type>())
        markTypeFromIndex(storage.as<Type>().index);
}

void WasmGCTypeRegistry::markType(WasmGCType* type)
{
    if (!type || type->marked())
        return;

    type->setMarked(true);

    // Follow supertype.
    markType(type->supertype());

    switch (type->typeKind()) {
    case WasmGCTypeKind::FunctionType: {
        auto* func = type->as<WasmGCFunctionType>();
        for (FunctionArgCount i = 0; i < func->argumentCount(); ++i)
            markTypeFromIndex(func->argumentType(i).index);
        for (FunctionArgCount i = 0; i < func->returnCount(); ++i)
            markTypeFromIndex(func->returnType(i).index);
        break;
    }
    case WasmGCTypeKind::StructType: {
        auto* structType = type->as<WasmGCStructType>();
        for (StructFieldCount i = 0; i < structType->fieldCount(); ++i)
            markStorageType(structType->field(i).type);
        break;
    }
    case WasmGCTypeKind::ArrayType: {
        auto* arrayType = type->as<WasmGCArrayType>();
        markStorageType(arrayType->elementType().type);
        break;
    }
    }
}

void WasmGCTypeRegistry::collect()
{
    // Mark phase: trace from all root sets.
    for (auto* rootSet : m_rootSets) {
        for (auto* type : rootSet->types())
            markType(type);
    }

    // Sweep phase: remove unmarked types, reset marked ones.
    m_typeSet.removeIf([](auto& entry) {
        auto* type = const_cast<WasmGCType*>(entry.key);
        if (!type->marked()) {
            type->destroy();
            return true;
        }
        type->setMarked(false);
        return false;
    });
}

void WasmGCTypeRegistry::registerCanonicalRTTForType(WasmGCType* type)
{
    if (type->m_rtt)
        return;

    Locker locker { type->m_rttLock };
    if (type->m_rtt)
        return;

    auto rtt = createCanonicalRTTForType(type);
    WTF::storeStoreFence();
    type->m_rtt = WTF::move(rtt);
}

Ref<RTT> WasmGCTypeRegistry::createCanonicalRTTForType(const WasmGCType* type)
{
    bool isFinalType = type->isFinal();
    RTTKind kind;
    StructFieldCount fieldCount = 0;

    switch (type->typeKind()) {
    case WasmGCTypeKind::FunctionType:
        kind = RTTKind::Function;
        break;
    case WasmGCTypeKind::ArrayType:
        kind = RTTKind::Array;
        break;
    case WasmGCTypeKind::StructType:
        kind = RTTKind::Struct;
        fieldCount = type->as<WasmGCStructType>()->fieldCount();
        break;
    }

    if (type->supertype() && type->supertype()->m_rtt) {
        auto superRTT = type->supertype()->m_rtt;
        auto result = RTT::tryCreate(kind, *superRTT, isFinalType, fieldCount);
        RELEASE_ASSERT(result);
        return result.releaseNonNull();
    }

    auto result = RTT::tryCreate(kind, isFinalType, fieldCount);
    RELEASE_ASSERT(result);
    return result.releaseNonNull();
}

Ref<const RTT> WasmGCTypeRegistry::getCanonicalRTT(const WasmGCType* type)
{
    auto result = type->m_rtt;
    RELEASE_ASSERT(result);
    return result.releaseNonNull();
}

} } // namespace JSC::Wasm

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(WEBASSEMBLY)
