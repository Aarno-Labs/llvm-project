//===- JSONSchemaValidator.h - JSON Schema Validator ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// Given a JSON Value `V`, which is the value being validated, and JSON Object
// `Schema` representing the schema for which we will validate againsts. This
// class simply performs checks against the `Schema`, and returns the appropri-
// ate Error if we encounter an error condition, otherwise Error::success() is
// returned.
//
// NOTE: This is not exactly feature complete, so take care in using this class
//       since there may be checks in your JSON schema file that are not yet
//       supported.
//===----------------------------------------------------------------------===//
#include "llvm/Support/JSONSchemaValidator.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/raw_os_ostream.h"
#include <cmath>
#include <optional>
#include <string>

namespace llvm {
namespace json {

namespace {
bool isJsonNull(const Value &V) {
  return !V.getAsObject() && !V.getAsArray() && !V.getAsString() &&
         !V.getAsNumber() && !V.getAsBoolean();
}

std::string prettyPath(StringRef Path) {
  if (Path.empty())
    return "$"; // root
  if (Path.starts_with("["))
    return "$" + Path.str(); // root array: "$[0]"
  return "$." + Path.str();  // normal: "$.a.b[0]"
}

std::string elideForMsg(llvm::StringRef S, size_t MaxLen = 100) {
  if (S.size() <= MaxLen)
    return S.str();
  if (MaxLen < 13) // Let's not allow for <msg>... size to be < 10
    MaxLen = 13;
  std::string Out;
  Out.reserve(MaxLen);
  Out.append(S.begin(), S.begin() + (MaxLen - 3));
  Out += "...";
  return Out;
}

static llvm::Expected<size_t> utf8CodePointLength(llvm::StringRef S) {
  using namespace llvm;

  const UTF8 *P = reinterpret_cast<const UTF8 *>(S.begin());
  const UTF8 *End = reinterpret_cast<const UTF8 *>(S.end());

  // Validate UTF-8 (also catches overlongs, invalid continuations, etc.)
  const UTF8 *Tmp = P;
  if (!isLegalUTF8String(&Tmp, End))
    return createStringError(inconvertibleErrorCode(), "Invalid UTF-8 string");

  // Count code points: count bytes that are NOT UTF-8 continuation bytes (10xxxxxx).
  size_t Count = 0;
  for (unsigned char C : S)
    if ((C & 0xC0) != 0x80)
      ++Count;

  return Count;
}


static bool parseUIntStrict(llvm::StringRef S, unsigned &Out) {
  // Reject empty strings and any leading '+' / '-' to keep these parsers strict.
  if (S.empty())
    return false;
  if (S.front() == '+' || S.front() == '-')
    return false;
  return !S.getAsInteger(10, Out);
}

static bool isLeapYear(unsigned Y) {
  return (Y % 4 == 0 && Y % 100 != 0) || (Y % 400 == 0);
}

static unsigned daysInMonth(unsigned Y, unsigned M) {
  switch (M) {
  case 1:
  case 3:
  case 5:
  case 7:
  case 8:
  case 10:
  case 12:
    return 31;
  case 4:
  case 6:
  case 9:
  case 11:
    return 30;
  case 2:
    return isLeapYear(Y) ? 29 : 28;
  default:
    return 0;
  }
}

static bool isValidDate(unsigned Y, unsigned M, unsigned D) {
  if (M < 1 || M > 12)
    return false;
  unsigned Dim = daysInMonth(Y, M);
  return D >= 1 && D <= Dim;
}

static bool isValidTime(unsigned H, unsigned Min, unsigned Sec) {
  if (H > 23)
    return false;
  if (Min > 59)
    return false;
  if (Sec > 59)
    return false;
  return true;
}

static bool isUUID(llvm::StringRef S) {
  if (S.size() != 36)
    return false;
  auto isDashPos = [](size_t I) {
    return I == 8 || I == 13 || I == 18 || I == 23;
  };
  for (size_t I = 0; I < S.size(); ++I) {
    char C = S[I];
    if (isDashPos(I)) {
      if (C != '-')
        return false;
    } else {
      if (!llvm::isHexDigit(C))
        return false;
    }
  }
  return true;
}

static bool isEmail(llvm::StringRef S) {
  // Minimal practical check (many schemas rely on a looser interpretation than RFC 5322).
  if (S.empty())
    return false;
  if (S.find_first_of(" \t\r\n") != llvm::StringRef::npos)
    return false;

  size_t At = S.find('@');
  if (At == llvm::StringRef::npos || At == 0 || At + 1 >= S.size())
    return false;
  if (S.find('@', At + 1) != llvm::StringRef::npos)
    return false;

  llvm::StringRef Domain = S.drop_front(At + 1);
  if (!Domain.contains('.'))
    return false;
  if (Domain.front() == '.' || Domain.back() == '.')
    return false;
  if (Domain.contains(".."))
    return false;

  return true;
}

static bool isURI(llvm::StringRef S) {
  // "uri" in JSON Schema is an absolute URI (RFC 3986). We use a simple, scheme-based check.
  static llvm::Regex R("^[A-Za-z][A-Za-z0-9+.-]*:.*$");
  return R.match(S);
}

static bool isHostname(llvm::StringRef S) {
  // RFC 1123-ish: labels are 1-63 chars, [A-Za-z0-9-], no leading/trailing '-'.
  if (S.empty() || S.size() > 253)
    return false;

  llvm::SmallVector<llvm::StringRef, 16> Labels;
  S.split(Labels, '.');

  for (llvm::StringRef L : Labels) {
    if (L.empty() || L.size() > 63)
      return false;
    if (L.front() == '-' || L.back() == '-')
      return false;
    for (char C : L) {
      if (!(llvm::isAlnum(C) || C == '-'))
        return false;
    }
  }
  return true;
}

static bool isRFC3339Date(llvm::StringRef S) {
  static llvm::Regex R("^([0-9]{4})-([0-9]{2})-([0-9]{2})$");
  llvm::SmallVector<llvm::StringRef, 4> M;
  if (!R.match(S, &M))
    return false;

  unsigned Y = 0, Mo = 0, D = 0;
  if (!parseUIntStrict(M[1], Y) || !parseUIntStrict(M[2], Mo) ||
      !parseUIntStrict(M[3], D))
    return false;

  return isValidDate(Y, Mo, D);
}

static bool isRFC3339Time(llvm::StringRef S) {
  // full-time requires a timezone.
  static llvm::Regex R("^([0-9]{2}):([0-9]{2}):([0-9]{2})(\\.[0-9]+)?"
                       "(Z|[+-][0-9]{2}:[0-9]{2})$");
  llvm::SmallVector<llvm::StringRef, 6> M;
  if (!R.match(S, &M))
    return false;

  unsigned H = 0, Min = 0, Sec = 0;
  if (!parseUIntStrict(M[1], H) || !parseUIntStrict(M[2], Min) ||
      !parseUIntStrict(M[3], Sec))
    return false;
  if (!isValidTime(H, Min, Sec))
    return false;

  llvm::StringRef TZ = M[5];
  if (TZ != "Z") {
    // "+HH:MM" or "-HH:MM"
    if (TZ.size() != 6 || (TZ[0] != '+' && TZ[0] != '-') || TZ[3] != ':')
      return false;
    unsigned TH = 0, TM = 0;
    if (!parseUIntStrict(TZ.substr(1, 2), TH) ||
        !parseUIntStrict(TZ.substr(4, 2), TM))
      return false;
    if (TH > 23 || TM > 59)
      return false;
  }

  return true;
}

static bool isRFC3339DateTime(llvm::StringRef S) {
  static llvm::Regex R("^([0-9]{4})-([0-9]{2})-([0-9]{2})T"
                       "([0-9]{2}):([0-9]{2}):([0-9]{2})(\\.[0-9]+)?"
                       "(Z|[+-][0-9]{2}:[0-9]{2})$");
  llvm::SmallVector<llvm::StringRef, 9> M;
  if (!R.match(S, &M))
    return false;

  unsigned Y = 0, Mo = 0, D = 0;
  unsigned H = 0, Min = 0, Sec = 0;
  if (!parseUIntStrict(M[1], Y) || !parseUIntStrict(M[2], Mo) ||
      !parseUIntStrict(M[3], D) || !parseUIntStrict(M[4], H) ||
      !parseUIntStrict(M[5], Min) || !parseUIntStrict(M[6], Sec))
    return false;

  if (!isValidDate(Y, Mo, D) || !isValidTime(H, Min, Sec))
    return false;

  llvm::StringRef TZ = M[8];
  if (TZ != "Z") {
    if (TZ.size() != 6 || (TZ[0] != '+' && TZ[0] != '-') || TZ[3] != ':')
      return false;
    unsigned TH = 0, TM = 0;
    if (!parseUIntStrict(TZ.substr(1, 2), TH) ||
        !parseUIntStrict(TZ.substr(4, 2), TM))
      return false;
    if (TH > 23 || TM > 59)
      return false;
  }

  return true;
}

static llvm::Error validateStringFormat(llvm::StringRef S, llvm::StringRef Format,
                                       llvm::StringRef Path) {
  // Draft-2020-12: "format" is an annotation keyword, but many schemas rely on it
  // as an assertion. We validate a small, common set of formats and ignore unknown
  // ones for forward compatibility.
  bool Ok = true;

  if (Format == "uuid")
    Ok = isUUID(S);
  else if (Format == "email")
    Ok = isEmail(S);
  else if (Format == "uri")
    Ok = isURI(S);
  else if (Format == "hostname")
    Ok = isHostname(S);
  else if (Format == "date")
    Ok = isRFC3339Date(S);
  else if (Format == "time")
    Ok = isRFC3339Time(S);
  else if (Format == "date-time")
    Ok = isRFC3339DateTime(S);
  else
    return llvm::Error::success();

  if (Ok)
    return llvm::Error::success();

  auto Msg = llvm::formatv("String at {0}: value \"{1}\" does not match format \"{2}\"",
                           prettyPath(Path), elideForMsg(S), Format)
                 .str();
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Msg);
}



// Walks an in-memory schema tree to find the first object-valued node whose
// string field `Key` equals `Wanted`. This is used for same-document $id/$anchor
// resolution without any remote fetch.
static const Value *findFirstObjectWithStringField(const Value &V, StringRef Key,
                                                   StringRef Wanted) {
  if (const Object *O = V.getAsObject()) {
    if (auto S = O->getString(Key))
      if (*S == Wanted)
        return &V;

    for (auto &KV : *O)
      if (const Value *R =
              findFirstObjectWithStringField(KV.second, Key, Wanted))
        return R;

    return nullptr;
  }

  if (const Array *A = V.getAsArray()) {
    for (const Value &E : *A)
      if (const Value *R = findFirstObjectWithStringField(E, Key, Wanted))
        return R;
  }

  return nullptr;
}

static const Value *findFirstObjectWithStringFieldInRoot(const Object &Root,
                                                         StringRef Key,
                                                         StringRef Wanted) {
  for (auto &KV : Root)
    if (const Value *R = findFirstObjectWithStringField(KV.second, Key, Wanted))
      return R;
  return nullptr;
}

} // namespace

Error JSONSchemaValidator::validate(const Value &V) const {
  RefCache.clear();
  return validateValue(V, RootSchema, "");
}

Error JSONSchemaValidator::validateValue(const Value &V, const Object &Schema,
                                         std::string Path) const {
  auto validateSubschema = [&](const Value &Instance, const Value &SubSchema,
                               std::string SubPath) -> Error {
    // Draft 2020-12: a schema is either an object or a boolean.
    if (auto B = SubSchema.getAsBoolean()) {
      if (*B)
        return Error::success();
      return createStringError(
          inconvertibleErrorCode(),
          formatv("Schema 'false' rejects instance at {0}", prettyPath(SubPath))
              .str());
    }
    if (auto O = SubSchema.getAsObject())
      return validateValue(Instance, *O, std::move(SubPath));
    return createStringError(
        inconvertibleErrorCode(),
        formatv("Schema error: subschema must be an object or boolean at {0}",
                prettyPath(SubPath))
            .str());
  };

  if (auto RefStr = getStringField(Schema, "$ref")) {
    // $ref in draft 2020-12 is an applicator; sibling keywords are still
    // evaluated. So we validate the referenced schema first, then continue.
    do {
    // Same-document $ref only: JSON Pointer (#/...) and anchors (#<name>), plus
    // $id-based references to subschemas in this same document. No remote fetch.
    auto ItCached = RefCache.find(*RefStr);
    if (ItCached != RefCache.end()) {
      if (auto Err = validateValue(V, *ItCached->second, Path))
        return Err;
      break;
    }

    auto decodePointerToken = [](StringRef Tok) -> std::string {
      std::string Out;
      Out.reserve(Tok.size());
      for (size_t I = 0; I < Tok.size(); ++I) {
        if (Tok[I] == '~' && I + 1 < Tok.size()) {
          if (Tok[I + 1] == '0') {
            Out.push_back('~');
            ++I;
            continue;
          }
          if (Tok[I + 1] == '1') {
            Out.push_back('/');
            ++I;
            continue;
          }
        }
        Out.push_back(Tok[I]);
      }
      return Out;
    };

    auto resolvePointer = [&](const Object &StartObj, StringRef PtrNoLeadingSlash,
                              const Value *&OutVal) -> llvm::Error {
      OutVal = nullptr;
      SmallVector<StringRef, 16> Parts;
      PtrNoLeadingSlash.split(Parts, '/', -1, false);

      const Object *CurObj = &StartObj;
      const Value *CurVal = nullptr;

      for (size_t I = 0; I < Parts.size(); ++I) {
        std::string Decoded = decodePointerToken(Parts[I]);
        auto It = CurObj->find(Decoded);
        if (It == CurObj->end()) {
          auto Msg = llvm::formatv("Invalid $ref pointer {0}: token \"{1}\" not found",
                                   elideForMsg(*RefStr), Decoded)
                         .str();
          return llvm::createStringError(llvm::inconvertibleErrorCode(), Msg);
        }

        CurVal = &It->second;
        if (const Object *NextObj = It->second.getAsObject()) {
          CurObj = NextObj;
          continue;
        }

        // Non-object target is only allowed at the terminal segment.
        if (I + 1 != Parts.size()) {
          auto Msg = llvm::formatv("Invalid $ref pointer {0}: non-object at \"{1}\"",
                                   elideForMsg(*RefStr), Decoded)
                         .str();
          return llvm::createStringError(llvm::inconvertibleErrorCode(), Msg);
        }
      }

      OutVal = CurVal;
      return llvm::Error::success();
    };

    auto validateTarget = [&](bool IsRoot, const Value *SchemaVal) -> llvm::Error {
      if (IsRoot) {
        RefCache[*RefStr] = &RootSchema;
        if (auto Err = validateValue(V, RootSchema, Path))
          return Err;
        return llvm::Error::success();
      }

      if (!SchemaVal) {
        auto Msg = llvm::formatv("Unsupported $ref {0}: not found", elideForMsg(*RefStr)).str();
        return llvm::createStringError(llvm::inconvertibleErrorCode(), Msg);
      }

      if (const Object *O = SchemaVal->getAsObject())
        RefCache[*RefStr] = O;

      if (auto Err = validateSubschema(V, *SchemaVal, Path))
        return Err;
      return llvm::Error::success();
    };

    StringRef Ref = *RefStr;

    // 1) Fragment-only forms: "#", "#/...", "#<anchor>".
    if (Ref.starts_with("#")) {
      StringRef Frag = Ref.drop_front(); // after '#'
      if (Frag.empty()) {
        if (auto Err = validateTarget(/*IsRoot*/ true, nullptr))
          return Err;
        break;
      }

      if (Frag.starts_with("/")) {
        // Preserve prior behavior: treat "#/" as the root schema.
        StringRef Ptr = Frag.drop_front(); // remove leading '/'
        if (Ptr.empty()) {
          if (auto Err = validateTarget(/*IsRoot*/ true, nullptr))
            return Err;
          break;
        }

        const Value *Val = nullptr;
        if (auto Err = resolvePointer(RootSchema, Ptr, Val))
          return Err;

        // Pointer can target object or boolean schema.
        if (auto Err = validateTarget(/*IsRoot*/ false, Val))
          return Err;
        break;
      }

      // "#<anchor>".
      if (auto A = RootSchema.getString("$anchor")) {
        if (*A == Frag) {
          if (auto Err = validateTarget(/*IsRoot*/ true, nullptr))
            return Err;
          break;
        }
      }

      const Value *Found =
          findFirstObjectWithStringFieldInRoot(RootSchema, "$anchor", Frag);
      if (auto Err = validateTarget(/*IsRoot*/ false, Found))
        return Err;
      break;
    }

    // 2) $id-based forms: "<id>", "<id>#/...", "<id>#<anchor>".
    size_t Hash = Ref.find('#');
    StringRef Base = (Hash == StringRef::npos) ? Ref : Ref.take_front(Hash);
    StringRef Frag = (Hash == StringRef::npos) ? StringRef() : Ref.drop_front(Hash + 1);

    const Object *BaseObj = nullptr;
    const Value *BaseNode = nullptr;

    if (auto I = RootSchema.getString("$id"))
      if (*I == Base)
        BaseObj = &RootSchema;

    if (!BaseObj)
      BaseNode = findFirstObjectWithStringFieldInRoot(RootSchema, "$id", Base);

    if (!BaseObj && BaseNode)
      BaseObj = BaseNode->getAsObject();

    if (!BaseObj) {
      auto Msg = llvm::formatv("Unsupported $ref {0}: unknown $id \"{1}\"",
                               elideForMsg(*RefStr), elideForMsg(Base))
                     .str();
      return llvm::createStringError(llvm::inconvertibleErrorCode(), Msg);
    }

    // Empty fragment selects the base schema itself.
    if (Frag.empty()) {
      if (auto Err = validateTarget(BaseObj == &RootSchema, BaseNode))
        return Err;
      break;
    }

    if (Frag.starts_with("/")) {
      // Preserve prior behavior: treat "<id>#/" as the base schema itself.
      StringRef Ptr = Frag;
      Ptr.consume_front("/");
      if (Ptr.empty()) {
        if (auto Err = validateTarget(BaseObj == &RootSchema, BaseNode))
          return Err;
        break;
      }

      const Value *Val = nullptr;
      if (auto Err = resolvePointer(*BaseObj, Ptr, Val))
        return Err;

      if (auto Err = validateTarget(/*IsRoot*/ false, Val))
        return Err;
      break;
    }

    // "<id>#<anchor>".
    if (BaseObj == &RootSchema) {
      if (auto A = RootSchema.getString("$anchor")) {
        if (*A == Frag) {
          if (auto Err = validateTarget(/*IsRoot*/ true, nullptr))
            return Err;
          break;
        }
      }
      const Value *Found =
          findFirstObjectWithStringFieldInRoot(RootSchema, "$anchor", Frag);
      if (auto Err = validateTarget(/*IsRoot*/ false, Found))
        return Err;
      break;
    }

    if (auto A = BaseObj->getString("$anchor")) {
      if (*A == Frag) {
        if (auto Err = validateTarget(/*IsRoot*/ false, BaseNode))
          return Err;
        break;
      }
    }

    const Value *Found = findFirstObjectWithStringField(*BaseNode, "$anchor", Frag);
    if (auto Err = validateTarget(/*IsRoot*/ false, Found))
      return Err;
    break;
    } while (false);
    // Continue validating sibling keywords in this schema (draft 2020-12).
  }

  // Enforce "const": value must equal the literal in the schema.
  if (auto It = Schema.find("const"); It != Schema.end()) {
    if (!(It->second == V)) {
      return createStringError(inconvertibleErrorCode(),
                               formatv("Value {0} does not equal const at {1} "
                                       "(expected {2})",
                                       V, prettyPath(Path), It->second)
                                   .str());
    }
  }

  // Enforce "oneOf": exactly one subschema must validate.
  //
  // Per draft 2020-12, sibling keywords are still evaluated even when "oneOf"
  // is present. So we validate each alternative, enforce the cardinality rule,
  // and then continue with the rest of this schema.
  if (auto OneOf = getArrayField(Schema, "oneOf")) {
    size_t Matches = 0;
    for (const auto &Alt : **OneOf) {
      if (auto Err = validateSubschema(V, Alt, Path)) {
        consumeError(std::move(Err)); // alternative didn't match
      } else {
        ++Matches;
        if (Matches > 1)
          break;
      }
    }

    if (Matches != 1) {
      auto Msg =
          formatv("oneOf failed at {0}: expected exactly 1 match, got {1}",
                  prettyPath(Path), Matches)
              .str();
      return createStringError(inconvertibleErrorCode(), Msg);
    }
  }

  // Enforce "anyOf": at least one subschema must validate.
  if (auto AnyOf = getArrayField(Schema, "anyOf")) {
    bool Matched = false;
    for (const auto &Alt : **AnyOf) {
      if (auto Err = validateSubschema(V, Alt, Path)) {
        consumeError(std::move(Err)); // alternative didn't match
      } else {
        Matched = true;
        break;
      }
    }
    if (!Matched) {
      auto Msg = formatv("anyOf failed at {0}: no alternative matched",
                         prettyPath(Path))
                     .str();
      return createStringError(inconvertibleErrorCode(), Msg);
    }
  }

  // Enforce "allOf": all subschemas must validate.
  if (auto AllOf = getArrayField(Schema, "allOf")) {
    for (const auto &Alt : **AllOf) {
      if (auto Err = validateSubschema(V, Alt, Path))
        return Err;
    }
  }

  // Enforce "not": must fail to validate the subschema.
  if (auto ItNot = Schema.find("not"); ItNot != Schema.end()) {
    // Succeeds if the instance does NOT validate against the subschema.
    if (auto Err = validateSubschema(V, ItNot->second, Path)) {
      consumeError(std::move(Err)); // subschema failed => "not" passes
    } else {
      auto Msg = formatv("not failed at {0}: instance unexpectedly matched",
                         prettyPath(Path))
                     .str();
      return createStringError(inconvertibleErrorCode(), Msg);
    }
  }

  // Conditional: if/then/else
  if (auto ItIf = Schema.find("if"); ItIf != Schema.end()) {
    bool IfPasses = false;
    if (auto Err = validateSubschema(V, ItIf->second, Path)) {
      consumeError(std::move(Err)); // 'if' failed
      IfPasses = false;
    } else {
      IfPasses = true;
    }

    if (IfPasses) {
      if (auto ItThen = Schema.find("then"); ItThen != Schema.end()) {
        if (auto Err = validateSubschema(V, ItThen->second, Path))
          return Err;
      }
    } else {
      if (auto ItElse = Schema.find("else"); ItElse != Schema.end()) {
        if (auto Err = validateSubschema(V, ItElse->second, Path))
          return Err;
      }
    }
  }

  // Number & string constraints are independent of "type", but only apply if
  // the instance is of the right runtime type.
  auto validateNumberConstraints = [&](double N) -> Error {
    // Read bounds once
    auto MinNum = Schema.getNumber("minimum");
    auto MaxNum = Schema.getNumber("maximum");
    auto ExMinNum = Schema.getNumber("exclusiveMinimum"); // numeric form
    auto ExMaxNum = Schema.getNumber("exclusiveMaximum"); // numeric form
    auto ExMinBool =
        Schema.getBoolean("exclusiveMinimum"); // legacy boolean form
    auto ExMaxBool =
        Schema.getBoolean("exclusiveMaximum"); // legacy boolean form

    // Schema-lint: boolean exclusiveMinimum/Maximum require the paired bound.
    // (Skip this if the numeric exclusive* is present, which supersedes
    // boolean.)
    if (ExMinBool && *ExMinBool && !ExMinNum && !MinNum) {
      auto Msg =
          formatv(
              "Schema error at {0}: exclusiveMinimum=true requires 'minimum'",
              prettyPath(Path))
              .str();
      return createStringError(inconvertibleErrorCode(), Msg);
    }
    if (ExMaxBool && *ExMaxBool && !ExMaxNum && !MaxNum) {
      auto Msg =
          formatv(
              "Schema error at {0}: exclusiveMaximum=true requires 'maximum'",
              prettyPath(Path))
              .str();
      return createStringError(inconvertibleErrorCode(), Msg);
    }

    // minimum (inclusive)
    if (MinNum && N < *MinNum) {
      auto Msg = formatv("Number at {0}: got {1}, requires >= {2} (minimum)",
                         prettyPath(Path), N, *MinNum)
                     .str();
      return createStringError(inconvertibleErrorCode(), Msg);
    }

    // maximum (inclusive)
    if (MaxNum && N > *MaxNum) {
      auto Msg = formatv("Number at {0}: got {1}, requires <= {2} (maximum)",
                         prettyPath(Path), N, *MaxNum)
                     .str();
      return createStringError(inconvertibleErrorCode(), Msg);
    }

    // exclusiveMinimum / exclusiveMaximum (numeric forms)
    if (ExMinNum && N <= *ExMinNum) {
      auto Msg =
          formatv("Number at {0}: got {1}, requires > {2} (exclusiveMinimum)",
                  prettyPath(Path), N, *ExMinNum)
              .str();
      return createStringError(inconvertibleErrorCode(), Msg);
    }
    if (ExMaxNum && N >= *ExMaxNum) {
      auto Msg =
          formatv("Number at {0}: got {1}, requires < {2} (exclusiveMaximum)",
                  prettyPath(Path), N, *ExMaxNum)
              .str();
      return createStringError(inconvertibleErrorCode(), Msg);
    }

    // exclusiveMinimum / exclusiveMaximum (boolean forms)
    if (ExMinBool && *ExMinBool && MinNum && N <= *MinNum) {
      auto Msg = formatv("Number at {0}: got {1}, requires > {2} "
                         "(minimum + exclusiveMinimum=true)",
                         prettyPath(Path), N, *MinNum)
                     .str();
      return createStringError(inconvertibleErrorCode(), Msg);
    }
    if (ExMaxBool && *ExMaxBool && MaxNum && N >= *MaxNum) {
      auto Msg = formatv("Number at {0}: got {1}, requires < {2} "
                         "(maximum + exclusiveMaximum=true)",
                         prettyPath(Path), N, *MaxNum)
                     .str();
      return createStringError(inconvertibleErrorCode(), Msg);
    }

    // multipleOf
    if (auto Mul = Schema.getNumber("multipleOf")) {
      if (*Mul <= 0) {
        auto Msg =
            formatv("Schema error at {0}: multipleOf must be > 0 (got {1})",
                    prettyPath(Path), *Mul)
                .str();
        return createStringError(inconvertibleErrorCode(), Msg);
      }

      double Rem = std::fmod(N, *Mul);
      if (Rem < 0)
        Rem += *Mul; // normalize to [0, mul)

      // Tolerance for FP roundoff. You can keep it constant, or scale it:
      // constexpr double Eps = 1e-12;
      double Eps = 1e-12 * std::max(1.0, std::abs(N));

      // Valid if remainder is ~0 or ~mul (both mean “on a multiple” within eps)
      if (Rem > Eps && (*Mul - Rem) > Eps) {
        auto Msg = formatv("Number at {0}: got {1}, requires a multiple of {2}",
                           prettyPath(Path), N, *Mul)
                       .str();
        return createStringError(inconvertibleErrorCode(), Msg);
      }
    }

    return Error::success();
  };

  auto validateStringConstraints = [&](StringRef S) -> Error {
    std::optional<size_t> CodePointLen;
    auto getLen = [&]() -> Expected<size_t> {
      if (CodePointLen)
        return *CodePointLen;
      auto L = utf8CodePointLength(S);
      if (!L)
        return L.takeError();
      CodePointLen = *L;
      return *CodePointLen;
    };

    // minLength
    if (auto ML = Schema.getInteger("minLength")) {
      if (*ML < 0) {
        auto Msg =
            formatv("Schema error at {0}: minLength must be >= 0 (got {1})",
                    prettyPath(Path), *ML)
                .str();
        return createStringError(inconvertibleErrorCode(), Msg);
      }
      size_t MinLen = static_cast<size_t>(*ML);
      auto Len = getLen();
      if (!Len)
        return createStringError(
            inconvertibleErrorCode(),
            formatv("String at {0}: invalid UTF-8", prettyPath(Path)).str());
      if (*Len < MinLen) {
        auto Msg =
            formatv(
                "String at {0}: length {1} < minLength {2} (value: \"{3}\")",
                prettyPath(Path), *Len, MinLen, elideForMsg(S))
                .str();
        return createStringError(inconvertibleErrorCode(), Msg);
      }
    }

    // maxLength
    if (auto ML = Schema.getInteger("maxLength")) {
      if (*ML < 0) {
        auto Msg =
            formatv("Schema error at {0}: maxLength must be >= 0 (got {1})",
                    prettyPath(Path), *ML)
                .str();
        return createStringError(inconvertibleErrorCode(), Msg);
      }
      size_t MaxLen = static_cast<size_t>(*ML);
      auto Len = getLen();
      if (!Len)
        return createStringError(
            inconvertibleErrorCode(),
            formatv("String at {0}: invalid UTF-8", prettyPath(Path)).str());
      if (*Len > MaxLen) {
        auto Msg =
            formatv(
                "String at {0}: length {1} > maxLength {2} (value: \"{3}\")",
                prettyPath(Path), *Len, MaxLen, elideForMsg(S))
                .str();
        return createStringError(inconvertibleErrorCode(), Msg);
      }
    }

    // pattern
    if (auto Pat = getStringField(Schema, "pattern")) {
      Regex R(*Pat);
      std::string E;
      if (!R.isValid(E)) {
        auto Msg =
            formatv(
                "Schema error at {0}: invalid regex for 'pattern' \"{1}\": {2}",
                prettyPath(Path), *Pat, E)
                .str();
        return createStringError(inconvertibleErrorCode(), Msg);
      }
      if (!R.match(S)) {
        auto Msg =
            formatv(
                "String at {0}: value \"{1}\" does not match pattern \"{2}\"",
                prettyPath(Path), elideForMsg(S), *Pat)
                .str();
        return createStringError(inconvertibleErrorCode(), Msg);
      }
    }
    // format
    if (auto Fmt = getStringField(Schema, "format")) {
      // Ensure valid UTF-8 when validating string formats.
      auto Len = getLen();
      if (!Len)
        return createStringError(
            inconvertibleErrorCode(),
            formatv("String at {0}: invalid UTF-8", prettyPath(Path)).str());

      if (auto Err = validateStringFormat(S, *Fmt, Path))
        return Err;
    }

    return Error::success();
  };

  if (auto Type = getStringField(Schema, "type")) {
    // Validate the types to make sure they are what we are expeecting.
    StringRef T = *Type;
    if (T == "object") {
      if (!V.getAsObject())
        return createStringError(inconvertibleErrorCode(),
                                 "Expected object at " + prettyPath(Path));
      if (auto Err = validateObject(*V.getAsObject(), Schema, Path))
        return Err;
    } else if (T == "array") {
      if (!V.getAsArray())
        return createStringError(inconvertibleErrorCode(),
                                 "Expected array at " + prettyPath(Path));
      if (auto Err = validateArray(*V.getAsArray(), Schema, Path))
        return Err;
    } else if (T == "string") {
      if (!V.getAsString())
        return createStringError(inconvertibleErrorCode(),
                                 "Expected string at " + prettyPath(Path));
      if (auto Err = validateStringConstraints(*V.getAsString()))
        return Err;
    } else if (T == "number") {
      if (auto N = V.getAsNumber()) {
        if (!std::isfinite(*N))
          return createStringError(inconvertibleErrorCode(),
                                   "Expected finite number at " +
                                       prettyPath(Path));
        if (auto Err = validateNumberConstraints(*N))
          return Err;
      } else {
        return createStringError(inconvertibleErrorCode(),
                                 "Expected number at " + prettyPath(Path));
      }
    } else if (T == "integer") {
      if (auto N = V.getAsNumber()) {
        if (std::floor(*N) != *N)
          return createStringError(inconvertibleErrorCode(),
                                   "Expected integer at " + prettyPath(Path));
        if (auto Err = validateNumberConstraints(*N))
          return Err;
      } else {
        return createStringError(inconvertibleErrorCode(),
                                 "Expected integer at " + prettyPath(Path));
      }
    } else if (T == "boolean") {
      if (!V.getAsBoolean())
        return createStringError(inconvertibleErrorCode(),
                                 "Expected boolean at " + prettyPath(Path));
    } else if (T == "null") {
      if (!isJsonNull(V))
        return createStringError(inconvertibleErrorCode(),
                                 "Expected null at " + prettyPath(Path));
    } else {
      // Unknown/unsupported type string in schema
      return createStringError(inconvertibleErrorCode(),
                               ("Unsupported JSON Schema type '" + T.str() +
                                "' at " + prettyPath(Path)));
    }
  } else if (auto TypeArr = getArrayField(Schema, "type")) {
    // Union "type": e.g. ["string","null","integer"]
    SmallVector<StringRef, 8> Allowed;
    Allowed.reserve((*TypeArr)->size());
    for (const auto &TVal : **TypeArr) {
      if (auto Ts = TVal.getAsString())
        Allowed.push_back(*Ts);
    }

    auto runtimeType = [&](const Value &IV) -> const char * {
      if (IV.getAsObject())
        return "object";
      if (IV.getAsArray())
        return "array";
      if (IV.getAsString())
        return "string";
      if (auto N = IV.getAsNumber())
        return (std::floor(*N) == *N) ? "integer" : "number";
      if (IV.getAsBoolean())
        return "boolean";
      if (isJsonNull(IV))
        return "null";
      return "unknown";
    };

    auto isAllowed = [&](const Value &IV) -> bool {
      if (IV.getAsObject())
        return llvm::is_contained(Allowed, "object");
      if (IV.getAsArray())
        return llvm::is_contained(Allowed, "array");
      if (IV.getAsString())
        return llvm::is_contained(Allowed, "string");
      if (auto N = IV.getAsNumber())
        return llvm::is_contained(Allowed, "number") ||
               (std::floor(*N) == *N && llvm::is_contained(Allowed, "integer"));
      if (IV.getAsBoolean())
        return llvm::is_contained(Allowed, "boolean");
      if (isJsonNull(IV))
        return llvm::is_contained(Allowed, "null");
      return false;
    };

    if (!isAllowed(V)) {
      // Render the instance value as JSON.
      std::string ValueStr;
      {
        raw_string_ostream os(ValueStr);
        os << V;
      }
      std::string AllowedCsv = join(Allowed, ", ");
      auto Msg =
          formatv("Type mismatch at {0}: value {1} has runtime type \"{2}\", "
                  "but schema 'type' allows only [{3}]",
                  prettyPath(Path), ValueStr, runtimeType(V), AllowedCsv)
              .str();
      return createStringError(inconvertibleErrorCode(), Msg);
    }

    // Apply type-specific constraints based on the instance's runtime type.
    if (auto O = V.getAsObject()) {
      if (auto Err = validateObject(*O, Schema, Path))
        return Err;
    }
    if (auto A = V.getAsArray()) {
      if (auto Err = validateArray(*A, Schema, Path))
        return Err;
    }
    if (auto S = V.getAsString()) {
      if (auto Err = validateStringConstraints(*S))
        return Err;
    }
    if (auto N = V.getAsNumber()) {
      if (auto Err = validateNumberConstraints(*N))
        return Err;
    }
  } else {
    // No explicit "type": in JSON Schema, type-applicable keywords still apply
    // based on the instance's runtime type.
    if (auto O = V.getAsObject()) {
      if (auto Err = validateObject(*O, Schema, Path))
        return Err;
    }
    if (auto A = V.getAsArray()) {
      if (auto Err = validateArray(*A, Schema, Path))
        return Err;
    }
    if (auto S = V.getAsString()) {
      if (auto Err = validateStringConstraints(*S))
        return Err;
    }
    if (auto N = V.getAsNumber()) {
      if (!std::isfinite(*N))
        return createStringError(inconvertibleErrorCode(),
                                 "Expected finite number at " +
                                     prettyPath(Path));
      if (auto Err = validateNumberConstraints(*N))
        return Err;
    }
  }

  // Enums in the JSON schema are specifically on arrays.
  if (auto EnumArr = getArrayField(Schema, "enum")) {
    // Attempt to locate the value in the enum list.
    bool Matched = false;
    for (const auto &Elem : **EnumArr) {
      if (Elem == V) {
        Matched = true;
        break;
      }
    }

    if (!Matched) {
      // Uh, oh. We did not find a match, so report the error.
      std::string EnumStr;
      raw_string_ostream OS(EnumStr);
      OS << "[";
      const auto &Array = **EnumArr;
      for (size_t Idx = 0; Idx < Array.size(); ++Idx) {
        OS << Array[Idx];
        if (Idx + 1 < Array.size())
          OS << ", ";
      }
      OS << "]";
      return createStringError(inconvertibleErrorCode(),
                               formatv("Value {0} not in enum list at {1}: {2}",
                                       V, prettyPath(Path), OS.str())
                                   .str());
    }
  }

  // NOTE: Remaining TODOs:
  // - format (date-time, email, etc.) — optional for now.
  // - contains/prefixItems (arrays) — see validateArray().
  // - dependentSchemas/propertyNames/patternProperties — see validateObject().

  return Error::success();
}

Error JSONSchemaValidator::validateObject(const Object &Obj,
                                          const Object &Schema,
                                          std::string Path) const {
  auto withPath = [&](StringRef Child) -> std::string {
    return Path.empty() ? Child.str() : (Path + "." + Child.str());
  };

  auto asValueCopy = [](const Object &O) -> Value {
    Object Copy = O;               // copy to a non-const
    return Value(std::move(Copy)); // move into a Value
  };

  auto validateSubschema = [&](const Value &Instance, const Value &SubSchema,
                               std::string SubPath) -> Error {
    // Draft 2020-12: a schema is either an object or a boolean.
    if (auto B = SubSchema.getAsBoolean()) {
      if (*B)
        return Error::success();
      return createStringError(
          inconvertibleErrorCode(),
          formatv("Schema 'false' rejects instance at {0}", prettyPath(SubPath))
              .str());
    }
    if (auto O = SubSchema.getAsObject())
      return validateValue(Instance, *O, std::move(SubPath));
    return createStringError(
        inconvertibleErrorCode(),
        formatv("Schema error: subschema must be an object or boolean at {0}",
                prettyPath(SubPath))
            .str());
  };

  auto ItUnevaluatedProperties = Schema.find("unevaluatedProperties");
  const bool HasUnevaluatedProperties =
      (ItUnevaluatedProperties != Schema.end());
  SmallDenseSet<StringRef, 32> EvaluatedProperties;
  if (HasUnevaluatedProperties) {
    // Full draft-2020-12 unevaluatedProperties semantics require tracking
    // evaluation annotations across applicators ($ref, allOf/anyOf/oneOf,
    // if/then/else, not). This validator currently only supports the local
    // case where evaluation is driven by properties/patternProperties/
    // additionalProperties within this object schema.
    if (Schema.find("$ref") != Schema.end() ||
        Schema.find("allOf") != Schema.end() ||
        Schema.find("anyOf") != Schema.end() ||
        Schema.find("oneOf") != Schema.end() ||
        Schema.find("if") != Schema.end() ||
        Schema.find("then") != Schema.end() ||
        Schema.find("else") != Schema.end() ||
        Schema.find("not") != Schema.end() ||
        Schema.find("dependentSchemas") != Schema.end()) {
      return createStringError(
          inconvertibleErrorCode(),
          "Schema error at %s: unevaluatedProperties requires cross-subschema "
          "evaluation tracking (not implemented)",
          Path.empty() ? "<root>" : Path.c_str());
    }
  }

  if (auto MP = Schema.getInteger("minProperties")) {
    if (*MP < 0) {
      return createStringError(inconvertibleErrorCode(),
                               "minProperties must be >= 0 at " +
                                   prettyPath(Path));
    }
    size_t MinProps = static_cast<size_t>(*MP);
    if (Obj.size() < MinProps) {
      return createStringError(
          inconvertibleErrorCode(),
          formatv("Object at {0}: requires >= {1} properties (got {2})",
                  prettyPath(Path), MinProps, Obj.size())
              .str());
    }
  }

  if (auto MP = Schema.getInteger("maxProperties")) {
    if (*MP < 0) {
      return createStringError(inconvertibleErrorCode(),
                               "maxProperties must be >= 0 at " +
                                   prettyPath(Path));
    }
    size_t MaxProps = static_cast<size_t>(*MP);
    if (Obj.size() > MaxProps) {
      return createStringError(
          inconvertibleErrorCode(),
          formatv("Object at {0}: requires <= {1} properties (got {2})",
                  prettyPath(Path), MaxProps, Obj.size())
              .str());
    }
  }

  // dependentRequired: if K present, require listed others.
  if (auto DepReq = getObjectField(Schema, "dependentRequired")) {
    for (const auto &[K, Vals] : **DepReq) {
      if (Obj.find(K) != Obj.end()) {
        if (auto Arr = Vals.getAsArray()) {
          for (const auto &ReqField : *Arr) {
            if (auto R = ReqField.getAsString()) {
              if (Obj.find(*R) == Obj.end()) {
                return createStringError(inconvertibleErrorCode(),
                                         "Missing dependentRequired field '" +
                                             R->str() + "' because '" +
                                             K.str() + "' is present at " +
                                             prettyPath(Path));
              }
            }
          }
        }
      }
    }
  }

  // dependentSchemas: if K present, the object must validate the subschema.
  if (auto DepSch = getObjectField(Schema, "dependentSchemas")) {
    for (const auto &[K, SubVal] : **DepSch) {
      if (Obj.find(K) != Obj.end()) {
        // Validate the WHOLE object against the subschema.
        if (auto Err = validateSubschema(asValueCopy(Obj), SubVal, Path))
          return Err;
      }
    }
  }

  if (auto Req = getArrayField(Schema, "required")) {
    // Iterate over all required fields as defined in the schema and ensure that
    // all fields are accounted for.
    for (const auto &Field : **Req) {
      if (auto FieldStr = Field.getAsString()) {
        if (Obj.find(*FieldStr) == Obj.end()) {
          return createStringError(inconvertibleErrorCode(),
                                   "Missing required field: " +
                                       prettyPath(withPath(*FieldStr)));
        }
      }
    }
  }

  if (auto Props = getObjectField(Schema, "properties")) {
    // Iterates over all properties in the schema and validates each property in
    // the JSON to ensure that those properties are defined and valid.
    for (const auto &[Key, SubSchemaVal] : **Props) {
      auto It = Obj.find(Key);
      if (It != Obj.end()) {
        std::string ChildPath = withPath(Key);
        if (auto Err = validateSubschema(It->second, SubSchemaVal, ChildPath))
          return Err;
        if (HasUnevaluatedProperties)
          EvaluatedProperties.insert(Key);
      }
    }
  }

  // patternProperties: apply schemas to keys matching regex.
  DenseMap<StringRef, const Value *> PatternSchemas;
  if (auto PatProps = getObjectField(Schema, "patternProperties")) {
    // Ensure all patterns are valid regexes.
    for (const auto &[Pat, SubVal] : **PatProps) {
      Regex R(Pat);
      std::string ErrMsg;
      if (!R.isValid(ErrMsg)) {
        return createStringError(
            inconvertibleErrorCode(),
            formatv("Invalid regex patternProperties[{0}] at {1}: {2}", Pat,
                    prettyPath(Path), ErrMsg)
                .str());
      }
      PatternSchemas[Pat] = &SubVal;
    }

    for (const auto &[Key, Val] : Obj) {
      std::string ChildPath = withPath(Key);
      for (const auto &[Pat, Sub] : PatternSchemas) {
        Regex R(Pat);
        if (R.match(Key)) {
          if (auto Err = validateSubschema(Val, *Sub, ChildPath))
            return Err;
          if (HasUnevaluatedProperties)
            EvaluatedProperties.insert(Key);
        }
      }
    }
  }

  // propertyNames: schema applied to each property name (instance is a string).
  if (auto ItPropNames = Schema.find("propertyNames");
      ItPropNames != Schema.end()) {
    for (const auto &[Key, _] : Obj) {
      if (auto Err = validateSubschema(Value(Key), ItPropNames->second,
                                       withPath("<propertyName>")))
        return Err;
    }
  }

  if (auto AP = Schema.getBoolean("additionalProperties"); AP && !*AP) {
    const Object *PropsObj = nullptr;
    if (auto Props = getObjectField(Schema, "properties"))
      PropsObj = &**Props;

    for (const auto &[Key, _] : Obj) {
      bool Known = PropsObj && (PropsObj->find(Key) != PropsObj->end());
      bool PatternOK = false;

      if (!Known && !PatternSchemas.empty()) {
        for (const auto &[Pat, _Sub] : PatternSchemas) {
          Regex R(Pat);
          std::string E;
          if (!R.isValid(E)) {
            return createStringError(inconvertibleErrorCode(),
                                     ("Invalid regex in patternProperties at " +
                                      prettyPath(Path) + ": " + Pat.str()));
          }
          if (R.match(Key)) {
            PatternOK = true;
            break;
          }
        }
      }

      if (!Known && !PatternOK) {
        std::string Full = withPath(Key);
        return createStringError(inconvertibleErrorCode(),
                                 ("Unexpected property: " + prettyPath(Full)));
      }
    }
  } else if (auto APObj = getObjectField(Schema, "additionalProperties")) {
    const Object &Aps = **APObj;
    const Object *PropsObj = nullptr;
    if (auto Props = getObjectField(Schema, "properties"))
      PropsObj = &**Props;

    for (const auto &[Key, Val] : Obj) {
      bool Known = PropsObj && (PropsObj->find(Key) != PropsObj->end());
      bool PatternOK = false;

      if (!Known && !PatternSchemas.empty()) {
        for (const auto &[Pat, _Sub] : PatternSchemas) {
          Regex R(Pat);
          if (R.match(Key)) {
            PatternOK = true;
            break;
          }
        }
      }

      if (!Known && !PatternOK) {
        if (auto Err = validateValue(Val, Aps, withPath(Key)))
          return Err;
        if (HasUnevaluatedProperties)
          EvaluatedProperties.insert(Key);
      }
    }
  }

  if (HasUnevaluatedProperties) {
    for (const auto &KV : Obj) {
      StringRef Key = KV.first;
      if (EvaluatedProperties.count(Key))
        continue;
      if (auto Err = validateSubschema(KV.second, ItUnevaluatedProperties->second,
                                      withPath(Key)))
        return Err;
    }
  }

  // NOTE: if/then/else and not are handled in validateValue().

  return Error::success();
}

Error JSONSchemaValidator::validateArray(const Array &Arr, const Object &Schema,
                                         std::string Path) const {
  auto indexPath = [&](size_t Idx) -> std::string {
    return Path.empty() ? formatv("[{0}]", Idx).str()
                        : formatv("{0}[{1}]", Path, Idx).str();
  };

  auto validateSubschema = [&](const Value &Instance, const Value &SubSchema,
                               std::string SubPath) -> Error {
    // Draft 2020-12: a schema is either an object or a boolean.
    if (auto B = SubSchema.getAsBoolean()) {
      if (*B)
        return Error::success();
      return createStringError(
          inconvertibleErrorCode(),
          formatv("Schema 'false' rejects instance at {0}", prettyPath(SubPath))
              .str());
    }
    if (auto O = SubSchema.getAsObject())
      return validateValue(Instance, *O, std::move(SubPath));
    return createStringError(
        inconvertibleErrorCode(),
        formatv("Schema error: subschema must be an object or boolean at {0}",
                prettyPath(SubPath))
            .str());
  };

  auto ItUnevaluatedItems = Schema.find("unevaluatedItems");
  const bool HasUnevaluatedItems = (ItUnevaluatedItems != Schema.end());
  SmallBitVector EvaluatedItems;
  if (HasUnevaluatedItems) {
    // Full draft-2020-12 unevaluatedItems semantics require tracking
    // evaluation annotations across applicators ($ref, allOf/anyOf/oneOf,
    // if/then/else, not). This validator currently only supports the local
    // case where evaluation is driven by prefixItems/items/contains within
    // this array schema.
    if (Schema.find("$ref") != Schema.end() ||
        Schema.find("allOf") != Schema.end() ||
        Schema.find("anyOf") != Schema.end() ||
        Schema.find("oneOf") != Schema.end() ||
        Schema.find("if") != Schema.end() ||
        Schema.find("then") != Schema.end() ||
        Schema.find("else") != Schema.end() ||
        Schema.find("not") != Schema.end()) {
      return createStringError(
          inconvertibleErrorCode(),
          "Schema error at %s: unevaluatedItems requires cross-subschema "
          "evaluation tracking (not implemented)",
          Path.empty() ? "<root>" : Path.c_str());
    }
    EvaluatedItems.resize(Arr.size());
  }

  if (auto MinItemsVal = Schema.getInteger("minItems")) {
    if (*MinItemsVal < 0)
      return createStringError(inconvertibleErrorCode(),
                               "minItems must be >= 0 at " + prettyPath(Path));
    size_t MinItems = static_cast<size_t>(*MinItemsVal);
    if (Arr.size() < MinItems) {
      return createStringError(
          inconvertibleErrorCode(),
          formatv("Array at {0}: requires >= {1} items (got {2})",
                  prettyPath(Path), MinItems, Arr.size())
              .str());
    }
  }

  if (auto MaxItemsVal = Schema.getInteger("maxItems")) {
    if (*MaxItemsVal < 0)
      return createStringError(inconvertibleErrorCode(),
                               "maxItems must be >= 0 at " + prettyPath(Path));
    size_t MaxItems = static_cast<size_t>(*MaxItemsVal);
    if (Arr.size() > MaxItems) {
      return createStringError(
          inconvertibleErrorCode(),
          formatv("Array at {0}: requires <= {1} items (got {2})",
                  prettyPath(Path), MaxItems, Arr.size())
              .str());
    }
  }

  if (auto Unique = Schema.getBoolean("uniqueItems"); Unique && *Unique) {
    // Make sure there are no duplicate items in the array.
    auto valuesEqual = [&](const Value &A, const Value &B, auto &&Self) -> bool {
      if (isJsonNull(A) || isJsonNull(B))
        return isJsonNull(A) && isJsonNull(B);

      if (auto AO = A.getAsObject()) {
        auto BO = B.getAsObject();
        if (!BO || AO->size() != BO->size())
          return false;
        for (const auto &KV : *AO) {
          auto It = BO->find(KV.first);
          if (It == BO->end() || !Self(KV.second, It->second, Self))
            return false;
        }
        return true;
      }

      if (auto AA = A.getAsArray()) {
        auto BA = B.getAsArray();
        if (!BA || AA->size() != BA->size())
          return false;
        for (size_t I = 0; I < AA->size(); ++I)
          if (!Self((*AA)[I], (*BA)[I], Self))
            return false;
        return true;
      }

      if (auto AS = A.getAsString()) {
        auto BS = B.getAsString();
        return BS && *AS == *BS;
      }

      if (auto AN = A.getAsNumber()) {
        auto BN = B.getAsNumber();
        return BN && *AN == *BN;
      }

      if (auto AB = A.getAsBoolean()) {
        auto BB = B.getAsBoolean();
        return BB && *AB == *BB;
      }

      return false;
    };

    for (size_t Idx = 0; Idx < Arr.size(); ++Idx) {
      for (size_t Prev = 0; Prev < Idx; ++Prev) {
        if (valuesEqual(Arr[Idx], Arr[Prev], valuesEqual)) {
          std::string DupPath = indexPath(Idx);
          std::string Repr = formatv("{0}", Arr[Idx]).str();
          return createStringError(
              inconvertibleErrorCode(),
              formatv("Duplicate array item at {0}: {1} (already seen at index {2})",
                      prettyPath(DupPath), Repr, std::to_string(Prev))
                  .str());
        }
      }
    }
  }

  // prefixItems: array of schemas for the first N elements (draft-2020-12).
  bool HasPrefixItems = false;
  size_t PrefixN = 0;
  if (auto Pfx = getArrayField(Schema, "prefixItems")) {
    HasPrefixItems = true;
    PrefixN = (**Pfx).size();
    for (size_t Idx = 0; Idx < Arr.size() && Idx < PrefixN; ++Idx) {
      std::string ElemPath = indexPath(Idx);
      if (auto Err = validateSubschema(Arr[Idx], (**Pfx)[Idx], ElemPath))
        return Err;
      if (HasUnevaluatedItems)
        EvaluatedItems.set(Idx);
    }
  }

  // items: draft-2020-12 treats "items" as a single subschema that applies to
  // all *remaining* items after prefixItems. If prefixItems is absent, it
  // applies to all items.
  //
  // We also support the legacy tuple form "items": [ ... ] (pre-2020-12), but
  // only when prefixItems is not present (to avoid double-applying constraints).
  if (auto ItItems = Schema.find("items"); ItItems != Schema.end()) {
    if (const Array *ItemsArr = ItItems->second.getAsArray()) {
      if (!HasPrefixItems) {
        size_t N = ItemsArr->size();
        for (size_t Idx = 0; Idx < Arr.size() && Idx < N; ++Idx) {
          std::string ElemPath = indexPath(Idx);
          if (auto Err = validateSubschema(Arr[Idx], (*ItemsArr)[Idx], ElemPath))
            return Err;
          if (HasUnevaluatedItems)
            EvaluatedItems.set(Idx);
        }
        // Extra items beyond tuple size: allowed. We leave them unconstrained
        // unless other keywords ("contains", etc.) restrict them.
      }
    } else {
      const size_t Start = HasPrefixItems ? PrefixN : 0;
      for (size_t Idx = Start; Idx < Arr.size(); ++Idx) {
        std::string ElemPath = indexPath(Idx);
        if (auto Err = validateSubschema(Arr[Idx], ItItems->second, ElemPath))
          return Err;
        if (HasUnevaluatedItems)
          EvaluatedItems.set(Idx);
      }
    }
  }

  // contains: at least one item must match the subschema.
  if (auto ItContains = Schema.find("contains"); ItContains != Schema.end()) {
    size_t Matches = 0;
    for (size_t Idx = 0; Idx < Arr.size(); ++Idx) {
      std::string ElemPath = indexPath(Idx);
      if (auto Err =
              validateSubschema(Arr[Idx], ItContains->second, ElemPath)) {
        consumeError(std::move(Err)); // element doesn't match; ignore
      } else {
        ++Matches;
        if (HasUnevaluatedItems)
          EvaluatedItems.set(Idx);
      }
    }

    // minContains defaults to 1 when "contains" is present
    size_t MinContains = 1;
    if (auto MC = Schema.getInteger("minContains")) {
      if (*MC < 0) {
        return createStringError(inconvertibleErrorCode(),
                                 "minContains must be >= 0 at " +
                                     prettyPath(Path));
      }
      MinContains = static_cast<size_t>(*MC);
    }

    if (Matches < MinContains) {
      return createStringError(
          inconvertibleErrorCode(),
          formatv(
              "Array at {0}: requires >= {1} matches for 'contains' (got {2})",
              prettyPath(Path), MinContains, Matches)
              .str());
    }

    if (auto MC = Schema.getInteger("maxContains")) {
      if (*MC < 0) {
        return createStringError(inconvertibleErrorCode(),
                                 "maxContains must be >= 0 at " +
                                     prettyPath(Path));
      }
      size_t MaxContains = static_cast<size_t>(*MC);
      if (Matches > MaxContains) {
        return createStringError(inconvertibleErrorCode(),
                                 formatv("Array at {0}: requires <= {1} "
                                         "matches for 'contains' (got {2})",
                                         prettyPath(Path), MaxContains, Matches)
                                     .str());
      }

      if (MinContains > MaxContains) {
        return createStringError(
            inconvertibleErrorCode(),
            formatv("minContains ({0}) > maxContains ({1}) at {2}", MinContains,
                    MaxContains, prettyPath(Path))
                .str());
      }
    }
  }

  if (HasUnevaluatedItems) {
    for (size_t Idx = 0; Idx < Arr.size(); ++Idx) {
      if (EvaluatedItems.test(Idx))
        continue;
      std::string ElemPath = indexPath(Idx);
      if (auto Err = validateSubschema(Arr[Idx], ItUnevaluatedItems->second,
                                      ElemPath))
        return Err;
    }
  }

  return Error::success();
}

std::optional<StringRef>
JSONSchemaValidator::getStringField(const Object &Obj, StringRef Key) const {
  auto It = Obj.find(Key);
  if (It == Obj.end() || !It->second.getAsString())
    return std::nullopt;
  return *It->second.getAsString();
}

std::optional<const Object *>
JSONSchemaValidator::getObjectField(const Object &Obj, StringRef Key) const {
  auto It = Obj.find(Key);
  if (It == Obj.end() || !It->second.getAsObject())
    return std::nullopt;
  return It->second.getAsObject();
}

std::optional<const Array *>
JSONSchemaValidator::getArrayField(const Object &Obj, StringRef Key) const {
  auto It = Obj.find(Key);
  if (It == Obj.end() || !It->second.getAsArray())
    return std::nullopt;
  return It->second.getAsArray();
}

} // namespace json
} // namespace llvm
