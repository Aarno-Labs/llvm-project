//===- JSONSchemaValidatorTest.cpp ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "llvm/Support/JSONSchemaValidator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::json;

namespace {

std::string takeErrorString(Error &&E) {
  std::string S;
  raw_string_ostream OS(S);
  if (!E)
    return std::string();
  handleAllErrors(std::move(E), [&](const ErrorInfoBase &EI) { EI.log(OS); });
  OS.flush();
  return S;
}

void ExpectValid(const Object &Schema, const Value &Instance) {
  JSONSchemaValidator V(Schema);
  Error Err = V.validate(Instance);
  if (Err) {
    ADD_FAILURE() << "Expected VALID but got error: "
                  << takeErrorString(std::move(Err));
  }
}

void ExpectInvalid(const Object &Schema, const Value &Instance,
                   StringRef Expected = "") {
  JSONSchemaValidator V(Schema);
  Error Err = V.validate(Instance);
  if (!Err) {
    ADD_FAILURE() << "Expected INVALID but got success";
    return;
  }
  if (!Expected.empty()) {
    std::string Actual = takeErrorString(std::move(Err));
    if (Expected != Actual) {
      ADD_FAILURE() << "Error message did not match expected substring.\n"
                    << "Expected: " << Expected << "\n"
                    << "Actual: " << Actual;
    }
  } else {
    consumeError(std::move(Err));
  }
}

TEST(JSONSchemaValidatorTest, TypePrimitives) {
  Object S;

  // string
  S = Object{{"type", "string"}};
  ExpectValid(S, "hello");
  ExpectInvalid(S, 42.0, "Expected string at $");

  // number
  S = Object{{"type", "number"}};
  ExpectValid(S, 1.5);
  ExpectValid(S, 1);
  ExpectInvalid(S, "x", "Expected number at $");

  // integer
  S = Object{{"type", "integer"}};
  ExpectValid(S, 3);
  ExpectInvalid(S, 3.14, "Expected integer at $");

  // boolean
  S = Object{{"type", "boolean"}};
  ExpectValid(S, true);
  ExpectInvalid(S, 0, "Expected boolean at $");

  // object
  S = Object{{"type", "object"},
             {"properties", Object{{"a", Object{{"type", "number"}}}}}};
  ExpectValid(S, Object{{"a", 1.0}});
  ExpectInvalid(S, Array{1, 2, 3}, "Expected object at $");

  // array
  S = Object{{"type", "array"}};
  ExpectValid(S, Array{});
  ExpectInvalid(S, Object{}, "Expected array at $");
}

TEST(JSONSchemaValidatorTest, ConstAndEnum) {
  Object S;

  // const
  S = Object{{"const", 7}};
  ExpectValid(S, 7);
  ExpectInvalid(S, 8, "Value 8 does not equal const at $ (expected 7)");

  // enum (strings)
  S = Object{{"enum", Array{"red", "green", "blue"}}};
  ExpectValid(S, "red");
  ExpectInvalid(
      S, "cyan",
      "Value \"cyan\" not in enum list at $: [\"red\", \"green\", \"blue\"]");

  // enum (mixed types)
  S = Object{{"enum", Array{nullptr, 0, "none"}}};
  ExpectValid(S, nullptr);
  ExpectValid(S, 0);
  ExpectValid(S, "none");
  ExpectInvalid(S, false,
                "Value false not in enum list at $: [null, 0, \"none\"]");

  // enum on object
  S = Object{{"enum", Array{Object{{"k", "A"}}, Object{{"k", "B"}}}}};
  ExpectValid(S, Object{{"k", "A"}});
  ExpectInvalid(S, Object{{"k", "C"}},
                "Value {\"k\":\"C\"} not in enum list at $: [{\"k\":\"A\"}, "
                "{\"k\":\"B\"}]");
}

TEST(JSONSchemaValidatorTest, NumericConstraints) {
  Object S;

  // inclusive minimum/maximum
  S = Object{{"type", "number"}, {"minimum", 0.0}, {"maximum", 10.0}};
  ExpectValid(S, 0);
  ExpectValid(S, 10);
  ExpectInvalid(S, -1, "Number at $: got -1.00, requires >= 0.00 (minimum)");
  ExpectInvalid(S, 11, "Number at $: got 11.00, requires <= 10.00 (maximum)");

  // numeric exclusiveMinimum / exclusiveMaximum
  S = Object{
      {"type", "number"}, {"exclusiveMinimum", 0.0}, {"exclusiveMaximum", 1.0}};
  ExpectInvalid(S, 0,
                "Number at $: got 0.00, requires > 0.00 (exclusiveMinimum)");
  ExpectInvalid(S, 1,
                "Number at $: got 1.00, requires < 1.00 (exclusiveMaximum)");
  ExpectValid(S, 0.5);

  // boolean exclusiveMinimum / exclusiveMaximum with paired bounds
  S = Object{{"type", "number"}, {"minimum", 0.0}, {"exclusiveMinimum", true}};
  ExpectInvalid(S, 0,
                "Number at $: got 0.00, requires > 0.00 (minimum + "
                "exclusiveMinimum=true)"); // boundary should fail
  ExpectValid(S, 0.1);

  S = Object{{"type", "number"}, {"maximum", 10.0}, {"exclusiveMaximum", true}};
  ExpectInvalid(S, 10,
                "Number at $: got 10.00, requires < 10.00 (maximum + "
                "exclusiveMaximum=true)"); // boundary should fail
  ExpectValid(S, 9.9);

  // multipleOf (basic)
  S = Object{{"type", "number"}, {"multipleOf", 0.5}};
  ExpectValid(S, 2.0);
  ExpectInvalid(S, 2.1, "Number at $: got 2.10, requires a multiple of 0.50");

  // boolean exclusiveMinimum=true WITHOUT minimum -> schema error
  S = Object{{"type", "number"}, {"exclusiveMinimum", true}};
  ExpectInvalid(S, 123,
                "Schema error at $: exclusiveMinimum=true requires 'minimum'");

  // boolean exclusiveMaximum=true WITHOUT maximum -> schema error
  S = Object{{"type", "number"}, {"exclusiveMaximum", true}};
  ExpectInvalid(S, 123,
                "Schema error at $: exclusiveMaximum=true requires 'maximum'");

  // multipleOf must be > 0 (schema errors)
  S = Object{{"type", "number"}, {"multipleOf", 0.0}};
  ExpectInvalid(S, 0.0, "Schema error at $: multipleOf must be > 0 (got 0.00)");
  S = Object{{"type", "number"}, {"multipleOf", -1.0}};
  ExpectInvalid(S, 0.0,
                "Schema error at $: multipleOf must be > 0 (got -1.00)");

  // multipleOf: FP tolerance near a multiple (should pass)
  S = Object{{"type", "number"}, {"multipleOf", 0.1}};
  ExpectValid(S, 0.3 + 1e-13); // 0.3000000000001 ~ 3 * 0.1

  // multipleOf: negative numbers (normalization)
  S = Object{{"type", "number"}, {"multipleOf", 2.0}};
  ExpectValid(S, -4.0);
  ExpectInvalid(S, -4.1, "Number at $: got -4.10, requires a multiple of 2.00");

  // Sanity: integer path still applies number constraints
  S = Object{{"type", "integer"}, {"minimum", 0.0}, {"maximum", 2.0}};
  ExpectValid(S, 2);
  ExpectInvalid(S, 3, "Number at $: got 3.00, requires <= 2.00 (maximum)");
}

TEST(JSONSchemaValidatorTest, StringConstraints) {
  Object S;

  // minLength / maxLength (inclusive bounds)
  S = Object{{"type", "string"}, {"minLength", 2}, {"maxLength", 4}};
  ExpectValid(S, "ab");   // on min
  ExpectValid(S, "abcd"); // on max
  ExpectInvalid(S, "a", "String at $: length 1 < minLength 2 (value: \"a\")");
  ExpectInvalid(S, "abcde",
                "String at $: length 5 > maxLength 4 (value: \"abcde\")");

  // pattern (valid / invalid)
  S = Object{{"type", "string"}, {"pattern", "^[A-Z]{3}[0-9]$"}};
  ExpectValid(S, "ABC1");
  ExpectInvalid(
      S, "abc1",
      "String at $: value \"abc1\" does not match pattern \"^[A-Z]{3}[0-9]$\"");
  ExpectInvalid(S, "ABCD1",
                "String at $: value \"ABCD1\" does not match pattern "
                "\"^[A-Z]{3}[0-9]$\"");

  // minLength: 0 allows empty string
  S = Object{{"type", "string"}, {"minLength", 0}};
  ExpectValid(S, "Schema error at $: minLength must be >= 0 (got -1)");

  // Negative minLength / maxLength => schema errors
  S = Object{{"type", "string"}, {"minLength", -1}};
  ExpectInvalid(S, "x", "Schema error at $: minLength must be >= 0 (got -1)");

  S = Object{{"type", "string"}, {"maxLength", -1}};
  ExpectInvalid(S, "x", "Schema error at $: maxLength must be >= 0 (got -1)");

  // Invalid regex in 'pattern' => schema error
  S = Object{{"type", "string"}, {"pattern", "("}}; // unbalanced
  ExpectInvalid(S, "anything",
                "Schema error at $: invalid regex for 'pattern' \"(\": "
                "parentheses not balanced");

  // Long string (>100) should be elided in error messages ("...")
  // Use maxLength to force a violation and thus print the value.
  std::string longStr(120, 'x'); // 120 'x'
  S = Object{{"type", "string"}, {"maxLength", 5}};
  ExpectInvalid(S, longStr,
                "String at $: length 120 > maxLength 5 (value: "
                "\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx...\")");

  // Pattern mismatch with a long string also exercises elision on pattern
  // errors
  S = Object{{"type", "string"}, {"pattern", "^[A-Z]+$"}};
  ExpectInvalid(
      S, longStr,
      "String at $: value "
      "\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
      "xxxxxxxxxxxxxxxxxxxxxxxxxxx...\" does not match pattern \"^[A-Z]+$\"");
}

TEST(JSONSchemaValidatorTest, ObjectPropertiesRequiredAdditional) {
  // required + properties + additionalProperties=false
  Object S = Object{{"type", "object"},
                    {"required", Array{"a", "b"}},
                    {"properties", Object{{"a", Object{{"type", "number"}}},
                                          {"b", Object{{"type", "string"}}}}},
                    {"additionalProperties", false}};

  ExpectValid(S, Object{{"a", 1.0}, {"b", "x"}});
  ExpectInvalid(S, Object{{"a", 1.0}}, "Missing required field: $.b");
  ExpectInvalid(S, Object{{"a", 1.0}, {"b", "x"}, {"c", 1}},
                "Unexpected property: $.c");
  ExpectInvalid(S, Object{{"a", 1.0}, {"b", 2.0}}, "Expected string at $.b");
}

TEST(JSONSchemaValidatorTest, PatternPropertiesAndPropertyNames) {
  // patternProperties + propertyNames + additionalProperties=false
  Object S =
      Object{{"type", "object"},
             {"patternProperties", Object{{"^x_", Object{{"type", "number"}}}}},
             {"propertyNames",
              Object{{"type", "string"}, {"pattern", "^[a-z_][a-z0-9_]*$"}}},
             {"additionalProperties", false}};

  // ok: matches patternProperties
  ExpectValid(S, Object{{"x_foo", 1.0}});
  // bad: property name fails propertyNames schema
  ExpectInvalid(S, Object{{"X_BAD", 1}},
                "String at $.<propertyName>: value \"X_BAD\" does not match "
                "pattern \"^[a-z_][a-z0-9_]*$\"");
  // bad: unknown key not matching patternProperties and addl=false
  ExpectInvalid(S, Object{{"foo", 1}}, "Unexpected property: $.foo");
}

TEST(JSONSchemaValidatorTest, AdditionalPropertiesAsSchema) {
  Object S = Object{{"type", "object"},
                    {"properties", Object{{"a", Object{{"type", "number"}}}}},
                    {"additionalProperties", Object{{"type", "string"}}}};

  // 'a' must be number
  ExpectInvalid(S, Object{{"a", "nope"}}, "Expected number at $.a");
  // unknown keys validated against schema (must be string)
  ExpectValid(S, Object{{"b", "ok"}});
  ExpectInvalid(S, Object{{"b", 1}}, "Expected string at $.b");
}

TEST(JSONSchemaValidatorTest, DependentRequiredAndSchemas) {
  // dependentRequired
  Object S = Object{{"type", "object"},
                    {"dependentRequired", Object{{"a", Array{"b", "c"}}}}};
  ExpectValid(S, Object{{"a", 1}, {"b", 2}, {"c", 3}});
  ExpectInvalid(
      S, Object{{"a", 1}, {"b", 2}},
      "Missing dependentRequired field 'c' because 'a' is present at $");

  // dependentSchemas: if 'flag' present, object must have 'x' a number
  S = Object{
      {"type", "object"},
      {"dependentSchemas",
       Object{{"flag",
               Object{{"type", "object"},
                      {"properties", Object{{"x", Object{{"type", "number"}}}}},
                      {"required", Array{"x"}}}}}}};
  ExpectValid(S, Object{{"flag", true}, {"x", 1.0}});
  ExpectInvalid(S, Object{{"flag", true}, {"x", "invalid"}},
                "Expected number at $.x");
  ExpectInvalid(S, Object{{"flag", true}}, "Missing required field: $.x");
}

TEST(JSONSchemaValidatorTest, Combinators) {
  // allOf
  Object S =
      Object{{"allOf", Array{Object{{"type", "number"}, {"minimum", 0.0}},
                             Object{{"type", "number"}, {"maximum", 1.0}}}}};
  ExpectValid(S, 0.5);
  ExpectInvalid(S, -1, "Number at $: got -1.00, requires >= 0.00 (minimum)");

  // anyOf
  S = Object{{"anyOf", Array{Object{{"const", 1}}, Object{{"const", 2}}}}};
  ExpectValid(S, 1);
  ExpectValid(S, 2);
  ExpectInvalid(S, 3, "anyOf failed at $: no alternative matched");

  // oneOf
  S = Object{{"oneOf", Array{Object{{"const", 1}}, Object{{"const", 2}}}}};
  ExpectValid(S, 1);
  ExpectInvalid(S, 3, "oneOf failed at $: expected exactly 1 match, got 0");

  // not
  S = Object{{"not", Object{{"type", "number"}}}};
  ExpectValid(S, "str");
  ExpectInvalid(S, 1, "not failed at $: instance unexpectedly matched");

  // if/then/else
  S = Object{
      {"if", Object{{"type", "object"},
                    {"properties", Object{{"k", Object{{"const", "A"}}}}}}},
      {"then", Object{{"type", "object"}, {"required", Array{"v"}}}},
      {"else", Object{{"type", "object"}, {"required", Array{"w"}}}}};
  ExpectValid(S, Object{{"k", "A"}, {"v", 1}});
  ExpectValid(S, Object{{"k", "B"}, {"w", 1}});
  ExpectInvalid(S, Object{{"k", "A"}}, "Missing required field: $.v");
  ExpectInvalid(S, Object{{"k", "B"}}, "Missing required field: $.w");
}

TEST(JSONSchemaValidatorTest, Arrays_Items_Prefix_Contains_Unique_Min) {
  // items (single schema) + minItems + uniqueItems
  Object S = Object{{"type", "array"},
                    {"items", Object{{"type", "integer"}}},
                    {"minItems", 2},
                    {"uniqueItems", true}};
  ExpectValid(S, Array{1, 2});
  ExpectInvalid(S, Array{1}, "Array at $: requires >= 2 items (got 1)");
  ExpectInvalid(S, Array{1, 1},
                "Duplicate array item at $[1]: 1 (already seen at index 0)");

  // tuple validation via items array
  S = Object{
      {"type", "array"},
      {"items", Array{Object{{"type", "string"}}, Object{{"type", "number"}}}}};
  ExpectValid(S, Array{"a", 1.0});
  ExpectInvalid(S, Array{1.0, "a"}, "Expected string at $[0]");
  ExpectInvalid(S, Array{"a", "b"}, "Expected number at $[1]");

  // prefixItems (first two elements string, number), rest unconstrained
  S = Object{{"type", "array"},
             {"prefixItems",
              Array{Object{{"type", "string"}}, Object{{"type", "number"}}}}};
  ExpectValid(S, Array{"ok", 2.0, true, nullptr});
  ExpectInvalid(S, Array{3, 2.0}, "Expected string at $[0]");

  // prefixItems + items (draft-2020-12): "items" applies only *after* the
  // prefix tuple, not to the prefix elements themselves.
  S = Object{{"type", "array"},
             {"prefixItems", Array{Object{{"type", "string"}}}},
             {"items", Object{{"type", "number"}}}};
  ExpectValid(S, Array{"s"});
  ExpectValid(S, Array{"s", 1.0, 2.0});
  ExpectInvalid(S, Array{"s", "t"}, "Expected number at $[1]");
  ExpectInvalid(S, Array{1.0, 2.0}, "Expected string at $[0]");

  // prefixItems + legacy tuple-form items array: when prefixItems is present,
  // do not interpret items:[...] (to avoid double-applying tuple constraints).
  S = Object{{"type", "array"},
             {"prefixItems", Array{Object{{"type", "string"}}}},
             {"items", Array{Object{{"type", "number"}}}}};
  ExpectValid(S, Array{"s"});
  ExpectValid(S, Array{"s", "t"});
  ExpectValid(S, Array{"s", 1.0});
  ExpectInvalid(S, Array{1.0}, "Expected string at $[0]");

  // contains: at least one element is > 10
  S = Object{{"type", "array"},
             {"contains", Object{{"type", "number"}, {"minimum", 11.0}}}};
  ExpectValid(S, Array{1, 11, 2});
  ExpectInvalid(S, Array{1, 2, 3},
                "Array at $: requires >= 1 matches for 'contains' (got 0)");
}


TEST(JSONSchemaValidatorTest, Arrays_UniqueItems_StructuralEquality) {
  Object Schema = Object{{"type", "array"}, {"uniqueItems", true}};

  // Numbers compare by numeric value, not by textual representation.
  // In particular, -0 and 0 are equal and must be rejected as duplicates.
  ExpectInvalid(Schema, Array{0.0, -0.0});

  // Objects compare structurally; key order must not matter.
  ExpectInvalid(Schema, Array{Object{{"a", 1}, {"b", 2}},
                             Object{{"b", 2}, {"a", 1}}});

  // Nested arrays/objects also compare structurally.
  ExpectInvalid(Schema, Array{Object{{"x", Object{{"a", 1}, {"b", 2}}}},
                             Object{{"x", Object{{"b", 2}, {"a", 1}}}}});

  // Arrays are ordered, so these are distinct.
  ExpectValid(Schema, Array{Array{1, 2}, Array{2, 1}});

  // Distinct objects are allowed.
  ExpectValid(Schema, Array{Object{{"a", 1}}, Object{{"a", 2}}});
}

TEST(JSONSchemaValidatorTest, RefAndDefs_Local) {
  Object Schema = Object{
      {"$defs",
       Object{{"Num", Object{{"type", "number"}, {"minimum", 0.0}}},
              {"Pair", Object{{"type", "array"},
                              {"items", Array{Object{{"$ref", "#/$defs/Num"}},
                                              Object{{"$ref", "#/$defs/Num"}}}},
                              {"minItems", 2}}}}},
      {"$ref", "#/$defs/Pair"}};

  ExpectValid(Schema, Array{0.0, 1.5});
  ExpectInvalid(Schema, Array{-1.0, 1.5},
                "Number at $[0]: got -1.00, requires >= 0.00 (minimum)");
}

TEST(JSONSchemaValidatorTest, TypeUnionAndNull) {
  Object S = Object{{"type", Array{"string", "null"}}};
  ExpectValid(S, "ok");
  ExpectValid(S, nullptr);
  ExpectInvalid(S, 1,
                "Type mismatch at $: value 1 has runtime type \"integer\", but "
                "schema 'type' allows only [string, null]");
}

TEST(JSONSchemaValidatorTest, ObjectMinMaxProperties) {
  Object S =
      Object{{"type", "object"}, {"minProperties", 2}, {"maxProperties", 3}};
  ExpectInvalid(S, Object{{"a", 1}},
                "Object at $: requires >= 2 properties (got 1)");
  ExpectInvalid(S, Object{{"a", 1}, {"b", 2}, {"c", 3}, {"d", 4}},
                "Object at $: requires <= 3 properties (got 4)");
}



TEST(JSONSchemaValidatorTest, RefHonorsSiblingKeywords) {
  Object S = Object{
      {"$defs", Object{{"Base", Object{{"type", "number"}, {"minimum", 0.0}}}}},
      {"$ref", "#/$defs/Base"},
      {"maximum", 10.0},
  };

  ExpectValid(S, 5.0);
  ExpectInvalid(S, 11.0, "Number at $: got 11.00, requires <= 10.00 (maximum)");
  ExpectInvalid(S, -1.0, "Number at $: got -1.00, requires >= 0.00 (minimum)");
}

TEST(JSONSchemaValidatorTest, OneOfHonorsSiblingKeywords) {
  Object S = Object{
      {"oneOf", Array{
                   Object{{"type", "number"}},
                   Object{{"type", "string"}},
               }},
      {"type", "number"},
      {"minimum", 5.0},
  };

  // Exactly one alternative matches, then sibling keywords are still enforced.
  ExpectValid(S, 6.0);
  ExpectInvalid(S, 4.0, "Number at $: got 4.00, requires >= 5.00 (minimum)");
  ExpectInvalid(S, "x", "Expected number at $");
}

TEST(JSONSchemaValidatorTest, TypelessSubschemaAppliesRuntimeConstraints) {
  // No explicit "type": object keywords must still apply when the instance is
  // an object.
  Object ObjSchema = Object{
      {"properties", Object{{"a", Object{{"type", "integer"}}}}},
      {"required", Array{"a"}},
  };

  ExpectValid(ObjSchema, Object{{"a", 1}});
  ExpectInvalid(ObjSchema, Object{}, "Missing required field: $.a");
  ExpectInvalid(ObjSchema, Object{{"a", "x"}}, "Expected integer at $.a");

  // Object-only keywords do not apply to non-objects.
  ExpectValid(ObjSchema, "not-an-object");

  // No explicit "type": array keywords must still apply when the instance is
  // an array.
  Object ArrSchema = Object{
      {"minItems", 2},
      {"items", Object{{"type", "string"}}},
  };

  ExpectValid(ArrSchema, Array{"a", "b"});
  ExpectInvalid(ArrSchema, Array{"a"},
                "Array at $: requires >= 2 items (got 1)");
  ExpectInvalid(ArrSchema, Array{1, 2}, "Expected string at $[0]");
}

TEST(JSONSchemaValidatorTest, ArrayMaxItemsAndMinMaxContains) {
  Object S = Object{{"type", "array"},
                    {"contains", Object{{"type", "integer"}}},
                    {"minContains", 2},
                    {"maxContains", 3}};
  ExpectInvalid(S, Array{1},
                "Array at $: requires >= 2 matches for 'contains' (got 1)");
  ExpectValid(S, Array{1, 2});
  ExpectInvalid(S, Array{1, 2, 3, 4},
                "Array at $: requires <= 3 matches for 'contains' (got 4)");
}

TEST(JSONSchemaValidatorTest, RefPointerUnescape) {
  // Requires RFC 6901 unescape (~1 => '/', ~0 => '~').
  Object S = Object{{"$defs", Object{{"a~1b", Object{{"const", 1}}}}},
                    {"$ref", "#/$defs/a~01b"}};
  ExpectValid(S, 1);
}

TEST(JSONSchemaValidatorTest, RefIdAndAnchorSameDocument) {
  // Root-level $anchor: "#<anchor>" may resolve to the document root schema.
  {
    Object S =
        Object{{"$anchor", "RootA"},
               {"minimum", 0},
               {"properties", Object{{"x", Object{{"$ref", "#RootA"}}}}}};
    ExpectValid(S, Object{{"x", 1}});
    ExpectInvalid(S, Object{{"x", -1}});
  }

  // #<anchor> via $anchor (same-document).
  {
    Object S =
        Object{{"$defs", Object{{"T", Object{{"$anchor", "A"}, {"const", 1}}}}},
               {"$ref", "#A"}};
    ExpectValid(S, 1);
    ExpectInvalid(S, 2);
  }

  // $id base selection (<id>).
  {
    Object S = Object{{"$defs", Object{{"Base", Object{{"$id", "urn:ex:base"},
                                                       {"type", "integer"},
                                                       {"minimum", 10}}}}},
                      {"$ref", "urn:ex:base"}};
    ExpectValid(S, 10);
    ExpectInvalid(S, 9);
  }

  // $id + JSON Pointer fragment (<id>#/...)
  {
    Object S = Object{
        {"$defs",
         Object{
             {"Base", Object{{"$id", "urn:ex:base"},
                             {"$defs", Object{{"T", Object{{"const", 5}}}}}}}}},
        {"$ref", "urn:ex:base#/$defs/T"}};
    ExpectValid(S, 5);
    ExpectInvalid(S, 6);
  }

  // $id + anchor fragment (<id>#<anchor>).
  {
    Object S = Object{
        {"$defs",
         Object{
             {"Base", Object{{"$id", "urn:ex:base"},
                             {"$defs", Object{{"T", Object{{"$anchor", "Z"},
                                                           {"const", 7}}}}}}}}},
        {"$ref", "urn:ex:base#Z"}};
    ExpectValid(S, 7);
    ExpectInvalid(S, 8);
  }

  // Unknown $id should fail (no remote fetch).
  {
    Object S = Object{{"$ref", "urn:ex:unknown#/$defs/T"}};
    ExpectInvalid(S, 1);
  }

  // Invalid pointer within a known $id should fail.
  {
    Object S =
        Object{{"$defs",
                Object{{"Base", Object{{"$id", "urn:ex:base"}, {"const", 1}}}}},
               {"$ref", "urn:ex:base#/no_such_token"}};
    ExpectInvalid(S, 1);
  }
}

TEST(JSONSchemaValidatorTest, BooleanSubschemasInCombinatorsAndConditionals) {
  // allOf with a boolean 'false' subschema must fail.
  Object AllOfFalse = Object{{"allOf", Array{true, false}}};
  ExpectInvalid(AllOfFalse, 1, "Schema 'false' rejects instance at $");

  // anyOf: one 'true' subschema is sufficient.
  Object AnyOfTF = Object{{"anyOf", Array{false, true}}};
  ExpectValid(AnyOfTF, 1);

  // oneOf: boolean subschemas participate like normal subschemas.
  Object OneOfOK = Object{{"oneOf", Array{true, false}}};
  ExpectValid(OneOfOK, 1);

  Object OneOfTooMany = Object{{"oneOf", Array{true, true}}};
  ExpectInvalid(OneOfTooMany, 1);

  // not: boolean subschemas invert as expected.
  Object NotTrue = Object{{"not", true}};
  ExpectInvalid(NotTrue, 1);

  Object NotFalse = Object{{"not", false}};
  ExpectValid(NotFalse, 1);

  // if/then/else: boolean subschemas in branches are honored.
  Object IfTrueThenFalse = Object{{"if", true}, {"then", false}};
  ExpectInvalid(IfTrueThenFalse, 1, "Schema 'false' rejects instance at $");

  Object IfFalseElseFalse = Object{{"if", false}, {"else", false}};
  ExpectInvalid(IfFalseElseFalse, 1, "Schema 'false' rejects instance at $");

  Object IfFalseElseTrue = Object{{"if", false}, {"else", true}};
  ExpectValid(IfFalseElseTrue, 1);
}

TEST(JSONSchemaValidatorTest, BooleanSubschemasInObjectAndArrayApplicators) {
  // properties: boolean 'false' rejects the property value when present.
  Object PropsFalse =
      Object{{"type", "object"}, {"properties", Object{{"a", false}}}};
  ExpectValid(PropsFalse, Object{});
  ExpectValid(PropsFalse, Object{{"b", 1}});
  ExpectInvalid(PropsFalse, Object{{"a", 1}},
               "Schema 'false' rejects instance at $.a");

  // patternProperties: boolean 'false' rejects matching property values.
  Object PatFalse =
      Object{{"type", "object"}, {"patternProperties", Object{{"^a", false}}}};
  ExpectInvalid(PatFalse, Object{{"abc", 1}},
               "Schema 'false' rejects instance at $.abc");

  // propertyNames: boolean 'false' rejects any non-empty object
  // (vacuously ok for {}).
  // {}).
  Object NamesFalse = Object{{"type", "object"}, {"propertyNames", false}};
  ExpectValid(NamesFalse, Object{});
  ExpectInvalid(NamesFalse, Object{{"a", 1}});

  // dependentSchemas: boolean subschema is applied when the triggering property
  // exists.
  Object DepFalse =
      Object{{"type", "object"}, {"dependentSchemas", Object{{"a", false}}}};
  ExpectValid(DepFalse, Object{{"b", 1}});
  ExpectInvalid(DepFalse, Object{{"a", 1}});

  // items: boolean 'false' rejects any element.
  Object ItemsFalse = Object{{"type", "array"}, {"items", false}};
  ExpectValid(ItemsFalse, Array{});
  ExpectInvalid(ItemsFalse, Array{1},
               "Schema 'false' rejects instance at $[0]");

  // prefixItems: boolean 'false' rejects that tuple position.
  Object PrefixFalse =
      Object{{"type", "array"}, {"prefixItems", Array{false}}, {"items", true}};
  ExpectValid(PrefixFalse, Array{});
  ExpectInvalid(PrefixFalse, Array{1},
               "Schema 'false' rejects instance at $[0]");

  // contains: boolean subschemas are evaluated for each array element.
  Object ContainsFalse = Object{{"type", "array"}, {"contains", false}};
  ExpectInvalid(ContainsFalse, Array{1});

  Object ContainsTrue = Object{{"type", "array"}, {"contains", true}};
  ExpectValid(ContainsTrue, Array{1});
}


TEST(JSONSchemaValidatorTest, StringLengthCountsUnicodeCodePoints) {
  // U+00E9 ('é') is 2 bytes in UTF-8; U+1F60A ('😊') is 4 bytes.
  // Total: 2 code points, 6 bytes.
  const std::string TwoCodePoints = "\xC3\xA9\xF0\x9F\x98\x8A";

  // maxLength must be enforced in code points (so this is valid at maxLength=2).
  Object Max2 = Object{{"type", "string"}, {"maxLength", 2}};
  ExpectValid(Max2, Value(TwoCodePoints));

  // maxLength violation reports code point length.
  Object Max1 = Object{{"type", "string"}, {"maxLength", 1}};
  ExpectInvalid(Max1, Value(TwoCodePoints),
                "String at $: length 2 > maxLength 1 (value: \"\xC3\xA9\xF0\x9F\x98\x8A\")");

  // minLength violation also reports code point length.
  Object Min3 = Object{{"type", "string"}, {"minLength", 3}};
  ExpectInvalid(Min3, Value(TwoCodePoints),
                "String at $: length 2 < minLength 3 (value: \"\xC3\xA9\xF0\x9F\x98\x8A\")");

}


TEST(JSONSchemaValidatorTest, StringFormats) {
  // uuid
  ExpectValid(Object{{"type", "string"}, {"format", "uuid"}},
              "123e4567-e89b-12d3-a456-426614174000");
  ExpectInvalid(
      Object{{"type", "string"}, {"format", "uuid"}},
      "123e4567e89b12d3a456426614174000",
      "String at $: value \"123e4567e89b12d3a456426614174000\" does not match "
      "format \"uuid\"");

  // email
  ExpectValid(Object{{"type", "string"}, {"format", "email"}}, "a@b.com");
  ExpectInvalid(Object{{"type", "string"}, {"format", "email"}}, "a@b",
                "String at $: value \"a@b\" does not match format \"email\"");

  // uri
  ExpectValid(Object{{"type", "string"}, {"format", "uri"}},
              "https://example.com/path");
  ExpectInvalid(Object{{"type", "string"}, {"format", "uri"}}, "example.com",
                "String at $: value \"example.com\" does not match format \"uri\"");

  // hostname
  ExpectValid(Object{{"type", "string"}, {"format", "hostname"}}, "example.com");
  ExpectInvalid(
      Object{{"type", "string"}, {"format", "hostname"}}, "exa_mple.com",
      "String at $: value \"exa_mple.com\" does not match format \"hostname\"");

  // date
  ExpectValid(Object{{"type", "string"}, {"format", "date"}}, "2024-02-29");
  ExpectInvalid(Object{{"type", "string"}, {"format", "date"}}, "2023-02-29",
                "String at $: value \"2023-02-29\" does not match format \"date\"");

  // time
  ExpectValid(Object{{"type", "string"}, {"format", "time"}}, "23:59:59Z");
  ExpectValid(Object{{"type", "string"}, {"format", "time"}},
              "12:34:56.789+01:00");
  ExpectInvalid(Object{{"type", "string"}, {"format", "time"}}, "23:59:59",
                "String at $: value \"23:59:59\" does not match format \"time\"");

  // date-time
  ExpectValid(Object{{"type", "string"}, {"format", "date-time"}},
              "2024-02-29T23:59:59Z");
  ExpectInvalid(
      Object{{"type", "string"}, {"format", "date-time"}}, "2024-02-29T23:59:59",
      "String at $: value \"2024-02-29T23:59:59\" does not match format "
      "\"date-time\"");

  // Unknown formats are ignored (no-op).
  ExpectValid(Object{{"type", "string"}, {"format", "does-not-exist"}}, "anything");

  // "format" is only enforced for string instances.
  ExpectValid(Object{{"format", "uuid"}}, 42);
}



TEST(JSONSchemaValidatorTest, UnevaluatedProperties_LocalOnly) {
  // Local-only semantics: properties/patternProperties/additionalProperties
  // mark evaluated properties; unevaluatedProperties applies to the remainder.

  Object S1 = Object{
      {"type", "object"},
      {"properties", Object{{"a", Object{{"type", "number"}}}}},
      {"unevaluatedProperties", false},
  };
  ExpectValid(S1, Object{{"a", 1.0}});
  ExpectInvalid(S1, Object{{"a", 1.0}, {"b", 2.0}},
                "Schema 'false' rejects instance at $.b");

  Object S1b = Object{
      {"type", "object"},
      {"properties", Object{{"a", Object{{"type", "number"}}}}},
      {"unevaluatedProperties", Object{{"type", "number"}}},
  };
  ExpectValid(S1b, Object{{"a", 1.0}, {"b", 2.0}});
  ExpectInvalid(S1b, Object{{"a", 1.0}, {"b", "x"}}, "Expected number at $.b");

  Object S2 = Object{
      {"type", "object"},
      {"patternProperties", Object{{"^x", Object{{"type", "number"}}}}},
      {"unevaluatedProperties", false},
  };
  ExpectValid(S2, Object{{"x1", 1.0}});
  ExpectInvalid(S2, Object{{"x1", 1.0}, {"y", 2.0}},
                "Schema 'false' rejects instance at $.y");

  Object S3 = Object{
      {"type", "object"},
      {"properties", Object{{"a", Object{{"type", "number"}}}}},
      {"additionalProperties", Object{{"type", "number"}}},
      {"unevaluatedProperties", false},
  };
  // "b" is evaluated by additionalProperties, so unevaluatedProperties does not
  // apply to it.
  ExpectValid(S3, Object{{"a", 1.0}, {"b", 2.0}});

  Object S4 = Object{
      {"type", "object"},
      {"allOf", Array{Object{{"type", "object"}}}},
      {"unevaluatedProperties", false},
  };
  // Reject schemas that would require cross-subschema annotation propagation.
  ExpectInvalid(
      S4, Object{},
      "Schema error at <root>: unevaluatedProperties requires cross-subschema "
      "evaluation tracking (not implemented)");
}

TEST(JSONSchemaValidatorTest, UnevaluatedItems_LocalOnly) {
  // Local-only semantics: prefixItems/items/contains mark evaluated indices;
  // unevaluatedItems applies to the remainder.

  Object S1 = Object{
      {"type", "array"},
      {"prefixItems",
       Array{Object{{"type", "number"}}, Object{{"type", "string"}}}},
      {"items", Object{{"type", "boolean"}}},
      {"unevaluatedItems", false},
  };
  // All indices are evaluated by prefixItems/items, so unevaluatedItems does
  // not apply.
  ExpectValid(S1, Array{1.0, "x", true, false});

  Object S2 = Object{
      {"type", "array"},
      {"prefixItems", Array{Object{{"type", "number"}}}},
      {"unevaluatedItems", false},
  };
  // Index 1 is not evaluated by prefixItems or items, so unevaluatedItems
  // applies.
  ExpectInvalid(S2, Array{1.0, 2.0}, "Schema 'false' rejects instance at $[1]");

  Object S2b = Object{
      {"type", "array"},
      {"prefixItems", Array{Object{{"type", "number"}}}},
      {"unevaluatedItems", Object{{"type", "number"}}},
  };
  ExpectValid(S2b, Array{1.0, 2.0});
  ExpectInvalid(S2b, Array{1.0, "x"}, "Expected number at $[1]");

  Object S3 = Object{
      {"type", "array"},
      // Legacy tuple items (array form) when prefixItems is absent.
      {"items", Array{Object{{"type", "number"}}}},
      {"unevaluatedItems", false},
  };
  ExpectValid(S3, Array{1.0});
  ExpectInvalid(S3, Array{1.0, "x"}, "Schema 'false' rejects instance at $[1]");

  Object S4 = Object{
      {"type", "array"},
      {"contains", Object{{"const", 1.0}}},
      {"unevaluatedItems", false},
  };
  // "contains" only evaluates matching indices; index 1 remains unevaluated.
  ExpectValid(S4, Array{1.0});
  ExpectInvalid(S4, Array{1.0, 2.0}, "Schema 'false' rejects instance at $[1]");

  Object S5 = Object{
      {"type", "array"},
      {"if", Object{}},
      {"then", Object{}},
      {"unevaluatedItems", false},
  };
  ExpectInvalid(
      S5, Array{},
      "Schema error at <root>: unevaluatedItems requires cross-subschema "
      "evaluation tracking (not implemented)");
}

} // namespace
