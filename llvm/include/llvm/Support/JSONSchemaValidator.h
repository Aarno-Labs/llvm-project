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
#ifndef LLVM_SUPPORT_JSONSCHEMAVALIDATOR_H
#define LLVM_SUPPORT_JSONSCHEMAVALIDATOR_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/JSON.h"
#include <optional>
#include <string>

namespace llvm {
namespace json {

/// Utility class for validating JSON files.
class JSONSchemaValidator {
  const Object &RootSchema;
  mutable DenseMap<StringRef, const Object *> RefCache;

public:
  JSONSchemaValidator(const Object &Schema) : RootSchema(Schema) {}

  /// Main call for validating Value `V`.
  Error validate(const Value &V) const;

private:
  Error validateValue(const Value &V, const Object &Schema,
                      std::string Path) const;
  Error validateObject(const Object &Obj, const Object &Schema,
                       std::string Path) const;
  Error validateArray(const Array &Arr, const Object &Schema,
                      std::string Path) const;

  std::optional<StringRef> getStringField(const Object &Obj,
                                          StringRef Key) const;
  std::optional<const Object *> getObjectField(const Object &Obj,
                                               StringRef Key) const;
  std::optional<const Array *> getArrayField(const Object &Obj,
                                             StringRef Key) const;
};

} // namespace json
} // namespace llvm

#endif // LLVM_SUPPORT_JSONSCHEMAVALIDATOR_H
