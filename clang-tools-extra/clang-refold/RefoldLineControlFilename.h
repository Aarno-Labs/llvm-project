//===--- RefoldLineControlFilename.h --------------------------*- C++ -*-===//
//
// Shared parser for source-spelled #line filename string literals.
//
// Include replay and source-line rewrite proofs both need the same conservative
// filename decoding rule: accept only the C string-literal escape spellings that
// Clang can replay deterministically as a presumed-file spelling, and fail
// closed for every unsupported or malformed escape.  Keeping that rule in one
// module prevents the include-replay proof and the line-control rewrite proof
// from drifting apart.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDLINECONTROLFILENAME_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDLINECONTROLFILENAME_H

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>

namespace clang {
namespace refold {

/// Return true iff `c` is one of the hexadecimal digits accepted by C escape
/// sequences.  The line-control filename parser is byte-oriented and does not
/// depend on any host locale state.
bool isLineControlHexDigit(char c);

/// Convert a hexadecimal digit already accepted by isLineControlHexDigit().
unsigned lineControlHexValue(char c);

/// Decode a greedy hexadecimal escape value and advance `rest` past every hex
/// digit consumed by the C string-literal grammar.  Values outside one byte are
/// rejected before arithmetic can overflow.
std::optional<unsigned>
decodeBoundedLineControlHexEscapeValue(llvm::StringRef &rest);

/// Decode exactly `digits` hexadecimal digits after a universal-character-name
/// introducer and advance `rest` past the digits on success.
std::optional<uint32_t>
decodeLineControlUniversalCharacterNameValue(llvm::StringRef &rest,
                                             unsigned digits);

/// Return true iff a decoded universal-character-name is within the stable
/// Unicode scalar range this filename proof can replay as UTF-8 bytes.
bool isReplayableLineControlUniversalCharacterName(uint32_t value);

/// Append the UTF-8 spelling Clang uses for an admitted universal-character-name
/// in a narrow #line filename string literal.
bool appendLineControlUTF8(std::string &out, uint32_t value);

/// Return true iff a decoded numeric filename byte can be replayed by this
/// proof's #line formatter without introducing an unmodeled filename state.
bool isReplayableLineControlNumericFilenameByte(unsigned value);

/// Return true iff a decoded filename byte is non-ASCII and therefore requires
/// the final filename spelling to be validated as UTF-8 before replay.
bool lineControlFilenameByteRequiresUTF8Validation(unsigned value);

/// Validate the structural UTF-8 subset that this proof may replay directly in
/// a #line filename.  This rejects overlong encodings, surrogate code points,
/// out-of-range scalars, and truncated continuation sequences.
bool isValidLineControlUTF8(llvm::StringRef text);

struct DecodedLineControlFilenameEscape {
  std::string bytes;
  bool requiresUTF8Validation = false;
};

/// Decode the bounded subset of C string-literal escapes that the source-only
/// line-control proof can replay exactly for a filename operand.
std::optional<DecodedLineControlFilenameEscape>
decodeLineControlFilenameEscape(llvm::StringRef &rest);

/// Parse and decode a quoted #line filename operand.
///
/// This is deliberately narrower than a general C string-literal parser.  It is
/// the inverse of the filename spellings the proof may synthesize and replay:
/// exact ordinary bytes, conservative quote/backslash escapes, warningful
/// ordinary unknown escapes, and numeric escapes that decode to replayable
/// filename bytes.  Unsupported escape forms fail closed.
std::optional<std::string>
parseLineControlFilenameLiteral(llvm::StringRef &rest);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDLINECONTROLFILENAME_H
