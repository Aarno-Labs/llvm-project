//===--- RefoldLineControlFilename.h --------------------------*- C++ -*-===//
//
// Shared parser for source-spelled #line filename string literals.
//
// Include replay and source-line rewrite proofs both need the same conservative
// filename decoding rule: accept only the C string-literal escape spellings
// that Clang can replay deterministically as a presumed-file spelling, and fail
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
/// sequences.  Keep this helper local to the line-control proof so the escape
/// decoder below is byte-oriented and independent from any host locale state.
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
///
/// C universal-character names for the basic source character set and control
/// characters are intentionally left outside this proof.  Admitting only scalar
/// values at or above U+00A0 matches the portable UCN domain used for ordinary
/// source text and avoids collisions with the existing exact tables for quotes,
/// backslashes, physical newlines, and named control escapes.
bool isReplayableLineControlUniversalCharacterName(uint32_t value);

/// Append the UTF-8 spelling Clang uses for an admitted
/// universal-character-name in a narrow #line filename string literal.
bool appendLineControlUTF8(std::string &out, uint32_t value);

/// Return true iff a decoded numeric filename byte can be replayed by this
/// proof's #line formatter without introducing an unmodeled filename state.
///
/// Numeric C escapes in a macro-stringified #line filename denote filename
/// bytes once the generated string literal is interpreted.  Single-byte ASCII
/// values are replayable when they are either ordinary printable bytes or one
/// of the bounded control bytes for which EscapeForLineDirective() has an
/// explicit one-line escape spelling.  High bytes are only provisionally
/// admitted here: parseLineControlFilenameLiteral() validates that the final
/// filename byte string is well-formed UTF-8 before accepting the directive.
/// That lets byte-oriented spellings such as `\303\251` round-trip as the
/// same filename state as the UCN spelling `\u00E9`, while malformed high-byte
/// sequences still fail closed.
bool isReplayableLineControlNumericFilenameByte(unsigned value);

/// Return true iff a decoded filename byte is non-ASCII and therefore requires
/// the final filename spelling to be validated as UTF-8 before replay.
bool lineControlFilenameByteRequiresUTF8Validation(unsigned value);

/// Validate the UTF-8 subset that this proof may replay directly in a #line
/// filename.
///
/// This is deliberately just a structural UTF-8 validator.  It rejects
/// overlong encodings, surrogate code points, out-of-range scalars, and
/// truncated continuation sequences.  The line-control proof calls it only when
/// a numeric escape contributed at least one high byte, preserving the previous
/// behavior for already source-spelled filename bytes while keeping escaped
/// high-byte sequences from smuggling malformed text into an emitted directive.
bool isValidLineControlUTF8(llvm::StringRef text);

struct DecodedLineControlFilenameEscape {
  std::string bytes;
  bool requiresUTF8Validation = false;
};

/// Decode the bounded subset of C string-literal escapes that this source-only
/// line-control proof can replay exactly for a filename operand.
///
/// The parser accepts:
/// * ordinary unknown escapes, whose observable spelling is the escaped byte;
/// * escaped quote, question mark, and backslash;
/// * bounded octal escapes that decode to replayable filename bytes;
/// * bounded hexadecimal escapes that decode to replayable filename bytes; and
/// * universal-character names that decode to replayable Unicode scalars.
///
/// Named control escapes (`\n`, `\t`, etc.) are decoded through an explicit
/// table and later replayed by LineDirectiveInserter::EscapeForLineDirective().
/// Universal-character names are emitted as UTF-8 bytes, which the #line
/// formatter already preserves byte-for-byte.  Empty hex escapes, out-of-range
/// numeric escapes, invalid UCN scalars, and physical line breaks fail closed.
std::optional<DecodedLineControlFilenameEscape>
decodeLineControlFilenameEscape(llvm::StringRef &rest);

/// Parse and decode a quoted #line filename operand.
///
/// This is deliberately narrower than a general C string-literal parser.  It is
/// the inverse of the filename spellings this proof may synthesize and replay:
/// exact ordinary bytes, conservative quote/backslash escapes, warningful
/// ordinary unknown escapes, and numeric escapes that decode to replayable
/// filename bytes.  Unsupported escape forms fail closed so the
/// owner-envelope closure can fall back instead of guessing the filename state
/// observed by later `__FILE__` uses.
std::optional<std::string>
parseLineControlFilenameLiteral(llvm::StringRef &rest);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDLINECONTROLFILENAME_H
