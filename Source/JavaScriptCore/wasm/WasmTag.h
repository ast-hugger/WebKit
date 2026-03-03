/*
 * Copyright (C) 2021-2023 Apple Inc. All rights reserved.
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

#if ENABLE(WEBASSEMBLY)

#include "WasmGCType.h"
#include <wtf/TZoneMalloc.h>

namespace JSC { namespace Wasm {

class Tag final : public ThreadSafeRefCounted<Tag> {
    WTF_MAKE_TZONE_ALLOCATED(Tag);
    WTF_MAKE_NONCOPYABLE(Tag);
public:
    // Use for types owned by the registry (module-declared tags).
    static Ref<Tag> create(const WasmGCFunctionType* type) { return adoptRef(*new Tag(type, false)); }
    // Use for synthetic types not in the registry (JS-created tags).
    static Ref<Tag> createOwning(const WasmGCFunctionType* type) { return adoptRef(*new Tag(type, true)); }

    ~Tag()
    {
        if (m_ownsType)
            const_cast<WasmGCFunctionType*>(m_type)->destroy();
    }

    FunctionArgCount parameterCount() const { return m_type->argumentCount(); }

    size_t parameterBufferSize() const
    {
        size_t result = 0;
        for (size_t i = 0; i < parameterCount(); i ++)
            result += m_type->argumentType(i).kind == TypeKind::V128 ? 2 : 1;
        return result;
    }

    Type parameter(FunctionArgCount i) const { return m_type->argumentType(i); }
    TypeIndex typeIndex() const { return m_type->index(); }

    // Since (1) we do not copy Wasm::Tag and (2) we always allocate Wasm::Tag from heap, we can use
    // pointer comparison for identity check.
    bool operator==(const Tag& other) const { return this == &other; }

    const WasmGCFunctionType& type() const { return *m_type; }

    static Tag& jsExceptionTag();

private:
    Tag(const WasmGCFunctionType* type, bool ownsType)
        : m_type(type)
        , m_ownsType(ownsType)
    {
    }

    const WasmGCFunctionType* m_type;
    bool m_ownsType;
};

} } // namespace JSC::Wasm

#endif // ENABLE(WEBASSEMBLY)
