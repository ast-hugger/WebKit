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
#include <JavaScriptCore/WasmGCType.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>

namespace JSC { namespace Wasm {

class WasmGCTypeRootSet {
    WTF_MAKE_NONCOPYABLE(WasmGCTypeRootSet);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(WasmGCTypeRootSet);
public:
    WasmGCTypeRootSet() = default;

    void append(WasmGCType* type) { m_types.append(type); }
    void clear() { m_types.clear(); }
    std::span<WasmGCType* const> types() const { return m_types.span(); }
    size_t size() const { return m_types.size(); }

private:
    Vector<WasmGCType*> m_types;
};

class WasmGCTypeRegistry {
    WTF_MAKE_NONCOPYABLE(WasmGCTypeRegistry);
    WTF_MAKE_TZONE_ALLOCATED(WasmGCTypeRegistry);

    WasmGCTypeRegistry();

public:
    JS_EXPORT_PRIVATE static WasmGCTypeRegistry& singleton();

    // Return a canonical type matching the given type, or nullptr.
    // Caller must be holding m_lock.
    JS_EXPORT_PRIVATE WasmGCType* findType(const WasmGCType*);

    // Add a new root set. Caller must be holding m_lock.
    // All types in the root set must already be canonical (i.e., deduplicated
    // against the registry via findType before being added to the root set).
    JS_EXPORT_PRIVATE void registerRootSet(WasmGCTypeRootSet*);
    JS_EXPORT_PRIVATE void deregisterRootSet(WasmGCTypeRootSet*);

    Lock& lock() { return m_lock; }

    JS_EXPORT_PRIVATE void registerCanonicalRTTForType(WasmGCType*);
    JS_EXPORT_PRIVATE Ref<const RTT> getCanonicalRTT(const WasmGCType*);

private:
    void collect() WTF_REQUIRES_LOCK(m_lock);
    static void markType(WasmGCType*);
    static void markTypeFromIndex(TypeIndex);
    static void markStorageType(const StorageType&);
    static Ref<RTT> createCanonicalRTTForType(const WasmGCType*);

    Lock m_lock;
    UncheckedKeyHashSet<WasmGCTypeHash> m_typeSet WTF_GUARDED_BY_LOCK(m_lock);
    HashSet<WasmGCTypeRootSet*> m_rootSets WTF_GUARDED_BY_LOCK(m_lock);
};

} } // namespace JSC::Wasm

#endif // ENABLE(WEBASSEMBLY)
