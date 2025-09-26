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

/// \class JSONSchemaValidator
/// \brief Validate a JSON value against a practical subset of JSON Schema
/// (draft 2020-12).
///
/// This validator checks an \c llvm::json::Value against a schema represented as
/// an \c llvm::json::Object. It returns \c llvm::Error::success() when the value
/// conforms to the schema, or an explanatory \c llvm::Error on the first failure.
/// Errors carry human-readable messages that include a normalized instance path:
/// - Root is rendered as \c "$"
/// - Properties use dot notation (e.g. \c "$.config.name")
/// - Array indices use brackets (e.g. \c "$.items[0]")
///
/// ### Supported keywords (draft 2020-12 subset)
/// **Core**
///  - \c $ref (local only, \c "#/…"), \c $defs
///
/// **Types & constraints**
///  - \c type — single string or array of strings (union). Supported kinds:
///    \c "object", \c "array", \c "string", \c "number", \c "integer",
///    \c "boolean", \c "null"
///  - \c const, \c enum
///
/// **Numbers**
///  - \c minimum, \c maximum
///  - \c exclusiveMinimum, \c exclusiveMaximum (both boolean and numeric forms)
///  - \c multipleOf
///
/// **Strings**
///  - \c minLength, \c maxLength
///  - \c pattern (validated with \c llvm::Regex)
///
/// **Objects**
///  - \c properties, \c required
///  - \c additionalProperties (boolean or schema)
///  - \c patternProperties, \c propertyNames
///  - \c dependentRequired, \c dependentSchemas
///  - \c minProperties, \c maxProperties
///
/// **Arrays**
///  - \c items (single schema) and tuple form (\c items: [s0, s1, …])
///  - \c prefixItems
///  - \c minItems, \c maxItems, \c uniqueItems
///  - \c contains with \c minContains / \c maxContains
///
/// **Combinators & conditionals**
///  - \c allOf, \c anyOf, \c oneOf, \c not, \c if / \c then / \c else
///
/// ### Error handling
/// This class uses \c llvm::Error (no exceptions). Callers must either return
/// errors or consume them. Internally, when a subschema is probed (e.g. within
/// \c anyOf or \c contains), failures are explicitly consumed to avoid
/// unhandled-error aborts.
///
/// ### Design notes
///  - Path reporting is normalized via a small helper so user-facing messages
///    always start at \c "$" (e.g. \c "$.field[2]").
///  - Validation short-circuits on the first failing keyword at a path.
///  - The validator clears its internal \c RefCache at the start of each
///    top-level \c validate() call. The class is not thread-safe if shared
///    across concurrent validations.
///
/// ### Current limitations (intentional)
///  - **Unevaluated tracking**: \c unevaluatedProperties / \c unevaluatedItems
///    are not implemented (they require bookkeeping of “evaluated” regions
///    across combinators).
///  - **Reference system**: Only local \c $ref pointers (\c "#/…") are resolved.
///    Remote references, \c $id / \c $anchor, and \c $dynamicRef / \c $dynamicAnchor
///    are out of scope.
///  - **Formats & content**: \c format (e.g. date-time, uri) and content keywords
///    (\c contentEncoding, \c contentMediaType, \c contentSchema) are not enforced.
///  - **Regex dialect**: \c pattern uses \c llvm::Regex (POSIX-like); this is not a
///    full ECMA-262 implementation, so advanced JS regex features may behave
///    differently.
///
/// ### Example
/// \code
/// using namespace llvm::json;
///
/// // Schema
/// Object Schema{
///   {"type", "object"},
///   {"properties", Object{
///     {"name", Object{{"type", "string"}, {"minLength", 1}}},
///     {"count", Object{{"type", "integer"}, {"minimum", 0}}}
///   }},
///   {"required", Array{"name"}},
///   {"additionalProperties", false}
/// };
///
/// // Instance
/// Object Doc{{"name", "demo"}, {"count", 3}};
///
/// JSONSchemaValidator V(Schema);
/// if (Error Err = V.validate(Value(Doc))) {
///   // On error: pretty path is included (e.g. "$.count")
///   logAllUnhandledErrors(std::move(Err), errs(), "schema error: ");
/// }
/// \endcode
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
