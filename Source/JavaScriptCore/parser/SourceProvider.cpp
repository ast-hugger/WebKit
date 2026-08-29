/*
 * Copyright (C) 2013-2023 Apple Inc. All rights reserved.
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
#include "SourceProvider.h"

#include <wtf/FileHandle.h>
#include <wtf/FileSystem.h>
#include <wtf/ProcessID.h>
#include <wtf/text/MakeString.h>

namespace JSC {

DEFINE_ALLOCATOR_WITH_HEAP_IDENTIFIER(StringSourceProvider);

SourceProvider::SourceProvider(const SourceOrigin& sourceOrigin, String&& sourceURL, String&& preRedirectURL, SourceTaintedOrigin taintedness, const TextPosition& startPosition, SourceProviderSourceType sourceType)
    : m_sourceType(sourceType)
    , m_sourceOrigin(sourceOrigin)
    , m_sourceURL(WTF::move(sourceURL))
    , m_preRedirectURL(WTF::move(preRedirectURL))
    , m_startPosition(startPosition)
    , m_taintedness(taintedness)
{
}

SourceProvider::~SourceProvider() = default;

void SourceProvider::lockUnderlyingBuffer()
{
    if (!m_lockingCount++)
        lockUnderlyingBufferImpl();
}

void SourceProvider::unlockUnderlyingBuffer()
{
    if (!--m_lockingCount)
        unlockUnderlyingBufferImpl();
}

CodeBlockHash SourceProvider::codeBlockHashConcurrently(int startOffset, int endOffset, CodeSpecializationKind kind)
{
    auto entireSourceCode = source();
    return CodeBlockHash { entireSourceCode.substring(startOffset, endOffset - startOffset), entireSourceCode, kind };
}

void SourceProvider::lockUnderlyingBufferImpl() { }

void SourceProvider::unlockUnderlyingBufferImpl() { }

void SourceProvider::getID()
{
    if (!m_id) {
        static std::atomic<SourceID> nextProviderID = nullID;
        m_id = ++nextProviderID;
        RELEASE_ASSERT(m_id);
    }
}

const String& SourceProvider::sourceURLStripped()
{
    if (m_sourceURL.isNull()) [[unlikely]]
        return m_sourceURLStripped;
    if (!m_sourceURLStripped.isNull()) [[likely]]
        return m_sourceURLStripped;
    m_sourceURLStripped = URL(m_sourceURL).strippedForUseAsReport();
    return m_sourceURLStripped;
}

CString SourceProvider::sourceCodeDumpFilePath(const CString& dumpDirectory)
{
    if (m_sourceCodeDumped.load(std::memory_order_acquire)) {
        Locker locker { m_sourceCodeDumpLock };
        return m_sourceCodeDumpFilePath;
    }

    Locker locker { m_sourceCodeDumpLock };
    if (m_sourceCodeDumped.load(std::memory_order_relaxed))
        return m_sourceCodeDumpFilePath;

    auto tryExtractLocalPath = [](const String& urlString) -> String {
        if (urlString.isNull())
            return { };
        if (urlString.startsWith('/'))
            return urlString;
        if (urlString.startsWith("file://"_s))
            return URL(urlString).fileSystemPath();
        return { };
    };

    String localPath = tryExtractLocalPath(sourceURL());

    if (!localPath.isNull())
        m_sourceCodeDumpFilePath = FileSystem::fileSystemRepresentation(localPath);
    else {
        auto baseName = makeString("source-"_s, asID(), '-', WTF::getCurrentProcessID());
        String filePath;
        FileSystem::FileHandle handle;
        if (dumpDirectory.isNull()) {
            auto result = FileSystem::openTemporaryFile(baseName, ".js"_s);
            filePath = result.first;
            handle = WTF::move(result.second);
        } else {
            filePath = makeString(String::fromUTF8(dumpDirectory.span()), FileSystem::pathSeparator, baseName, ".js"_s);
            handle = FileSystem::openFile(filePath, FileSystem::FileOpenMode::Truncate);
        }
        if (handle) {
            auto sourceText = source().utf8();
            handle.write(WTF::asByteSpan(sourceText.span()));
            handle.flush();
            m_sourceCodeDumpFilePath = FileSystem::fileSystemRepresentation(filePath);
        }
    }

    m_sourceCodeDumped.store(true, std::memory_order_release);
    return m_sourceCodeDumpFilePath;
}

#if ENABLE(WEBASSEMBLY)
BaseWebAssemblySourceProvider::BaseWebAssemblySourceProvider(const SourceOrigin& sourceOrigin, String&& sourceURL)
    : SourceProvider(sourceOrigin, WTF::move(sourceURL), String(), SourceTaintedOrigin::Untainted, TextPosition(), SourceProviderSourceType::WebAssembly)
{
}
#endif

template<typename CharType>
Vector<unsigned> LineStartTable::build(std::span<const CharType> text)
{
    Vector<unsigned> lineStarts;
    // Element 0 is the start of the first line, so the table is never empty and a line number is an
    // index into it.
    lineStarts.append(0);

    // findLineTerminator is find-first rather than find-all, which suits this: terminators are
    // sparse -- roughly one per line against one token per few bytes -- so each call consumes a
    // whole line of source. Nothing here needs a retained per-byte bitmap, which is the awkward
    // thing to obtain on ARM.
    const CharType* const begin = text.data();
    const CharType* const end = std::to_address(text.end());
    size_t index = 0;
    while (index < text.size()) {
        const CharType* found = findLineTerminator(text.subspan(index));
        if (found == end)
            break;
        // Recorded even when the next line starts at the end of the text: a source ending in a
        // terminator has a final empty line, because that is where the lexer's cursor ends up, and
        // agreeing with the lexer is this table's whole contract.
        size_t next = lineStartAfterTerminator(text, static_cast<size_t>(found - begin));
        lineStarts.append(static_cast<unsigned>(next));
        // Resuming past the whole terminator is what keeps CRLF a single line break: starting again
        // at the LF would count it a second time.
        index = next;
    }

    lineStarts.shrinkToFit();
    return lineStarts;
}

const Vector<unsigned>& LineStartTable::ensureBuilt(StringView text)
{
    if (!m_lineStarts) {
        m_lineStarts = text.is8Bit() ? build(text.span8()) : build(text.span16());
        m_builtForLength = text.length();
    }
    // Built once from one source; handing it a different one would silently return positions for the
    // wrong text.
    ASSERT(m_builtForLength == text.length());
    return *m_lineStarts;
}

auto LineStartTable::positionInfoForOffset(StringView text, unsigned offset) -> PositionInfo
{
    Locker locker { m_lock };
    const Vector<unsigned>& lineStarts = ensureBuilt(text);

    // The greatest line whose start is at or before `offset`. An offset past the end of the text is
    // clamped to the last line rather than refused: callers reach here from error reporting, where
    // answering approximately beats not answering.
    size_t line = lineStarts.size() - 1;
    if (offset < lineStarts.last()) {
        // upper_bound gives the first line starting strictly after the offset.
        auto it = std::upper_bound(lineStarts.begin(), lineStarts.end(), offset);
        ASSERT(it != lineStarts.begin());
        line = static_cast<size_t>(it - lineStarts.begin()) - 1;
    }

    unsigned lineStart = lineStarts[line];
    unsigned length = text.length();
    unsigned lineEnd = (line + 1 < lineStarts.size()) ? lineStarts[line + 1] : length;
    // A non-final line's end came from the next line's start, which is past the terminator, so step
    // back over it -- two characters for CRLF, one otherwise.
    if (lineEnd < length) {
        if (lineEnd >= 2 && isCRLFPair(text[lineEnd - 2], text[lineEnd - 1]))
            lineEnd -= 2;
        else
            lineEnd -= 1;
    }

    return {
        static_cast<unsigned>(line),
        offset > lineStart ? offset - lineStart : 0,
        lineStart,
        lineEnd,
    };
}

unsigned LineStartTable::lineCount(StringView text)
{
    Locker locker { m_lock };
    return ensureBuilt(text).size();
}

} // namespace JSC

