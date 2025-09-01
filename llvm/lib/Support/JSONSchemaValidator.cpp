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
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_os_ostream.h"
#include <cmath>
#include <optional>
#include <string>

namespace llvm {
namespace json {

Error JSONSchemaValidator::validate(const Value &V) const {
  if (RefCache.empty())
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
      if (!RefStr->starts_with("#/"))
        return createStringError(inconvertibleErrorCode(),
                                 "Unsupported $ref format: " + *RefStr);

      SmallVector<StringRef, 8> Parts;
      StringRef RefPath = RefStr->drop_front(2);
      RefPath.split(Parts, '/');

      // Resolve the object being reference by traversing the parts in the
      // reference path.
      const Object *Current = &RootSchema;
      for (StringRef Part : Parts) {
        if (auto Inner = Current->find(Part);
            Inner != Current->end() && Inner->second.getAsObject()) {
          Current = Inner->second.getAsObject();
        } else {
          return createStringError(inconvertibleErrorCode(),
                                   "$ref path component not found: " + Part);
        }
      }
      // Current must be non-null if we get here.
      Resolved = Current;
      RefCache[*RefStr] = Resolved;
    }

    return validateValue(V, *Resolved, Path);
  }

  // Enforce "const": value must equal the literal in the schema.
  if (auto It = Schema.find("const"); It != Schema.end()) {
    if (!(It->second == V)) {
      return createStringError(inconvertibleErrorCode(),
                               formatv("Value {0} does not equal const at {1} "
                                       "(expected {2})",
                                       V, Path, It->second).str());
    }
  }

  // Enforce "oneOf": exactly one subschema must validate.
  if (auto OneOf = getArrayField(Schema, "oneOf")) {
    size_t Matches = 0;
    for (const auto &Alt : **OneOf) {
      if (const Object *Sub = Alt.getAsObject()) {
        // Try validating against this alternative; if it fails, discard error.
        if (auto Err = validateValue(V, *Sub, Path)) {
          consumeError(std::move(Err));
        } else {
          ++Matches;
        }
      }
    }
    if (Matches == 1)
      return Error::success();
    if (Matches == 0)
      return createStringError(inconvertibleErrorCode(),
                               "Value did not match any 'oneOf' subschema at " +
                                   Path);
    return createStringError(inconvertibleErrorCode(),
                             "Value matched multiple 'oneOf' subschemas at " +
                                 Path);
  }

  if (auto Type = getStringField(Schema, "type")) {
    // Validate the types to make sure they are what we are expeecting.
    StringRef T = *Type;
    if (T == "object") {
      if (!V.getAsObject())
        return createStringError(inconvertibleErrorCode(),
                                 "Expected object at " + Path);
      return validateObject(*V.getAsObject(), Schema, Path);
    } else if (T == "array") {
      if (!V.getAsArray())
        return createStringError(inconvertibleErrorCode(),
                                 "Expected array at " + Path);
      return validateArray(*V.getAsArray(), Schema, Path);
    } else if (T == "string") {
      if (!V.getAsString())
        return createStringError(inconvertibleErrorCode(),
                                 "Expected string at " + Path);
    } else if (T == "number") {
      if (auto N = V.getAsNumber()) {
        if (!std::isfinite(*N))
          return createStringError(inconvertibleErrorCode(),
                                   "Expected finite number at " + Path);
      } else {
        return createStringError(inconvertibleErrorCode(),
                                 "Expected number at " + Path);
      }
    } else if (T == "integer") {
      if (auto N = V.getAsNumber()) {
        if (std::floor(*N) != *N)
          return createStringError(inconvertibleErrorCode(),
                                   "Expected integer at " + Path);
      } else {
        return createStringError(inconvertibleErrorCode(),
                                 "Expected integer at " + Path);
      }
    } else if (T == "boolean") {
      if (!V.getAsBoolean())
        return createStringError(inconvertibleErrorCode(),
                                 "Expected boolean at " + Path);
    }
  }

  // Enums in the JSON schema are specifically on arrays.
  if (auto EnumArr = getArrayField(Schema, "enum")) {
    // Attempt to locate the value in the enum list.
    bool match = false;
    for (const auto &Elem : **EnumArr) {
      if (Elem == V) {
        match = true;
        break;
      }
    }

    if (!match) {
      // Uh, oh. We did not find a match, so report the error.
      std::string EnumStr;
      raw_string_ostream OS(EnumStr);
      OS << "[";
      const auto &Array = **EnumArr;
      for (size_t i = 0; i < Array.size(); ++i) {
        OS << Array[i];
        if (i + 1 < Array.size())
          OS << ", ";
      }
      OS << "]";
      return createStringError(
          inconvertibleErrorCode(),
          formatv("Value {0} not in enum list at {1}: {2}", V, Path, OS.str())
              .str());
    }
  }

  // TODO: Support const
  // TODO: Support format (e.g., date-time, email)
  // TODO: Support minimum, maximum, exclusiveMinimum, exclusiveMaximum
  // TODO: Support multipleOf for numbers
  // TODO: Support minLength, maxLength for strings
  // TODO: Support pattern for strings
  // TODO: Support oneOf, anyOf, allOf, not
  // TODO: Support if/then/else
  // TODO: Support contains and prefixItems for arrays
  // TODO: Support $ref and $defs (schema references)
  // TODO: Support dependentRequired, dependentSchemas
  // TODO: Support propertyNames

  return Error::success();
}

Error JSONSchemaValidator::validateObject(const Object &Obj,
                                          const Object &Schema,
                                          std::string Path) const {
  if (auto Req = getArrayField(Schema, "required")) {
    // Iterate over all required fields as defined in the schema and ensure that
    // all fields are accounted for.
    for (const auto &Field : **Req) {
      if (auto FieldStr = Field.getAsString()) {
        if (Obj.find(*FieldStr) == Obj.end()) {
          std::string FullPath = Path.empty() ? std::string(*FieldStr)
                                              : Path + "." + FieldStr->str();
          return createStringError(inconvertibleErrorCode(),
                                   "Missing required field: " + FullPath);
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
          std::string ChildPath =
              Path.empty() ? Key.str() : Path + "." + Key.str();
          if (auto Err = validateValue(It->second, *SubSchemaObj, ChildPath))
            return Err;
        }
      }
    }
  }

  if (auto AP = Schema.getBoolean("additionalProperties"); AP && !*AP) {
    // Additional properties is specified and is false, which means we must
    // return an error if we find a key in the JSON object that is not in the
    // properties.
    if (auto Props = getObjectField(Schema, "properties")) {
      for (const auto &[Key, _] : Obj) {
        if ((*Props)->find(Key) == (*Props)->end()) {
          std::string FullPath =
              Path.empty() ? Key.str() : Path + "." + Key.str();
          return createStringError(inconvertibleErrorCode(),
                                   "Unexpected property: " + FullPath);
        }
      }
    }
  }

  // TODO: Support patternProperties and propertyNames
  // TODO: Support dependentRequired, dependentSchemas
  // TODO: Support if/then/else and not within objects

  return Error::success();
}

Error JSONSchemaValidator::validateArray(const Array &Arr, const Object &Schema,
                                         std::string Path) const {
  if (auto MinItems = Schema.getInteger("minItems")) {
    // Check the array size, and return an error if it exceeds min items.
    if (Arr.size() < static_cast<size_t>(*MinItems))
      return createStringError(
          inconvertibleErrorCode(),
          "Array at " + Path +
              " has fewer items than minItems = " + std::to_string(*MinItems));
  }

  if (auto Unique = Schema.getBoolean("uniqueItems"); Unique && *Unique) {
    // Make sure there are no duplicate items in the array.
    StringMap<size_t> Seen;
    for (size_t i = 0; i < Arr.size(); ++i) {
      std::string Repr = formatv("{0}", Arr[i]).str();
      auto It = Seen.find(Repr);
      if (It != Seen.end()) {
        std::string DupPath = formatv("{0}[{1}]", Path, i).str();
        return createStringError(inconvertibleErrorCode(),
                                 "Duplicate array item at " + DupPath + ": " +
                                     Repr + " (already seen at index " +
                                     std::to_string(It->second) + ")");
      }
      Seen[Repr] = i;
    }
  }

  // TODO: The "items" field can also be an array where each element corresponds
  //       to each element in this array.
  if (auto Items = getObjectField(Schema, "items")) {
    for (size_t i = 0; i < Arr.size(); ++i) {
      std::string ElemPath = formatv("{0}[{1}]", Path, i).str();
      if (auto Err = validateValue(Arr[i], **Items, ElemPath))
        return Err;
    }
  }

  // TODO: Support prefixItems and contains

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
