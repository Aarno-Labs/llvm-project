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
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Regex.h"
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
} // namespace

Error JSONSchemaValidator::validate(const Value &V) const {
  RefCache.clear();
  return validateValue(V, RootSchema, "");
}

Error JSONSchemaValidator::validateValue(const Value &V, const Object &Schema,
                                         std::string Path) const {
  if (auto RefStr = getStringField(Schema, "$ref")) {
    auto It = RefCache.find(*RefStr);
    const Object *Resolved = nullptr;

    // First check the ref cache to see if there was a prior enrtry.
    if (It != RefCache.end()) {
      Resolved = It->second;
    } else {
      // Ok, so no entry, which means we must create a new one.
      if (!RefStr->starts_with("#/")) {
        return createStringError(inconvertibleErrorCode(),
                                 "Unsupported $ref format: " + *RefStr);
      }

      auto decodePointerToken = [](StringRef Tok) {
        std::string Out;
        Out.reserve(Tok.size());
        for (size_t Idx = 0; Idx < Tok.size(); ++Idx) {
          if (Tok[Idx] == '~' && Idx + 1 < Tok.size()) {
            char Char = Tok[Idx + 1];
            if (Char == '1') {
              Out.push_back('/');
              ++Idx;
              continue;
            }
            if (Char == '0') {
              Out.push_back('~');
              ++Idx;
              continue;
            }
          }
          Out.push_back(Tok[Idx]);
        }
        return Out; // std::string
      };

      SmallVector<StringRef, 8> Parts;
      StringRef RefPath = RefStr->drop_front(2);
      Parts.reserve(RefPath.count('/') + 1);
      RefPath.split(Parts, '/');

      // Resolve the object being reference by traversing the parts in the
      // reference path.
      const Object *Current = &RootSchema;
      for (StringRef Part : Parts) {
        std::string Decoded = decodePointerToken(Part);
        auto It = Current->find(Decoded);
        if (It != Current->end() && It->second.getAsObject()) {
          Current = It->second.getAsObject();
        } else {
          return createStringError(inconvertibleErrorCode(),
                                   "$ref path component not found: " + Decoded);
        }
      }

      // Current must be non-null if we get here.
      Resolved = Current;
      RefCache[*RefStr] = Resolved;
    }

    if (auto Err = validateValue(V, *Resolved, Path))
      return Err;
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
      if (const Object *Sub = Alt.getAsObject()) {
        // Probe this alternative; if it fails, discard the error.
        if (auto Err = validateValue(V, *Sub, Path)) {
          consumeError(std::move(Err));
        } else {
          ++Matches;
          if (Matches > 1)
            break; // already invalid; no need to keep probing
        }
      }
    }
    if (Matches == 1) {
      // Exactly one matched; continue evaluating sibling keywords.
    } else if (Matches == 0) {
      return createStringError(inconvertibleErrorCode(),
                               "Value did not match any 'oneOf' subschema at " +
                                   prettyPath(Path));
    } else {
      return createStringError(inconvertibleErrorCode(),
                               "Value matched multiple 'oneOf' subschemas at " +
                                   prettyPath(Path));
    }
  }

  // Enforce "anyOf": at least one subschema must validate.
  if (auto AnyOf = getArrayField(Schema, "anyOf")) {
    bool Matched = false;
    for (const auto &Alt : **AnyOf) {
      if (const Object *Sub = Alt.getAsObject()) {
        if (auto Err = validateValue(V, *Sub, Path)) {
          consumeError(std::move(Err)); // probe failed; ignore
        } else {
          Matched = true;
          break;
        }
      }
    }
    if (!Matched)
      return createStringError(inconvertibleErrorCode(),
                               "Value did not match any 'anyOf' subschema at " +
                                   prettyPath(Path));
  }

  // Enforce "allOf": all subschemas must validate.
  if (auto AllOf = getArrayField(Schema, "allOf")) {
    for (const auto &Alt : **AllOf) {
      if (const Object *Sub = Alt.getAsObject()) {
        if (auto Err = validateValue(V, *Sub, Path))
          return Err;
      }
    }
  }

  // Enforce "not": must fail to validate the subschema.
  if (auto Not = getObjectField(Schema, "not")) {
    if (auto Err = validateValue(V, **Not, Path)) {
      consumeError(std::move(Err)); // 'not' passes because subschema failed
    } else {
      return createStringError(inconvertibleErrorCode(),
                               "Value must NOT validate 'not' subschema at " +
                                   prettyPath(Path));
    }
  }

  // Conditional: if/then/else
  if (auto If = getObjectField(Schema, "if")) {
    bool IfPasses = false;
    if (auto Err = validateValue(V, **If, Path)) {
      consumeError(std::move(Err)); // 'if' failed
      IfPasses = false;
    } else {
      IfPasses = true;
    }
    if (IfPasses) {
      if (auto Then = getObjectField(Schema, "then")) {
        if (auto Err = validateValue(V, **Then, Path))
          return Err;
      }
    } else {
      if (auto Else = getObjectField(Schema, "else")) {
        if (auto Err = validateValue(V, **Else, Path))
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
      if (S.size() < MinLen) {
        auto Msg =
            formatv(
                "String at {0}: length {1} < minLength {2} (value: \"{3}\")",
                prettyPath(Path), S.size(), MinLen, elideForMsg(S))
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
      if (S.size() > MaxLen) {
        auto Msg =
            formatv(
                "String at {0}: length {1} > maxLength {2} (value: \"{3}\")",
                prettyPath(Path), S.size(), MaxLen, elideForMsg(S))
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
        if (const Object *Sub = SubVal.getAsObject()) {
          // Validate the WHOLE object against the subschema.
          if (auto Err = validateValue(asValueCopy(Obj), *Sub, Path))
            return Err;
        }
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
        if (auto SubSchemaObj = SubSchemaVal.getAsObject()) {
          std::string ChildPath = withPath(Key);
          if (auto Err = validateValue(It->second, *SubSchemaObj, ChildPath))
            return Err;
        }
      }
    }
  }

  // patternProperties: apply schemas to keys matching regex.
  DenseMap<StringRef, const Object *> PatternSchemas;
  if (auto PatProps = getObjectField(Schema, "patternProperties")) {
    for (const auto &[Pat, SubVal] : **PatProps)
      if (const Object *Sub = SubVal.getAsObject())
        PatternSchemas[Pat] = Sub;
    for (const auto &[Key, Val] : Obj) {
      for (const auto &[Pat, Sub] : PatternSchemas) {
        Regex R(Pat);
        std::string E;
        if (!R.isValid(E))
          return createStringError(inconvertibleErrorCode(),
                                   "Invalid regex in patternProperties at " +
                                       prettyPath(Path) + ": " + Pat.str());
        if (R.match(Key)) {
          std::string ChildPath = withPath(Key);
          if (auto Err = validateValue(Val, *Sub, ChildPath))
            return Err;
        }
      }
    }
  }

  // propertyNames: validate each key as a string instance.
  if (auto PropNames = getObjectField(Schema, "propertyNames")) {
    for (const auto &[Key, _] : Obj) {
      if (auto Err = validateValue(Value(Key), **PropNames,
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
      }
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
    StringMap<size_t> Seen;
    for (size_t Idx = 0; Idx < Arr.size(); ++Idx) {
      std::string Repr = formatv("{0}", Arr[Idx]).str();
      auto It = Seen.find(Repr);
      if (It != Seen.end()) {
        std::string DupPath = indexPath(Idx);
        return createStringError(
            inconvertibleErrorCode(),
            formatv(
                "Duplicate array item at {0}: {1} (already seen at index {2})",
                prettyPath(DupPath), Repr, std::to_string(It->second))
                .str());
      }
      Seen[Repr] = Idx;
    }
  }

  // "items" can be an object (single schema for all items) OR an array (tuple
  // form).
  if (auto Items = getObjectField(Schema, "items")) {
    for (size_t Idx = 0; Idx < Arr.size(); ++Idx) {
      std::string ElemPath = indexPath(Idx);
      if (auto Err = validateValue(Arr[Idx], **Items, ElemPath))
        return Err;
    }
  } else if (auto ItemsArr = getArrayField(Schema, "items")) {
    size_t N = (*ItemsArr)->size();
    for (size_t Idx = 0; Idx < Arr.size() && Idx < N; ++Idx) {
      if (const Object *Sub = (**ItemsArr)[Idx].getAsObject()) {
        std::string ElemPath = indexPath(Idx);
        if (auto Err = validateValue(Arr[Idx], *Sub, ElemPath))
          return Err;
      }
    }
    // Extra items beyond tuple size: allowed (draft-2020-12 recommends
    // prefixItems/items), we leave them unconstrained unless "items" (single
    // schema) or "contains" says otherwise.
  }

  // prefixItems: array of schemas for the first N elements (draft-2020-12).
  if (auto Pfx = getArrayField(Schema, "prefixItems")) {
    size_t N = (**Pfx).size();
    for (size_t Idx = 0; Idx < Arr.size() && Idx < N; ++Idx) {
      if (const Object *Sub = (**Pfx)[Idx].getAsObject()) {
        std::string ElemPath = indexPath(Idx);
        if (auto Err = validateValue(Arr[Idx], *Sub, ElemPath))
          return Err;
      }
    }
  }

  // contains: at least one item must match the subschema.
  if (auto Contains = getObjectField(Schema, "contains")) {
    size_t Matches = 0;
    for (size_t Idx = 0; Idx < Arr.size(); ++Idx) {
      std::string ElemPath = indexPath(Idx);
      if (auto Err = validateValue(Arr[Idx], **Contains, ElemPath)) {
        consumeError(std::move(Err)); // element doesn't match; ignore
      } else {
        ++Matches;
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
