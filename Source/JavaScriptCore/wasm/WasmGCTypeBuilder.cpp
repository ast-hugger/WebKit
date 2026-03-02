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
#include "WasmGCTypeBuilder.h"

#if ENABLE(WEBASSEMBLY)

#include <wtf/HashMap.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace Wasm {

void WasmGCTypeBuilder::patchPlaceholders(std::span<WasmGCType* const> groupTypes)
{
    for (auto* type : groupTypes) {
        walkTypeReferences(type, [&](TypeIndex& idx) {
            if (isPlaceholder(idx))
                idx = groupTypes[placeholderToGroupIndex(idx)]->index();
        });
    }
}

void WasmGCTypeBuilder::deduplicateAndRegister(
    std::span<WasmGCType*> groupTypes,
    Vector<WasmGCType*>& gcTypeSignatures,
    WasmGCTypeRootSet& rootSet)
{
    auto& registry = WasmGCTypeRegistry::singleton();

    // Build a replacement map: tentative pointer -> canonical pointer.
    HashMap<WasmGCType*, WasmGCType*> replacements;

    {
        Locker locker { registry.lock() };

        // Phase 1: Find matches in the registry.
        for (size_t i = 0; i < groupTypes.size(); ++i) {
            auto* tentative = groupTypes[i];
            auto* canonical = registry.findType(tentative);
            if (canonical) {
                replacements.add(tentative, canonical);
                groupTypes[i] = canonical;
            }
        }

        // Phase 2: Patch surviving (non-matched) types' references and supertypes.
        for (size_t i = 0; i < groupTypes.size(); ++i) {
            auto* type = groupTypes[i];
            // Only patch types that survived (are not themselves replaced).
            if (replacements.contains(type))
                continue;

            walkTypeReferences(type, [&](TypeIndex& idx) {
                auto* referenced = std::bit_cast<WasmGCType*>(idx);
                auto it = replacements.find(referenced);
                if (it != replacements.end())
                    idx = it->value->index();
            });

            // Patch supertype if it was replaced.
            if (type->supertype()) {
                auto it = replacements.find(type->supertype());
                if (it != replacements.end())
                    type->setSupertype(it->value);
            }
        }

        // Phase 3: Register the final types.
        for (auto* type : groupTypes)
            rootSet.append(type);
        registry.registerRootSet(&rootSet);
    }

    // Phase 4: Destroy replaced tentative types (outside the lock).
    for (auto& entry : replacements)
        entry.key->destroy();

    // Phase 5: Append final types to gcTypeSignatures.
    for (auto* type : groupTypes)
        gcTypeSignatures.append(type);
}

} } // namespace JSC::Wasm

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(WEBASSEMBLY)
