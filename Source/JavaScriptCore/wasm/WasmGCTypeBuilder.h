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
#include <JavaScriptCore/WasmFormat.h>
#include <JavaScriptCore/WasmGCType.h>
#include <JavaScriptCore/WasmGCTypeRegistry.h>
#include <JavaScriptCore/WasmLimits.h>
#include <span>
#include <wtf/Vector.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace Wasm {

// Helper for constructing WasmGCType objects during type section parsing.
//
// When parsing a recursion group, type members may reference other members
// that haven't been parsed yet (forward references). These use small positive
// integers as placeholder TypeIndex values. Placeholders are safe because
// builtin TypeIndex values are negative (cast from TypeKind enum), and heap
// pointers from fastMalloc are always well above maxRecursionGroupCount on
// 64-bit systems. For backward references and cross-group references, the
// actual WasmGCType* pointer (via index()) is used directly.
//
// Per-group processing workflow:
//   1. Parse each member, creating tentative WasmGCType objects.
//   2. patchPlaceholders() replaces placeholder indices with actual pointers.
//   3. Caller sets supertype and finality on each type.
//   4. deduplicateAndRegister() atomically (under the registry lock) checks
//      each type against the registry, replaces matches with canonical
//      pointers, patches surviving types' references, and registers the root
//      set. This ensures no window exists where a looked-up canonical pointer
//      is held outside the registry's protection, and no window where a novel
//      type is not yet visible to other threads which may add a duplicate.
//
class WasmGCTypeBuilder {
    WTF_MAKE_NONCOPYABLE(WasmGCTypeBuilder);
public:
    static constexpr TypeIndex placeholderForGroupIndex(uint32_t k) { return k + 1; }
    static constexpr bool isPlaceholder(TypeIndex idx) { return idx > 0 && idx <= maxRecursionGroupCount; }
    static constexpr uint32_t placeholderToGroupIndex(TypeIndex idx) { return static_cast<uint32_t>(idx) - 1; }

    JS_EXPORT_PRIVATE static void patchPlaceholders(std::span<WasmGCType* const> groupTypes);

    // Iterate all concrete TypeIndex references in a type and call a callback that may
    // modify each index.
    template<typename Func>
        requires std::invocable<Func, TypeIndex&>
    static void walkTypeReferences(WasmGCType*, Func&&);

    JS_EXPORT_PRIVATE static void deduplicateAndRegister(
        std::span<WasmGCType*> groupTypes,
        Vector<WasmGCType*>& gcTypeSignatures,
        WasmGCTypeRootSet& rootSet);
};

template<typename Func>
    requires std::invocable<Func, TypeIndex&>
void WasmGCTypeBuilder::walkTypeReferences(WasmGCType* type, Func&& callback)
{
    switch (type->typeKind()) {
    case WasmGCTypeKind::FunctionType: {
        auto* func = type->as<WasmGCFunctionType>();
        for (FunctionArgCount i = 0; i < func->returnCount(); ++i) {
            Type& t = func->getReturnType(i);
            if (isRefType(t) && !typeIndexIsType(t.index))
                callback(t.index);
        }
        for (FunctionArgCount i = 0; i < func->argumentCount(); ++i) {
            Type& t = func->getArgumentType(i);
            if (isRefType(t) && !typeIndexIsType(t.index))
                callback(t.index);
        }
        break;
    }
    case WasmGCTypeKind::StructType: {
        auto* structType = type->as<WasmGCStructType>();
        for (StructFieldCount i = 0; i < structType->fieldCount(); ++i) {
            FieldType& fieldType = structType->mutableFields()[i];
            if (fieldType.type.is<Type>()) {
                Type t = fieldType.type.as<Type>();
                if (isRefType(t) && !typeIndexIsType(t.index)) {
                    callback(t.index);
                    fieldType.type = StorageType(t);
                }
            }
        }
        break;
    }
    case WasmGCTypeKind::ArrayType: {
        auto* arrayType = type->as<WasmGCArrayType>();
        if (arrayType->m_elementType.type.is<Type>()) {
            Type t = arrayType->m_elementType.type.as<Type>();
            if (isRefType(t) && !typeIndexIsType(t.index)) {
                callback(t.index);
                arrayType->m_elementType.type = StorageType(t);
            }
        }
        break;
    }
    }
}

} } // namespace JSC::Wasm

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(WEBASSEMBLY)
