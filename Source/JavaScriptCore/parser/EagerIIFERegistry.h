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

#pragma once

#include "Nodes.h"
#include "ParserModes.h"
#include "Strong.h"
#include <wtf/InlineMap.h>
#include <wtf/OptionSet.h>
#include <wtf/RefCounted.h>

namespace JSC {

class ProgramNode;
class UnlinkedFunctionExecutable;

// Two-phase per-SourceProvider cache for likely IIFEs.
//
// Phase 1 (during initial top-level parse): eagerly-built FunctionNodes land in
// m_pendingNodes keyed by source start offset. The associated FunctionMetadataNode
// (also referenced by the enclosing program's FuncExprNode) is kept alongside so
// the end-of-parse pass can create the UnlinkedFunctionExecutable.
//
// Phase 2 (end of top-level parse): each pending FunctionNode is byte-compiled
// into an UnlinkedFunctionCodeBlock installed on a freshly-created
// UnlinkedFunctionExecutable. The executable is stored in m_executables; the
// FunctionNode (and its ParserArena) is freed.
//
// During the enclosing program's bytecode pass, BytecodeGenerator::makeFunction
// consults m_executables by source position and reuses the cached executable
// instead of creating a new one.
class EagerIIFERegistry : public RefCounted<EagerIIFERegistry> {
public:
    struct PendingEntry {
        std::unique_ptr<FunctionNode> node;
        FunctionMetadataNode* metadata { nullptr };
    };

    EagerIIFERegistry() { }
    ~EagerIIFERegistry();

    void clear();

    // Phase 1: called by the parser when an IIFE has been eagerly parsed.
    void addPendingNode(int sourcePosition, std::unique_ptr<FunctionNode>, FunctionMetadataNode*);

    // Phase 2 helpers: iterate and consume pending nodes.
    bool hasPendingNodes() const { return !m_pendingNodes.isEmpty(); }
    auto takePendingNodes() -> InlineMap<int, PendingEntry, 4, WTF::IntHash<int>, WTF::UnsignedWithZeroKeyHashTraits<int>>
    {
        auto result = WTF::move(m_pendingNodes);
        m_pendingNodes.clear();
        return result;
    }

    // Fallback consumer used when eager-compile is disabled: the lazy
    // re-parse path short-circuits by taking the cached FunctionNode.
    std::unique_ptr<FunctionNode> takePendingNode(int sourcePosition)
    {
        auto entry = m_pendingNodes.take(sourcePosition);
        return WTF::move(entry.node);
    }

    // Phase 2: install the bytecode-compiled executable keyed by source position.
    void addExecutable(VM&, int sourcePosition, UnlinkedFunctionExecutable*);

    // Consumed by BytecodeGenerator::makeFunction.
    UnlinkedFunctionExecutable* takeExecutable(int sourcePosition);

private:
    InlineMap<int, PendingEntry, 4, WTF::IntHash<int>, WTF::UnsignedWithZeroKeyHashTraits<int>> m_pendingNodes;
    InlineMap<int, Strong<UnlinkedFunctionExecutable>, 4, WTF::IntHash<int>, WTF::UnsignedWithZeroKeyHashTraits<int>> m_executables;
};

// End-of-parse pass: bytecode-compile every pending IIFE and install the result
// on a newly-created UnlinkedFunctionExecutable stored in the registry.
// Called from the top-level program parse just before returning the ProgramNode.
// Frees AST + ParserArena for each IIFE as soon as its bytecode is generated.
// On compile failure for an individual entry, drops it; the lazy path will re-parse.
void performEagerIIFECompileForProgram(VM&, EagerIIFERegistry&, ProgramNode&, const SourceCode&, OptionSet<CodeGenerationMode>, JSParserScriptMode);

} // namespace JSC
