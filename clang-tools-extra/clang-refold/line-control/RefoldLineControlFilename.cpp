//===--- RefoldLineControlFilename.cpp ------------------------*- C++ -*-===//
//
// Shared implementation for the conservative #line filename string-literal
// parser used by include replay and source-line rewrite proof.
//
//===----------------------------------------------------------------------===//

#include "line-control/RefoldLineControlFilename.h"

namespace clang {
namespace refold {

using llvm::StringRef;

bool isLineControlHexDigit(char c) {
  return ('0' <= c && c <= '9') || ('a' <= c && c <= 'f') ||
         ('A' <= c && c <= 'F');
}

unsigned lineControlHexValue(char c) {
  if ('0' <= c && c <= '9')
    return static_cast<unsigned>(c - '0');
  if ('a' <= c && c <= 'f')
    return static_cast<unsigned>(c - 'a' + 10);
  return static_cast<unsigned>(c - 'A' + 10);
}

std::optional<unsigned>
decodeBoundedLineControlHexEscapeValue(StringRef &rest) {
  if (rest.empty() || !isLineControlHexDigit(rest.front()))
    return std::nullopt;

  unsigned value = 0;
  do {
    const unsigned digit = lineControlHexValue(rest.front());
    if (value > (0xffu - digit) / 16u)
      return std::nullopt;
    value = (value * 16u) + digit;
    rest = rest.drop_front();
  } while (!rest.empty() && isLineControlHexDigit(rest.front()));

  return value;
}

std::optional<uint32_t>
decodeLineControlUniversalCharacterNameValue(StringRef &rest,
                                             unsigned digits) {
  if (rest.size() < digits)
    return std::nullopt;

  uint32_t value = 0;
  for (unsigned i = 0; i < digits; ++i) {
    const char ch = rest[i];
    if (!isLineControlHexDigit(ch))
      return std::nullopt;
    value = (value << 4) | lineControlHexValue(ch);
  }

  rest = rest.drop_front(digits);
  return value;
}

bool isReplayableLineControlUniversalCharacterName(uint32_t value) {
  return 0x00a0u <= value && value <= 0x10ffffu &&
         !(0xd800u <= value && value <= 0xdfffu);
}

bool appendLineControlUTF8(std::string &out, uint32_t value) {
  if (!isReplayableLineControlUniversalCharacterName(value))
    return false;

  if (value <= 0x7fu) {
    out.push_back(static_cast<char>(value));
    return true;
  }
  if (value <= 0x7ffu) {
    out.push_back(static_cast<char>(0xc0u | (value >> 6)));
    out.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
    return true;
  }
  if (value <= 0xffffu) {
    out.push_back(static_cast<char>(0xe0u | (value >> 12)));
    out.push_back(static_cast<char>(0x80u | ((value >> 6) & 0x3fu)));
    out.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
    return true;
  }

  out.push_back(static_cast<char>(0xf0u | (value >> 18)));
  out.push_back(static_cast<char>(0x80u | ((value >> 12) & 0x3fu)));
  out.push_back(static_cast<char>(0x80u | ((value >> 6) & 0x3fu)));
  out.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
  return true;
}

bool isReplayableLineControlNumericFilenameByte(unsigned value) {
  if (0x20 <= value && value <= 0x7e)
    return true;

  if (0x80 <= value && value <= 0xff)
    return true;

  switch (value) {
  case '\a':
  case '\b':
  case '\f':
  case '\n':
  case '\r':
  case '\t':
  case '\v':
  case 0x1b:
    return true;
  default:
    return false;
  }
}

bool lineControlFilenameByteRequiresUTF8Validation(unsigned value) {
  return 0x80 <= value && value <= 0xff;
}

bool isValidLineControlUTF8(StringRef text) {
  for (size_t i = 0; i < text.size();) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    if (lead < 0x80) {
      ++i;
      continue;
    }

    auto continuation = [&](size_t index) -> std::optional<unsigned char> {
      if (index >= text.size())
        return std::nullopt;
      const unsigned char byte = static_cast<unsigned char>(text[index]);
      if ((byte & 0xc0u) != 0x80u)
        return std::nullopt;
      return byte;
    };

    if (0xc2 <= lead && lead <= 0xdf) {
      if (!continuation(i + 1))
        return false;
      i += 2;
      continue;
    }

    if (lead == 0xe0) {
      std::optional<unsigned char> b1 = continuation(i + 1);
      if (!b1 || *b1 < 0xa0 || !continuation(i + 2))
        return false;
      i += 3;
      continue;
    }

    if ((0xe1 <= lead && lead <= 0xec) || (0xee <= lead && lead <= 0xef)) {
      if (!continuation(i + 1) || !continuation(i + 2))
        return false;
      i += 3;
      continue;
    }

    if (lead == 0xed) {
      std::optional<unsigned char> b1 = continuation(i + 1);
      if (!b1 || *b1 >= 0xa0 || !continuation(i + 2))
        return false;
      i += 3;
      continue;
    }

    if (lead == 0xf0) {
      std::optional<unsigned char> b1 = continuation(i + 1);
      if (!b1 || *b1 < 0x90 || !continuation(i + 2) ||
          !continuation(i + 3))
        return false;
      i += 4;
      continue;
    }

    if (0xf1 <= lead && lead <= 0xf3) {
      if (!continuation(i + 1) || !continuation(i + 2) ||
          !continuation(i + 3))
        return false;
      i += 4;
      continue;
    }

    if (lead == 0xf4) {
      std::optional<unsigned char> b1 = continuation(i + 1);
      if (!b1 || *b1 > 0x8f || !continuation(i + 2) ||
          !continuation(i + 3))
        return false;
      i += 4;
      continue;
    }

    return false;
  }

  return true;
}

std::optional<DecodedLineControlFilenameEscape>
decodeLineControlFilenameEscape(StringRef &rest) {
  if (rest.empty())
    return std::nullopt;

  const char escaped = rest.front();
  if (escaped == '\n' || escaped == '\r')
    return std::nullopt;

  if ('0' <= escaped && escaped <= '7') {
    unsigned value = 0;
    unsigned digits = 0;
    while (!rest.empty() && digits < 3 && '0' <= rest.front() &&
           rest.front() <= '7') {
      value = (value * 8) + static_cast<unsigned>(rest.front() - '0');
      rest = rest.drop_front();
      ++digits;
    }
    if (!isReplayableLineControlNumericFilenameByte(value))
      return std::nullopt;
    return DecodedLineControlFilenameEscape{
        std::string(1, static_cast<char>(value)),
        lineControlFilenameByteRequiresUTF8Validation(value)};
  }

  if (escaped == 'x') {
    rest = rest.drop_front();
    std::optional<unsigned> value =
        decodeBoundedLineControlHexEscapeValue(rest);
    if (!value || !isReplayableLineControlNumericFilenameByte(*value))
      return std::nullopt;
    return DecodedLineControlFilenameEscape{
        std::string(1, static_cast<char>(*value)),
        lineControlFilenameByteRequiresUTF8Validation(*value)};
  }

  if (escaped == 'u' || escaped == 'U') {
    rest = rest.drop_front();
    std::optional<uint32_t> value =
        decodeLineControlUniversalCharacterNameValue(
            rest, escaped == 'u' ? 4u : 8u);
    if (!value)
      return std::nullopt;

    std::string utf8;
    if (!appendLineControlUTF8(utf8, *value))
      return std::nullopt;
    return DecodedLineControlFilenameEscape{utf8, false};
  }

  switch (escaped) {
  case '"':
  case '?':
  case '\\':
    rest = rest.drop_front();
    return DecodedLineControlFilenameEscape{std::string(1, escaped), false};
  case 'a':
    rest = rest.drop_front();
    return DecodedLineControlFilenameEscape{std::string(1, '\a'), false};
  case 'b':
    rest = rest.drop_front();
    return DecodedLineControlFilenameEscape{std::string(1, '\b'), false};
  case 'e':
  case 'E':
    // Clang accepts \e/\E as an extension and records the same escape byte
    // that it later prints for __FILE__.  Model it explicitly instead of
    // treating it as a warningful unknown escape.
    rest = rest.drop_front();
    return DecodedLineControlFilenameEscape{
        std::string(1, static_cast<char>(0x1b)), false};
  case 'f':
    rest = rest.drop_front();
    return DecodedLineControlFilenameEscape{std::string(1, '\f'), false};
  case 'n':
    rest = rest.drop_front();
    return DecodedLineControlFilenameEscape{std::string(1, '\n'), false};
  case 'r':
    rest = rest.drop_front();
    return DecodedLineControlFilenameEscape{std::string(1, '\r'), false};
  case 't':
    rest = rest.drop_front();
    return DecodedLineControlFilenameEscape{std::string(1, '\t'), false};
  case 'v':
    rest = rest.drop_front();
    return DecodedLineControlFilenameEscape{std::string(1, '\v'), false};
  default:
    // Clang's warningful ordinary unknown escapes in #line filenames delete the
    // backslash and preserve the escaped byte.  This matches the prior bounded
    // raw-backslash proof and is still reparsed by the strict directive parser.
    rest = rest.drop_front();
    return DecodedLineControlFilenameEscape{std::string(1, escaped), false};
  }
}

std::optional<std::string>
parseLineControlFilenameLiteral(StringRef &rest) {
  if (!rest.consume_front("\""))
    return std::nullopt;

  std::string fileSpelling;
  bool closedQuote = false;
  bool requiresUTF8Validation = false;
  while (!rest.empty()) {
    char ch = rest.front();
    rest = rest.drop_front();
    if (ch == '\"') {
      closedQuote = true;
      break;
    }
    if (ch == '\n' || ch == '\r')
      return std::nullopt;
    if (ch == '\\') {
      std::optional<DecodedLineControlFilenameEscape> decoded =
          decodeLineControlFilenameEscape(rest);
      if (!decoded)
        return std::nullopt;
      requiresUTF8Validation |= decoded->requiresUTF8Validation;
      fileSpelling += decoded->bytes;
      continue;
    }
    fileSpelling.push_back(ch);
  }

  if (!closedQuote)
    return std::nullopt;
  if (requiresUTF8Validation && !isValidLineControlUTF8(fileSpelling))
    return std::nullopt;
  return fileSpelling;
}

} // namespace refold
} // namespace clang
