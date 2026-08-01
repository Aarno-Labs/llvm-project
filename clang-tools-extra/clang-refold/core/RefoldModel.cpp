//===--- RefoldModel.cpp ----------------------------------------*- C++ -*-===//
//
// This file defines the RefoldModel class — a structured in-memory
// representation of the "refold map" JSON emitted by the modified Clang
// preprocessor. The model captures the logical mapping between preprocessor
// constructs in the original source (A) and the corresponding constructs in the
// edited preprocessed stream (B).
//
// The RefoldModel serves as a stable, deterministic schema layer over the
// untyped JSON representation, providing strongly typed access to:
//   - Include items (#include, #include_next directives)
//   - Macro invocations (object- and function-like forms)
//   - Token-to-source span mappings and ownership relationships
//
// The model performs consistency validation during construction and exposes
// lightweight query methods for downstream consumers such as RefoldEngine.
//
// Author:
//   jeikenberry
//
//===----------------------------------------------------------------------===//

#include "core/RefoldModel.h"

#include "core/RefoldLog.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <cassert>
#include <functional>
#include <limits>
#include <optional>

using namespace llvm;

// ==================== Local JSON helpers (no exceptions) =====================

namespace {
using namespace clang::refold;

/// Look up a required JSON field and return a typed Error on absence.
Expected<const json::Value *> requireField(const json::Object &obj,
                                           StringRef key, StringRef ctx) {
  if (const json::Value *val = obj.get(key))
    return val;
  return createStringError(inconvertibleErrorCode(),
                           "Missing required field '%s' at %s",
                           key.str().c_str(), ctx.str().c_str());
}

/// Require a JSON value to be an object at the supplied diagnostic context.
Expected<const json::Object *> asObject(const json::Value &val, StringRef ctx) {
  if (auto *obj = val.getAsObject())
    return obj;
  return createStringError(inconvertibleErrorCode(), "Expected object at %s",
                           ctx.str().c_str());
}

/// Require a JSON value to be an array at the supplied diagnostic context.
Expected<const json::Array *> asArray(const json::Value &val, StringRef ctx) {
  if (auto *arr = val.getAsArray())
    return arr;
  return createStringError(inconvertibleErrorCode(), "Expected array at %s",
                           ctx.str().c_str());
}

/// Require a JSON value to be a string and return it as a StringRef view.
Expected<StringRef> asString(const json::Value &val, StringRef ctx) {
  if (auto str = val.getAsString())
    return *str;
  return createStringError(inconvertibleErrorCode(), "Expected string at %s",
                           ctx.str().c_str());
}

/// Require a JSON integer to fit in uint32_t.
Expected<uint32_t> asUInt32(const json::Value &val, StringRef ctx) {
  if (auto n = val.getAsUINT64()) {
    if (*n <= std::numeric_limits<uint32_t>::max())
      return static_cast<uint32_t>(*n);
  }
  return createStringError(inconvertibleErrorCode(), "Expected uint32_t at %s",
                           ctx.str().c_str());
}

/// Require a JSON integer to fit in uint64_t.
Expected<uint64_t> asUInt64(const json::Value &val, StringRef ctx) {
  if (auto n = val.getAsUINT64())
    return *n;
  return createStringError(inconvertibleErrorCode(), "Expected uint64_t at %s",
                           ctx.str().c_str());
}

/// Require a JSON value to be boolean.
Expected<bool> asBool(const json::Value &val, StringRef ctx) {
  if (auto b = val.getAsBoolean()) {
    return *b;
  }
  return createStringError(inconvertibleErrorCode(), "Expected bool at %s",
                           ctx.str().c_str());
}

/// Read an optional array field, treating null as valid only when requested.
std::optional<const json::Array *>
asOptArray(const json::Object &obj, StringRef key, bool canBeNull = false) {
  if (const json::Value *val = obj.get(key)) {
    if (val->getAsNull()) {
      if (canBeNull)
        return std::nullopt;
      REFOLD_LOG_FATAL("model",
                       "encountered unexpected null for property '{0}'", key);
    }
    auto arr = val->getAsArray();
    if (!arr) {
      REFOLD_LOG_FATAL(
          "model",
          "invalid json value type on field '{0}': expected array value", key);
    }
    return arr;
  }
  return std::nullopt;
}

/// Read an optional string field, treating null as valid only when requested.
///
/// Type mismatches are returned as std::nullopt; callers rely on the prior JSON
/// schema validation pass to reject malformed typed fields.
std::optional<StringRef> asOptString(const json::Object &obj, StringRef key,
                                     bool canBeNull = false) {
  if (const json::Value *val = obj.get(key)) {
    if (val->getAsNull()) {
      if (canBeNull)
        return std::nullopt;
      REFOLD_LOG_FATAL("model",
                       "encountered unexpected null for property '{0}'", key);
    }
    return val->getAsString();
  }
  return std::nullopt;
}

[[maybe_unused]]
/// Read an optional uint32_t field.
///
/// Out-of-range integer values are fatal because they cannot be represented in
/// the target type. Non-integer values are returned as std::nullopt; callers
/// rely on schema validation to catch malformed typed fields.
std::optional<uint32_t> asOptUInt32(const json::Object &obj, StringRef key,
                                    bool canBeNull = false) {
  if (const json::Value *val = obj.get(key)) {
    if (val->getAsNull()) {
      if (canBeNull)
        return std::nullopt;
      REFOLD_LOG_FATAL("model",
                       "encountered unexpected null for property '{0}'", key);
    }
    if (auto n = val->getAsUINT64()) {
      if (*n <= std::numeric_limits<uint32_t>::max())
        return static_cast<uint32_t>(*n);
      REFOLD_LOG_FATAL(
          "model",
          "uint32_t type out of bounds for value {0} on property '{1}'", *n,
          key);
    }
  }
  return std::nullopt;
}

/// Read an optional uint64_t field.
///
/// Non-integer values are returned as std::nullopt; callers rely on schema
/// validation to catch malformed typed fields.
std::optional<uint64_t> asOptUInt64(const json::Object &obj, StringRef key,
                                    bool canBeNull = false) {
  if (const json::Value *val = obj.get(key)) {
    if (val->getAsNull()) {
      if (canBeNull)
        return std::nullopt;
      REFOLD_LOG_FATAL("model",
                       "encountered unexpected null for property '{0}'", key);
    }
    return val->getAsUINT64();
  }
  return std::nullopt;
}

[[maybe_unused]]
/// Read an optional bool field.
///
/// Non-boolean values are returned as std::nullopt; callers rely on schema
/// validation to catch malformed typed fields.
std::optional<bool> asOptBool(const json::Object &obj, StringRef key,
                              bool canBeNull = false) {
  if (const json::Value *val = obj.get(key)) {
    if (val->getAsNull()) {
      if (canBeNull)
        return std::nullopt;
      REFOLD_LOG_FATAL("model",
                       "encountered unexpected null for property '{0}'", key);
    }
    return val->getAsBoolean();
  }
  return std::nullopt;
}

/// Apply a typed parser to a required field, preserving Expected error flow.
template <typename Fn>
auto applyToField(Fn &&fn, const json::Object &obj, StringRef key,
                  StringRef ctx = "root")
    -> decltype(std::forward<Fn>(fn)(std::declval<const json::Value &>(),
                                     std::declval<StringRef>())) {
  auto fieldOrErr = requireField(obj, key, ctx);
  if (!fieldOrErr)
    return fieldOrErr.takeError();
  return std::forward<decltype(fn)>(fn)(**fieldOrErr, key);
}

/// Return one array element as an object with contextual diagnostics.
Expected<const json::Object *> arrayObjElemAt(const json::Array &arr,
                                              std::size_t idx, StringRef ctx) {
  const json::Value &val = arr[idx];
  return asObject(val, ctx);
}

/// Read an optional array of uint64_t values.
static std::optional<std::vector<uint64_t>>
readUint64Array(const json::Object &parent, StringRef field) {
  const json::Array *arr = parent.getArray(field);
  if (!arr)
    return std::nullopt;

  std::vector<uint64_t> out;
  out.reserve(arr->size());
  for (const auto &v : *arr) {
    auto n = v.getAsUINT64();
    if (n)
      out.push_back(*n);
    else
      REFOLD_LOG_FATAL("model", "expected uint64_t from field '{0}'", field);
  }
  return out;
}

Expected<IncludeLookupKind> parseIncludeLookupKind(StringRef name,
                                                   StringRef ctx) {
  if (name == "source_relative")
    return IncludeLookupKind::SourceRelative;
  if (name == "quote_dir")
    return IncludeLookupKind::QuoteDir;
  if (name == "user_I")
    return IncludeLookupKind::UserI;
  if (name == "system")
    return IncludeLookupKind::System;
  if (name == "idirafter")
    return IncludeLookupKind::IdirAfter;
  if (name == "framework")
    return IncludeLookupKind::Framework;
  if (name == "builtin")
    return IncludeLookupKind::Builtin;
  if (name == "absolute_operand")
    return IncludeLookupKind::AbsoluteOperand;
  if (name == "unknown")
    return IncludeLookupKind::Unknown;
  return createStringError(inconvertibleErrorCode(),
                           "Invalid include lookup kind '%s' at %s",
                           name.str().c_str(), ctx.str().c_str());
}

Expected<RefoldModel::IncludeSearchEntry>
parseIncludeSearchEntry(const json::Object &obj, uint32_t expectedIndex,
                        StringRef ctx) {
  RefoldModel::IncludeSearchEntry entry;

  auto indexOrErr = applyToField(asUInt32, obj, "index", ctx);
  if (!indexOrErr)
    return indexOrErr.takeError();
  entry.index = *indexOrErr;
  if (entry.index != expectedIndex)
    return createStringError(
        inconvertibleErrorCode(),
        "Non-dense include_search_chain index at %s: expected %u, got %u",
        ctx.str().c_str(), expectedIndex, entry.index);

  auto kindOrErr = applyToField(asString, obj, "kind", ctx);
  if (!kindOrErr)
    return kindOrErr.takeError();
  auto parsedKind = parseIncludeLookupKind(*kindOrErr, ctx);
  if (!parsedKind)
    return parsedKind.takeError();
  entry.kind = *parsedKind;
  if (!isSearchChainIncludeLookupKind(entry.kind) &&
      entry.kind != IncludeLookupKind::Unknown)
    return createStringError(inconvertibleErrorCode(),
                             "Invalid per-edge include lookup kind '%s' in "
                             "include_search_chain at %s",
                             kindOrErr->str().c_str(), ctx.str().c_str());

  auto spellingOrErr = applyToField(asString, obj, "spelling", ctx);
  if (!spellingOrErr)
    return spellingOrErr.takeError();
  entry.spelling = *spellingOrErr;

  auto pathOrErr = applyToField(asString, obj, "path", ctx);
  if (!pathOrErr)
    return pathOrErr.takeError();
  entry.path = *pathOrErr;

  return entry;
}

Expected<std::optional<RefoldModel::IncludeLookupProvenance>>
parseOptionalIncludeLookupProvenance(
    const json::Object &obj, StringRef fieldName, StringRef ctx,
    ArrayRef<RefoldModel::IncludeSearchEntry> includeSearchChain) {
  const json::Value *lookupVal = obj.get(fieldName);
  if (!lookupVal)
    return std::optional<RefoldModel::IncludeLookupProvenance>();
  if (lookupVal->getAsNull())
    return createStringError(inconvertibleErrorCode(),
                             "Unexpected null include lookup at %s.%s",
                             ctx.str().c_str(), fieldName.str().c_str());

  auto lookupObjOrErr =
      asObject(*lookupVal, (Twine(ctx) + "." + fieldName).str());
  if (!lookupObjOrErr)
    return lookupObjOrErr.takeError();
  const json::Object &lookupObj = **lookupObjOrErr;

  auto kindStrOrErr =
      applyToField(asString, lookupObj, "kind", (Twine(ctx) + ".lookup").str());
  if (!kindStrOrErr)
    return kindStrOrErr.takeError();
  auto kindOrErr = parseIncludeLookupKind(*kindStrOrErr,
                                          (Twine(ctx) + ".lookup.kind").str());
  if (!kindOrErr)
    return kindOrErr.takeError();

  RefoldModel::IncludeLookupProvenance lookup;
  lookup.kind = *kindOrErr;
  lookup.searchChainIndex = asOptUInt32(lookupObj, "search_chain_index");
  lookup.directorySpelling = asOptString(lookupObj, "directory_spelling");
  lookup.directoryPath = asOptString(lookupObj, "directory_path");

  if (isSearchChainIncludeLookupKind(lookup.kind)) {
    if (!lookup.searchChainIndex)
      return createStringError(
          inconvertibleErrorCode(),
          "%s.lookup search-chain kind '%s' requires search_chain_index",
          ctx.str().c_str(), toString(lookup.kind).str().c_str());

    if (!includeSearchChain.empty()) {
      if (*lookup.searchChainIndex >= includeSearchChain.size())
        return createStringError(inconvertibleErrorCode(),
                                 "%s.lookup.search_chain_index %u is outside "
                                 "pp_ctx.include_search_chain",
                                 ctx.str().c_str(), *lookup.searchChainIndex);

      const auto &entry = includeSearchChain[*lookup.searchChainIndex];
      if (entry.kind != lookup.kind)
        return createStringError(
            inconvertibleErrorCode(),
            "%s.lookup kind '%s' disagrees with include_search_chain[%u] "
            "kind '%s'",
            ctx.str().c_str(), toString(lookup.kind).str().c_str(),
            *lookup.searchChainIndex, toString(entry.kind).str().c_str());

      // New-schema search-chain hits are selected by search_chain_index; the
      // directory spelling/path live authoritatively on the referenced
      // pp_ctx.include_search_chain entry.  Older producer maps may still carry
      // these per-edge copies as audit redundancy.  Accept them only when they
      // exactly match the chain entry, then normalize the in-memory lookup to
      // the chain values so downstream proof code has one source of truth.
      if (lookup.directorySpelling &&
          *lookup.directorySpelling != entry.spelling)
        return createStringError(
            inconvertibleErrorCode(),
            "%s.lookup.directory_spelling '%s' disagrees with "
            "include_search_chain[%u].spelling '%s'",
            ctx.str().c_str(), lookup.directorySpelling->str().c_str(),
            *lookup.searchChainIndex, entry.spelling.str().c_str());
      if (lookup.directoryPath && *lookup.directoryPath != entry.path)
        return createStringError(
            inconvertibleErrorCode(),
            "%s.lookup.directory_path '%s' disagrees with "
            "include_search_chain[%u].path '%s'",
            ctx.str().c_str(), lookup.directoryPath->str().c_str(),
            *lookup.searchChainIndex, entry.path.str().c_str());
      lookup.directorySpelling = entry.spelling;
      lookup.directoryPath = entry.path;
    }
  } else if (isPerEdgeIncludeLookupKind(lookup.kind)) {
    if (lookup.searchChainIndex)
      return createStringError(
          inconvertibleErrorCode(),
          "%s.lookup per-edge kind '%s' must not carry search_chain_index",
          ctx.str().c_str(), toString(lookup.kind).str().c_str());
    if (!lookup.directorySpelling || !lookup.directoryPath)
      return createStringError(
          inconvertibleErrorCode(),
          "%s.lookup per-edge kind '%s' requires directory_spelling and "
          "directory_path",
          ctx.str().c_str(), toString(lookup.kind).str().c_str());
  } else {
    assert(lookup.kind == IncludeLookupKind::Unknown &&
           "unhandled include lookup kind");
    if (lookup.searchChainIndex || lookup.directorySpelling ||
        lookup.directoryPath)
      return createStringError(
          inconvertibleErrorCode(),
          "%s.lookup unknown provenance must not carry directory/cursor fields",
          ctx.str().c_str());
  }

  return std::optional<RefoldModel::IncludeLookupProvenance>(std::move(lookup));
}

Expected<std::optional<RefoldModel::IncludeNextProvenance>>
parseOptionalIncludeNextProvenance(
    const json::Object &obj, StringRef subkind, StringRef ctx,
    ArrayRef<RefoldModel::IncludeSearchEntry> includeSearchChain) {
  const json::Value *nextVal = obj.get("include_next");
  if (!nextVal)
    return std::optional<RefoldModel::IncludeNextProvenance>();
  if (subkind != "#include_next")
    return createStringError(
        inconvertibleErrorCode(),
        "%s carries include_next provenance but subkind is '%s'",
        ctx.str().c_str(), subkind.str().c_str());
  if (nextVal->getAsNull())
    return createStringError(inconvertibleErrorCode(),
                             "Unexpected null include_next provenance at %s",
                             ctx.str().c_str());

  auto nextObjOrErr = asObject(*nextVal, (Twine(ctx) + ".include_next").str());
  if (!nextObjOrErr)
    return nextObjOrErr.takeError();
  const json::Object &nextObj = **nextObjOrErr;

  auto provenanceOrErr = applyToField(asString, nextObj, "provenance",
                                      (Twine(ctx) + ".include_next").str());
  if (!provenanceOrErr)
    return provenanceOrErr.takeError();

  RefoldModel::IncludeNextProvenance provenance;
  if (*provenanceOrErr == "known") {
    provenance.known = true;
    auto containingOrErr =
        applyToField(asUInt64, nextObj, "containing_file_include_id",
                     (Twine(ctx) + ".include_next").str());
    if (!containingOrErr)
      return containingOrErr.takeError();
    provenance.containingFileIncludeId = *containingOrErr;

    auto resumeOrErr =
        applyToField(asUInt32, nextObj, "resume_search_chain_index",
                     (Twine(ctx) + ".include_next").str());
    if (!resumeOrErr)
      return resumeOrErr.takeError();
    provenance.resumeSearchChainIndex = *resumeOrErr;

    if (!includeSearchChain.empty() &&
        *provenance.resumeSearchChainIndex >= includeSearchChain.size())
      return createStringError(
          inconvertibleErrorCode(),
          "%s.include_next.resume_search_chain_index %u is outside "
          "pp_ctx.include_search_chain",
          ctx.str().c_str(), *provenance.resumeSearchChainIndex);
  } else if (*provenanceOrErr == "unknown") {
    provenance.known = false;
    if (nextObj.get("containing_file_include_id") ||
        nextObj.get("resume_search_chain_index"))
      return createStringError(
          inconvertibleErrorCode(),
          "%s.include_next unknown provenance must not carry cursor fields",
          ctx.str().c_str());
  } else {
    return createStringError(inconvertibleErrorCode(),
                             "Invalid include_next provenance '%s' at %s",
                             provenanceOrErr->str().c_str(), ctx.str().c_str());
  }

  return std::optional<RefoldModel::IncludeNextProvenance>(
      std::move(provenance));
}

static Error validateIncludeMetadataAudit(const RefoldModel &model) {
  DenseMap<uint64_t, const RefoldModel::IncludeItem *> includeById;
  for (const RefoldModel::IncludeItem &include : model.GetIncludes()) {
    auto inserted = includeById.try_emplace(include.id, &include);
    if (!inserted.second)
      return createStringError(inconvertibleErrorCode(),
                               "Duplicate include item id %llu",
                               static_cast<unsigned long long>(include.id));

    if (include.enteredFileName && !include.enteredFileSpelling)
      return createStringError(
          inconvertibleErrorCode(),
          "include item %llu carries entered_file_name without "
          "entered_file_spelling",
          static_cast<unsigned long long>(include.id));

    if (!include.lookup)
      continue;

    const IncludeLookupKind kind = include.lookup->kind;
    if (isSearchChainIncludeLookupKind(kind)) {
      if (model.GetIncludeSearchChain().empty())
        return createStringError(
            inconvertibleErrorCode(),
            "include item %llu has search-chain lookup kind '%s' but "
            "pp_ctx.include_search_chain is absent or empty",
            static_cast<unsigned long long>(include.id),
            toString(kind).str().c_str());
      // parseOptionalIncludeLookupProvenance has already checked index
      // presence/range, kind equality, and any legacy redundant directory
      // copies against the search-chain entry.  Keep this cross-entry audit
      // here so the producer metadata contract remains enforced after model
      // construction.
      assert(include.lookup->searchChainIndex &&
             "search-chain lookup parsed without an index");
    }
  }

  for (const RefoldModel::IncludeItem &include : model.GetIncludes()) {
    if (include.subkind != "#include_next" || !include.includeNext ||
        !include.includeNext->known)
      continue;

    if (model.GetIncludeSearchChain().empty())
      return createStringError(
          inconvertibleErrorCode(),
          "#include_next item %llu has known provenance but no "
          "pp_ctx.include_search_chain",
          static_cast<unsigned long long>(include.id));

    if (!include.lookup || !include.lookup->searchChainIndex)
      return createStringError(
          inconvertibleErrorCode(),
          "#include_next item %llu has known provenance but its selected "
          "target lookup has no search_chain_index",
          static_cast<unsigned long long>(include.id));

    const unsigned selectedIndex = *include.lookup->searchChainIndex;
    const unsigned resumeIndex = *include.includeNext->resumeSearchChainIndex;
    if (selectedIndex < resumeIndex)
      return createStringError(
          inconvertibleErrorCode(),
          "#include_next item %llu selected search_chain_index %u before "
          "resume_search_chain_index %u",
          static_cast<unsigned long long>(include.id), selectedIndex,
          resumeIndex);

    const uint64_t containingId = *include.includeNext->containingFileIncludeId;
    auto ownerIt = includeById.find(containingId);
    if (ownerIt == includeById.end())
      return createStringError(
          inconvertibleErrorCode(),
          "#include_next item %llu references missing containing include "
          "item %llu",
          static_cast<unsigned long long>(include.id),
          static_cast<unsigned long long>(containingId));

    const RefoldModel::IncludeItem &owner = *ownerIt->second;
    if (!owner.lookup || !owner.lookup->searchChainIndex)
      return createStringError(
          inconvertibleErrorCode(),
          "#include_next item %llu has known provenance but containing "
          "include item %llu has no search_chain_index",
          static_cast<unsigned long long>(include.id),
          static_cast<unsigned long long>(containingId));

    const unsigned containingIndex = *owner.lookup->searchChainIndex;
    if (resumeIndex != containingIndex + 1)
      return createStringError(
          inconvertibleErrorCode(),
          "#include_next item %llu resume_search_chain_index %u does not "
          "immediately follow containing include item %llu selected index %u",
          static_cast<unsigned long long>(include.id), resumeIndex,
          static_cast<unsigned long long>(containingId), containingIndex);
  }

  return Error::success();
}
} // namespace

namespace clang {
namespace refold {

namespace {
/// Parse a JSON span array into strongly typed PPSpan/PPArgSpan records.
///
/// For PPArgSpan, \p argKind records which producer span family supplied the
/// element so later proof code can distinguish argument, stringify, and paste
/// provenance.
template <typename T = RefoldModel::PPSpan>
Expected<std::vector<T>>
parseSpans(const json::Value &val, StringRef ctx,
           std::optional<PPArgSpanKind> argKind = std::nullopt) {
  std::vector<T> out;
  auto arrOrErr = asArray(val, ctx);
  if (!arrOrErr)
    return arrOrErr.takeError();
  const json::Array *arr = *arrOrErr;

  out.reserve(arr->size());
  for (std::size_t i = 0; i < arr->size(); ++i) {
    const std::string ctxItem = (ctx + "[" + Twine(i) + "]").str();

    auto objOrErr = arrayObjElemAt(*arr, i, ctxItem);
    if (!objOrErr)
      return objOrErr.takeError();
    const json::Object *obj = *objOrErr;

    // 1. Parse common fields
    auto bOrErr = applyToField(asUInt64, *obj, "begin", ctxItem);
    if (!bOrErr)
      return bOrErr.takeError();

    auto eOrErr = applyToField(asUInt64, *obj, "end", ctxItem);
    if (!eOrErr)
      return eOrErr.takeError();

    T span;
    span.begin = *bOrErr;
    span.end = *eOrErr;

    // 2. Handle arg_index if T is PPArgSpan
    // Using 'if constexpr' ensures this code only exists for PPArgSpan
    if constexpr (std::is_same_v<T, RefoldModel::PPArgSpan>) {
      // Record which PPArgSpan kind array we are parsing (arg_spans,
      // stringify_spans, paste_spans).
      assert(argKind && "missing 'argKind' when parsing a PPArgSpan type");
      span.kind = *argKind;

      auto argOrErr = applyToField(asUInt32, *obj, "arg_index", ctxItem);
      if (!argOrErr)
        return argOrErr.takeError();
      span.argIdx = *argOrErr;

      // byte_begin/byte_end describe a token-internal slice within the
      // spelled output token.  They are required whenever a span identifies
      // only part of the spelled token, including paste spans and wrapped
      // stringify spans such as L## #x.
      span.byteBegin = asOptUInt32(*obj, "byte_begin");
      if (span.byteBegin) {
        auto beOrErr = applyToField(asUInt32, *obj, "byte_end", ctxItem);
        if (!beOrErr)
          return beOrErr.takeError();
        span.byteEnd = *beOrErr;
      } else if (obj->get("byte_end")) {
        return make_error<StringError>(
            formatv("{0}: field 'byte_end' requires 'byte_begin'", ctxItem)
                .str(),
            inconvertibleErrorCode());
      }

      // Check if there are pp_byte_begin/pp_byte_end pairs.
      span.ppByteBegin = asOptUInt64(*obj, "pp_byte_begin");
      if (span.ppByteBegin) {
        auto pbeOrErr = applyToField(asUInt64, *obj, "pp_byte_end", ctxItem);
        if (!pbeOrErr)
          return pbeOrErr.takeError();
        span.ppByteEnd = *pbeOrErr;
      }
    } else {
      assert(!argKind && "unexpected PPArgSpanKind on non-PPArgSpan type");
    }

    out.push_back(std::move(span));
  }
  return out;
}
} // namespace

// Public wrapper for parsing PPSpan arrays from JSON. This is used by the
// clang-refold driver for ancillary checks (e.g., --check + --no-lines ignore
// masks) without duplicating parsing logic.
/// Parse ordinary preprocessor token spans.
Expected<std::vector<RefoldModel::PPSpan>> parsePPSpans(const json::Value &val,
                                                        StringRef ctx) {
  return parseSpans<RefoldModel::PPSpan>(val, ctx);
}

bool RefoldModel::PPArgSpan::IsValid() const {
  if (!PPSpan::IsValid())
    return false;

  bool okTokenSubrange = (!byteBegin && !byteEnd) ||
                         (byteBegin && byteEnd && *byteEnd > *byteBegin);
  bool okPaste = (kind != PPArgSpanKind::Paste) || okTokenSubrange;
  bool okStringify = (kind != PPArgSpanKind::Stringify) || okTokenSubrange;

  bool okPP = (!ppByteBegin && !ppByteEnd) || (ppByteBegin && ppByteEnd);

  return okPaste && okStringify && okPP;
}

// ========================== RefoldModel construction =========================

Expected<RefoldModel::PreprocessContext>
RefoldModel::ParsePreprocessContext(const json::Object &root) {
  const json::Object *ctxObj = root.getObject("pp_ctx");
  if (!ctxObj) {
    return createStringError(inconvertibleErrorCode(),
                             "missing required top-level key 'pp_ctx'");
  }

  PreprocessContext ctx;

  if (auto cwd = ctxObj->getString("cwd")) {
    ctx.cwd = cwd->str();
  } else {
    return createStringError(inconvertibleErrorCode(),
                             "pp_ctx.cwd must be a string");
  }

  if (auto lang = ctxObj->getString("lang")) {
    ctx.lang = lang->str();
  } else {
    return createStringError(inconvertibleErrorCode(),
                             "pp_ctx.lang must be a string");
  }

  const json::Array *argvArr = ctxObj->getArray("argv");
  if (!argvArr) {
    return createStringError(inconvertibleErrorCode(),
                             "pp_ctx.argv must be an array");
  }

  ctx.argv.reserve(argvArr->size());
  for (const json::Value &v : *argvArr) {
    auto s = v.getAsString();
    if (!s) {
      return createStringError(inconvertibleErrorCode(),
                               "pp_ctx.argv must contain only strings");
    }
    ctx.argv.emplace_back(s->str());
  }

  return ctx;
}

Expected<std::string> RefoldModel::ParseSourcePath(const json::Object &root) {
  auto s = root.getString("source");
  if (!s || s->empty())
    return createStringError(inconvertibleErrorCode(),
                             "refold map missing required 'source' path");
  return s->str();
}

Expected<RefoldModel> RefoldModel::FromJson(const json::Object &root) {
  RefoldModel model;
  model.root_ = &root;

  // version
  auto versOrErr = applyToField(asString, root, "version");
  if (!versOrErr)
    return versOrErr.takeError();
  model.version_ = *versOrErr;

  // source
  auto srcPathOrErr = applyToField(asString, root, "source");
  if (!srcPathOrErr)
    return srcPathOrErr.takeError();
  model.sourcePath_ = *srcPathOrErr;

  // pp_ctx.cwd
  auto ppCtxOrErr = applyToField(asObject, root, "pp_ctx");
  if (!ppCtxOrErr)
    return ppCtxOrErr.takeError();
  const json::Object &ppCtxObj = **ppCtxOrErr;
  auto cwdOrErr = applyToField(asString, ppCtxObj, "cwd", "pp_ctx.cwd");
  if (!cwdOrErr)
    return cwdOrErr.takeError();
  model.ppCwd_ = *cwdOrErr;
  auto langOrErr = applyToField(asString, ppCtxObj, "lang", "pp_ctx.lang");
  if (!langOrErr)
    return langOrErr.takeError();
  model.ppLang_ = *langOrErr;

  if (const json::Value *argvVal = ppCtxObj.get("argv")) {
    auto argvOrErr = asArray(*argvVal, "pp_ctx.argv");
    if (!argvOrErr)
      return argvOrErr.takeError();
    const json::Array *argvArr = *argvOrErr;
    model.ppArgv_.reserve(argvArr->size());
    for (std::size_t i = 0; i < argvArr->size(); ++i) {
      auto argOrErr = asString((*argvArr)[i],
                               (Twine("pp_ctx.argv[") + Twine(i) + "]").str());
      if (!argOrErr)
        return argOrErr.takeError();
      model.ppArgv_.push_back(argOrErr->str());
    }
  }

  if (const json::Value *chainVal = ppCtxObj.get("include_search_chain")) {
    auto chainOrErr = asArray(*chainVal, "pp_ctx.include_search_chain");
    if (!chainOrErr)
      return chainOrErr.takeError();
    const json::Array *chainArr = *chainOrErr;
    model.includeSearchChain_.reserve(chainArr->size());
    for (std::size_t i = 0; i < chainArr->size(); ++i) {
      const std::string ctxItem =
          (Twine("pp_ctx.include_search_chain[") + Twine(i) + "]").str();
      auto entryObjOrErr = arrayObjElemAt(*chainArr, i, ctxItem);
      if (!entryObjOrErr)
        return entryObjOrErr.takeError();
      auto entryOrErr = parseIncludeSearchEntry(
          **entryObjOrErr, static_cast<uint32_t>(i), ctxItem);
      if (!entryOrErr)
        return entryOrErr.takeError();
      model.includeSearchChain_.push_back(*entryOrErr);
    }
  }

  // tokens.count
  auto tokObjOrErr = applyToField(asObject, root, "tokens");
  if (!tokObjOrErr)
    return tokObjOrErr.takeError();
  const json::Object &tokObj = **tokObjOrErr;
  auto cntOrErr = applyToField(asUInt64, tokObj, "count", "tokens.count");
  if (!cntOrErr)
    return cntOrErr.takeError();
  model.tokensCountA_ = *cntOrErr;

  // tokens.pp_byte_begin / tokens.pp_byte_end (optional)
  auto pbbOrErr = readUint64Array(tokObj, "pp_byte_begin");
  if (pbbOrErr)
    model.tokPPByteBeginA_ = std::move(*pbbOrErr);
  auto pbeOrErr = readUint64Array(tokObj, "pp_byte_end");
  if (pbeOrErr)
    model.tokPPByteEndA_ = std::move(*pbeOrErr);

  // Keep the optional per-token A-byte arrays only when the producer supplied
  // both halves of the begin/end pair. A single half is not useful for safe
  // byte-envelope reasoning, so drop it fail-closed.
  if (model.tokPPByteBeginA_ && !model.tokPPByteEndA_) {
    model.tokPPByteBeginA_ = std::nullopt;
    REFOLD_LOG_WARN("model", "missing per-token pp byte end spans");
  } else if (model.tokPPByteEndA_ && !model.tokPPByteBeginA_) {
    model.tokPPByteEndA_ = std::nullopt;
    REFOLD_LOG_WARN("model", "missing per-token pp byte begin spans");
  }

  // Make sure the token count matches the size of each tokPPByteBeginA_ and
  // tokPPByteEndA_ vector.
  if (model.tokPPByteBeginA_ &&
      (model.tokPPByteBeginA_->size() != model.tokPPByteEndA_->size() ||
       model.tokPPByteBeginA_->size() != (size_t)model.tokensCountA_)) {
    model.tokPPByteBeginA_ = std::nullopt;
    model.tokPPByteEndA_ = std::nullopt;
    REFOLD_LOG_WARN(
        "model",
        "pp byte begin/end spans size does not match the A-side token count");
  }

  auto parseMacroDefParams =
      [&](const json::Object &ownerObj, StringRef fieldName,
          const std::string &ctxItem) -> std::vector<MacroDefParam> {
    std::vector<MacroDefParam> params;
    if (auto paramsArr = asOptArray(ownerObj, fieldName, /*canBeNull=*/true)) {
      params.reserve((**paramsArr).size());
      for (const json::Value &Elem : **paramsArr) {
        auto objOrErrLocal =
            asObject(Elem, (Twine(ctxItem) + ": " + fieldName + "[]").str());
        if (!objOrErrLocal)
          REFOLD_LOG_FATAL("model", "{0}: {1} element is not an object",
                           ctxItem, fieldName);
        const json::Object &ParamObj = **objOrErrLocal;

        const json::Value *NameVal = ParamObj.get("name");
        if (!NameVal)
          REFOLD_LOG_FATAL("model", "{0}: {1} element missing name", ctxItem,
                           fieldName);
        auto nameOrErrLocal = asString(
            *NameVal, (Twine(ctxItem) + ": " + fieldName + ".name").str());
        if (!nameOrErrLocal)
          REFOLD_LOG_FATAL("model", "{0}: {1}.name is not a string", ctxItem,
                           fieldName);

        const json::Value *VarVal = ParamObj.get("variadic");
        if (!VarVal)
          REFOLD_LOG_FATAL("model", "{0}: {1} element missing variadic",
                           ctxItem, fieldName);
        auto varOrErr = asBool(
            *VarVal, (Twine(ctxItem) + ": " + fieldName + ".variadic").str());
        if (!varOrErr)
          REFOLD_LOG_FATAL("model", "{0}: {1}.variadic is not a bool", ctxItem,
                           fieldName);

        params.emplace_back(*nameOrErrLocal, *varOrErr);
      }
    }
    return params;
  };

  auto parseMacroReplacementTokens =
      [&](const json::Object &ownerObj, ArrayRef<MacroDefParam> defParams,
          const std::string &ctxItem) -> std::vector<MacroReplacementToken> {
    std::vector<MacroReplacementToken> tokens;
    if (auto tokensArr = asOptArray(ownerObj, "replacement_tokens",
                                    /*canBeNull=*/true)) {
      tokens.reserve((**tokensArr).size());
      for (const json::Value &Elem : **tokensArr) {
        auto objOrErrLocal =
            asObject(Elem, (Twine(ctxItem) + ": replacement_tokens[]").str());
        if (!objOrErrLocal)
          REFOLD_LOG_FATAL("model",
                           "{0}: replacement_tokens element is not an object",
                           ctxItem);
        const json::Object &TokObj = **objOrErrLocal;

        const json::Value *KindVal = TokObj.get("kind");
        if (!KindVal)
          REFOLD_LOG_FATAL(
              "model", "{0}: replacement_tokens element missing kind", ctxItem);
        auto kindOrErrLocal = asString(
            *KindVal, (Twine(ctxItem) + ": replacement_tokens.kind").str());
        if (!kindOrErrLocal)
          REFOLD_LOG_FATAL(
              "model", "{0}: replacement_tokens.kind is not a string", ctxItem);

        const json::Value *SpellingVal = TokObj.get("spelling");
        if (!SpellingVal)
          REFOLD_LOG_FATAL("model",
                           "{0}: replacement_tokens element missing spelling",
                           ctxItem);
        auto spellingOrErr =
            asString(*SpellingVal,
                     (Twine(ctxItem) + ": replacement_tokens.spelling").str());
        if (!spellingOrErr)
          REFOLD_LOG_FATAL("model",
                           "{0}: replacement_tokens.spelling is not a string",
                           ctxItem);

        MacroReplacementToken token;
        token.spelling = *spellingOrErr;
        if (*kindOrErrLocal == "literal") {
          token.kind = MacroReplacementTokenKind::Literal;
          if (TokObj.get("param_index"))
            REFOLD_LOG_FATAL(
                "model",
                "{0}: literal replacement_tokens element has param_index",
                ctxItem);
        } else if (*kindOrErrLocal == "param_ref") {
          token.kind = MacroReplacementTokenKind::ParamRef;
          const json::Value *ParamVal = TokObj.get("param_index");
          if (!ParamVal)
            REFOLD_LOG_FATAL(
                "model",
                "{0}: param_ref replacement_tokens element missing "
                "param_index",
                ctxItem);
          auto paramOrErr = asUInt32(
              *ParamVal,
              (Twine(ctxItem) + ": replacement_tokens.param_index").str());
          if (!paramOrErr)
            REFOLD_LOG_FATAL("model",
                             "{0}: replacement_tokens.param_index is invalid",
                             ctxItem);
          if (*paramOrErr >= defParams.size())
            REFOLD_LOG_FATAL(
                "model",
                "{0}: replacement_tokens.param_index references missing "
                "macro formal",
                ctxItem);
          if (defParams[*paramOrErr].name != token.spelling)
            REFOLD_LOG_FATAL(
                "model",
                "{0}: replacement_tokens param_ref spelling does not match "
                "the referenced macro formal",
                ctxItem);
          token.paramIndex = *paramOrErr;
        } else {
          REFOLD_LOG_FATAL("model",
                           "{0}: invalid replacement_tokens.kind '{1}'",
                           ctxItem, *kindOrErrLocal);
        }

        tokens.push_back(std::move(token));
      }
    }
    return tokens;
  };

  auto parseCalleeOriginParts =
      [&](const json::Object &originObj, StringRef finalSpelling,
          const std::string &ctxItem) -> std::vector<CalleeOriginPart> {
    std::vector<CalleeOriginPart> parts;
    auto partsArr = asOptArray(originObj, "parts", /*canBeNull=*/true);
    if (!partsArr)
      return parts;

    parts.reserve((**partsArr).size());
    std::string tiled;
    for (const json::Value &Elem : **partsArr) {
      auto objOrErrLocal =
          asObject(Elem, (Twine(ctxItem) + ": callee_origin.parts[]").str());
      if (!objOrErrLocal)
        REFOLD_LOG_FATAL("model",
                         "{0}: callee_origin.parts element is not an object",
                         ctxItem);
      const json::Object &PartObj = **objOrErrLocal;

      const json::Value *KindVal = PartObj.get("kind");
      if (!KindVal)
        REFOLD_LOG_FATAL(
            "model", "{0}: callee_origin.parts element missing kind", ctxItem);
      auto kindOrErrLocal = asString(
          *KindVal, (Twine(ctxItem) + ": callee_origin.parts.kind").str());
      if (!kindOrErrLocal)
        REFOLD_LOG_FATAL(
            "model", "{0}: callee_origin.parts.kind is not a string", ctxItem);

      const json::Value *SpellingVal = PartObj.get("spelling");
      if (!SpellingVal)
        REFOLD_LOG_FATAL("model",
                         "{0}: callee_origin.parts element missing spelling",
                         ctxItem);
      auto spellingOrErr =
          asString(*SpellingVal,
                   (Twine(ctxItem) + ": callee_origin.parts.spelling").str());
      if (!spellingOrErr)
        REFOLD_LOG_FATAL("model",
                         "{0}: callee_origin.parts.spelling is not a string",
                         ctxItem);

      CalleeOriginPart part;
      part.spelling = *spellingOrErr;
      if (*kindOrErrLocal == "literal") {
        part.kind = CalleeOriginPartKind::Literal;
        if (PartObj.get("root_macro_id") || PartObj.get("root_param_index") ||
            PartObj.get("byte_begin") || PartObj.get("byte_end"))
          REFOLD_LOG_FATAL(
              "model",
              "{0}: literal callee_origin part carries selector-slice fields",
              ctxItem);
      } else if (*kindOrErrLocal == "caller_arg_slice") {
        part.kind = CalleeOriginPartKind::CallerArgSlice;
        auto rootOrErr =
            applyToField(asUInt64, PartObj, "root_macro_id",
                         (Twine(ctxItem) + ": callee_origin.parts").str());
        if (!rootOrErr)
          REFOLD_LOG_FATAL("model",
                           "{0}: callee_origin.parts.root_macro_id is invalid",
                           ctxItem);
        auto paramOrErr =
            applyToField(asUInt32, PartObj, "root_param_index",
                         (Twine(ctxItem) + ": callee_origin.parts").str());
        if (!paramOrErr)
          REFOLD_LOG_FATAL(
              "model", "{0}: callee_origin.parts.root_param_index is invalid",
              ctxItem);
        auto beginOrErrLocal =
            applyToField(asUInt32, PartObj, "byte_begin",
                         (Twine(ctxItem) + ": callee_origin.parts").str());
        if (!beginOrErrLocal)
          REFOLD_LOG_FATAL("model",
                           "{0}: callee_origin.parts.byte_begin is invalid",
                           ctxItem);
        auto endOrErrLocal =
            applyToField(asUInt32, PartObj, "byte_end",
                         (Twine(ctxItem) + ": callee_origin.parts").str());
        if (!endOrErrLocal)
          REFOLD_LOG_FATAL(
              "model", "{0}: callee_origin.parts.byte_end is invalid", ctxItem);
        if (*endOrErrLocal < *beginOrErrLocal)
          REFOLD_LOG_FATAL("model",
                           "{0}: callee_origin selector slice range is invalid",
                           ctxItem);
        part.rootMacroId = *rootOrErr;
        part.rootParamIndex = *paramOrErr;
        part.byteBegin = *beginOrErrLocal;
        part.byteEnd = *endOrErrLocal;
      } else {
        REFOLD_LOG_FATAL("model", "{0}: invalid callee_origin.parts.kind '{1}'",
                         ctxItem, *kindOrErrLocal);
      }

      tiled.append(part.spelling.begin(), part.spelling.end());
      parts.push_back(std::move(part));
    }

    if (tiled != finalSpelling)
      REFOLD_LOG_FATAL(
          "model",
          "{0}: callee_origin.parts do not tile the final callee spelling",
          ctxItem);
    return parts;
  };

  // tokmap
  {
    auto arrOrErr = applyToField(asArray, root, "tokmap");
    if (!arrOrErr)
      return arrOrErr.takeError();
    const json::Array *arr = *arrOrErr;

    model.tokmap_.reserve(arr->size());
    for (std::size_t i = 0; i < arr->size(); ++i) {
      const std::string ctxItem = (Twine("tokmap[") + Twine(i) + "]").str();

      auto objOrErr = arrayObjElemAt(*arr, i, ctxItem);
      if (!objOrErr)
        return objOrErr.takeError();
      const json::Object *obj = *objOrErr;

      TokMapEntry entry;

      // required
      auto fileOrErr = applyToField(asString, *obj, "file", ctxItem);
      if (!fileOrErr)
        return fileOrErr.takeError();
      entry.file = *fileOrErr;

      auto bOrErr = applyToField(asUInt64, *obj, "b", ctxItem);
      if (!bOrErr)
        return bOrErr.takeError();
      entry.b = *bOrErr;

      auto eOrErr = applyToField(asUInt64, *obj, "e", ctxItem);
      if (!eOrErr)
        return eOrErr.takeError();
      entry.e = *eOrErr;

      // `pp` (the A-token index this range maps to) is a required provenance
      // fact.  It must be read from the producer, never synthesized from stream
      // order: a positional index would fabricate provenance and could collide
      // with an explicit `pp` from another entry, silently corrupting
      // token->byte resolution.  Fail closed when it is absent.
      auto ppOrErr = applyToField(asUInt64, *obj, "pp", ctxItem);
      if (!ppOrErr)
        return ppOrErr.takeError();
      entry.pp = *ppOrErr;

      model.tokmap_.push_back(entry);
      model.tokmapByPP_[entry.pp] = entry;
    }
  }

  // items
  {
    auto arrOrErr = applyToField(asArray, root, "items");
    if (!arrOrErr)
      return arrOrErr.takeError();
    const json::Array *arr = *arrOrErr;

    for (std::size_t i = 0; i < arr->size(); ++i) {
      const std::string ctxItem = (Twine("items[") + Twine(i) + "]").str();

      auto objOrErr = arrayObjElemAt(*arr, i, ctxItem);
      if (!objOrErr)
        return objOrErr.takeError();
      const json::Object *obj = *objOrErr;

      // kind
      auto kindOrErr = applyToField(asString, *obj, "kind", ctxItem);
      if (!kindOrErr)
        return kindOrErr.takeError();
      StringRef kindStr = *kindOrErr;

      if (kindStr == "directive") {
        // subkind
        auto skOrErr = applyToField(asString, *obj, "subkind", ctxItem);
        if (!skOrErr)
          return skOrErr.takeError();
        StringRef skStr = *skOrErr;

        if (skStr == "#include" || skStr == "#include_next") {
          const std::string ctxItem =
              (Twine("include items[") + Twine(i) + "]").str();

          // required
          auto idOrErr = applyToField(asUInt64, *obj, "id", ctxItem);
          if (!idOrErr)
            return idOrErr.takeError();
          int id = *idOrErr;

          auto textOrErr = applyToField(asString, *obj, "text", ctxItem);
          if (!textOrErr)
            return textOrErr.takeError();
          StringRef text = *textOrErr;

          auto spOrErr = applyToField(asString, *obj, "site_path", ctxItem);
          if (!spOrErr)
            return spOrErr.takeError();
          StringRef sitePath = *spOrErr;

          auto tgtOrErr = applyToField(asString, *obj, "target", ctxItem);
          if (!tgtOrErr)
            return tgtOrErr.takeError();
          StringRef target = *tgtOrErr;

          auto angledOrErr = applyToField(asBool, *obj, "angled", ctxItem);
          if (!angledOrErr)
            return angledOrErr.takeError();
          bool angled = *angledOrErr;

          // not really optional, but can be string or null (null values will
          // produce a fatal error)
          uint64_t siteB = *asOptUInt64(*obj, "site_b");
          uint64_t siteE = *asOptUInt64(*obj, "site_e");

          // optional
          std::optional<StringRef> resolved =
              asOptString(*obj, "resolved_path");
          std::optional<StringRef> openedPath =
              asOptString(*obj, "opened_path");
          std::optional<StringRef> enteredFileSpelling =
              asOptString(*obj, "entered_file_spelling");
          std::optional<StringRef> enteredFileName =
              asOptString(*obj, "entered_file_name");

          auto lookupOrErr = parseOptionalIncludeLookupProvenance(
              *obj, "lookup", ctxItem, model.includeSearchChain_);
          if (!lookupOrErr)
            return lookupOrErr.takeError();
          std::optional<IncludeLookupProvenance> lookup =
              std::move(*lookupOrErr);

          auto includeNextOrErr = parseOptionalIncludeNextProvenance(
              *obj, skStr, ctxItem, model.includeSearchChain_);
          if (!includeNextOrErr)
            return includeNextOrErr.takeError();
          std::optional<IncludeNextProvenance> includeNext =
              std::move(*includeNextOrErr);

          std::optional<uint64_t> parent = asOptUInt64(*obj, "parent");

          std::vector<PPSpan> spans;
          if (const json::Value *spansVal = obj->get("spans")) {
            auto sp = parseSpans(*spansVal, "include.spans");
            if (!sp)
              return sp.takeError();
            spans = std::move(*sp);
          }

          std::vector<RefoldModel::HeaderDecl> decls;
          if (auto declArr = asOptArray(*obj, "decls")) {
            decls.reserve((*declArr)->size());
            for (std::size_t di = 0; di < (*declArr)->size(); ++di) {
              const std::string ctxDecl =
                  (Twine("include.decls[") + Twine(di) + "]").str();
              auto declObjOrErr = asObject((**declArr)[di], ctxDecl);
              if (!declObjOrErr)
                return declObjOrErr.takeError();
              const json::Object &declObj = **declObjOrErr;

              auto kindOrErr = applyToField(asString, declObj, "kind", ctxDecl);
              if (!kindOrErr)
                return kindOrErr.takeError();
              auto nameOrErr = applyToField(asString, declObj, "name", ctxDecl);
              if (!nameOrErr)
                return nameOrErr.takeError();

              const std::string headerCtxDecl = ctxDecl + ".header_span";
              auto hsValOrErr = requireField(declObj, "header_span", ctxDecl);
              if (!hsValOrErr)
                return hsValOrErr.takeError();
              auto hsObjOrErr = asObject(**hsValOrErr, headerCtxDecl);
              if (!hsObjOrErr)
                return hsObjOrErr.takeError();
              const json::Object &hsObj = **hsObjOrErr;

              // header_span.file is optional; when omitted, the header file is
              // implied by the enclosing include's new opened_path when
              // present, otherwise by legacy resolved_path. This avoids
              // repeating the same path string for every decl while preserving
              // old-map compatibility.
              std::optional<StringRef> hsFileOpt = asOptString(hsObj, "file");
              StringRef hsFile;
              if (hsFileOpt) {
                hsFile = *hsFileOpt;
              } else if (openedPath) {
                hsFile = *openedPath;
              } else if (resolved) {
                hsFile = *resolved;
              } else {
                return createStringError(
                    inconvertibleErrorCode(),
                    "Missing required field 'header_span.file' at %s (and "
                    "include has neither opened_path nor resolved_path)",
                    headerCtxDecl.c_str());
              }

              auto hsBOrErr = applyToField(asUInt64, hsObj, "b", headerCtxDecl);
              if (!hsBOrErr)
                return hsBOrErr.takeError();
              auto hsEOrErr = applyToField(asUInt64, hsObj, "e", headerCtxDecl);
              if (!hsEOrErr)
                return hsEOrErr.takeError();

              auto psValOrErr = requireField(declObj, "pp_span", ctxDecl);
              if (!psValOrErr)
                return psValOrErr.takeError();
              auto psObjOrErr = asObject(**psValOrErr, ctxDecl + ".pp_span");
              if (!psObjOrErr)
                return psObjOrErr.takeError();
              const json::Object &psObj = **psObjOrErr;
              auto psBOrErr = applyToField(asUInt64, psObj, "begin", ctxDecl);
              if (!psBOrErr)
                return psBOrErr.takeError();
              auto psEOrErr = applyToField(asUInt64, psObj, "end", ctxDecl);
              if (!psEOrErr)
                return psEOrErr.takeError();

              RefoldModel::HeaderDecl decl;
              decl.kind = *kindOrErr;
              decl.name = *nameOrErr;
              decl.file = hsFile;
              decl.headerB = *hsBOrErr;
              decl.headerE = *hsEOrErr;
              decl.span = PPSpan{*psBOrErr, *psEOrErr};
              decls.push_back(std::move(decl));
            }
          }

          IncludeItem inc(/*id*/ id,
                          /*subkind*/ skStr,
                          /*text*/ text,
                          /*sitePath*/ sitePath,
                          /*siteB*/ siteB,
                          /*siteE*/ siteE,
                          /*target*/ target,
                          /*resolvedPath*/ resolved,
                          /*openedPath*/ openedPath,
                          /*enteredFileSpelling*/ enteredFileSpelling,
                          /*enteredFileName*/ enteredFileName,
                          /*lookup*/ std::move(lookup),
                          /*includeNext*/ std::move(includeNext),
                          /*angled*/ angled,
                          /*parent*/ parent,
                          /*spans*/ std::move(spans),
                          /*decls*/ std::move(decls));

          model.includes_.push_back(std::move(inc));
        } else if (skStr == "#define" || skStr == "#undef") {
          MacroDirective md;

          const std::string ctxItem =
              (Twine("define/undef items[") + Twine(i) + "]").str();

          // required
          auto idOrErr = applyToField(asUInt64, *obj, "id", ctxItem);
          if (!idOrErr)
            return idOrErr.takeError();
          md.id = *idOrErr;

          auto nameOrErr = applyToField(asString, *obj, "name", ctxItem);
          if (!nameOrErr)
            return nameOrErr.takeError();
          md.name = *nameOrErr;

          auto textOrErr = applyToField(asString, *obj, "text", ctxItem);
          if (!textOrErr)
            return textOrErr.takeError();
          md.text = *textOrErr;

          if (skStr == "#define") {
            auto functionLikeOrErr =
                applyToField(asBool, *obj, "function_like", ctxItem);
            if (!functionLikeOrErr)
              return functionLikeOrErr.takeError();
            md.functionLike = *functionLikeOrErr;
          }

          auto spOrErr = applyToField(asString, *obj, "site_path", ctxItem);
          if (!spOrErr)
            return spOrErr.takeError();
          md.sitePath = *spOrErr;

          auto spansOrErr = requireField(*obj, "spans", ctxItem);
          if (!spansOrErr)
            return spansOrErr.takeError();
          auto spans = parseSpans(**spansOrErr, "macro.directive.spans");
          if (!spans)
            return spans.takeError();
          md.spans = std::move(*spans);

          // not really optional, but can be string or null (null values will
          // produce a fatal error)
          md.siteB = *asOptUInt64(*obj, "site_b");
          md.siteE = *asOptUInt64(*obj, "site_e");

          // Optional producer-owned replay data for #define directives.
          // #undef records intentionally keep these vectors empty: their only
          // semantic payload is the macro-state transition, not
          // replacement-list replay.
          if (skStr == "#define") {
            md.defParams = parseMacroDefParams(*obj, "def_params", ctxItem);
            md.replacementTokens =
                parseMacroReplacementTokens(*obj, md.defParams, ctxItem);
          }

          // optional
          md.ownerIncludeId = asOptUInt64(*obj, "owner_include_id");

          md.subkind = skStr;
          model.macroDirs_.push_back(std::move(md));
        } else if (skStr == "#pragma") {
          PragmaDirective pd;

          const std::string ctxItem =
              (Twine("pragma items[") + Twine(i) + "]").str();

          // required
          auto idOrErr = applyToField(asUInt64, *obj, "id", ctxItem);
          if (!idOrErr)
            return idOrErr.takeError();
          pd.id = *idOrErr;

          auto textOrErr = applyToField(asString, *obj, "text", ctxItem);
          if (!textOrErr)
            return textOrErr.takeError();
          pd.text = *textOrErr;

          auto spOrErr = applyToField(asString, *obj, "site_path", ctxItem);
          if (!spOrErr)
            return spOrErr.takeError();
          pd.sitePath = *spOrErr;

          // not really optional, but can be string or null (null values will
          // produce a fatal error)
          pd.siteB = *asOptUInt64(*obj, "site_b");
          pd.siteE = *asOptUInt64(*obj, "site_e");

          // Optional in older maps.  When present, this is the preferred
          // repeated-header disambiguator for zero-token pragma owners.
          pd.ownerIncludeId = asOptUInt64(*obj, "owner_include_id");

          // Preserve producer-owned `_Pragma` source provenance in the typed
          // model.  Sideband replay already consumes these fields directly
          // from JSON; the preprocessing-structure census also needs them so a
          // mid-line pragma operator cannot disappear behind an ordinary token
          // byte envelope.  Missing or partial ranges remain unbound and are
          // rejected by the interval index rather than guessed here.
          pd.viaPragmaOperator =
              obj->getBoolean("via_pragma_operator").value_or(false);
          pd.operatorB = asOptUInt64(*obj, "operator_b");
          pd.operatorE = asOptUInt64(*obj, "operator_e");

          model.pragmas_.push_back(std::move(pd));
        } else {
          return createStringError(
              inconvertibleErrorCode(),
              "Unknown directive subkind '%s' at items[%zu]",
              skStr.str().c_str(), i);
        }
      } else if (kindStr == "macro") {
        const std::string ctxItem =
            (Twine("macro items[") + Twine(i) + "]").str();

        // required
        auto idOrErr = applyToField(asUInt64, *obj, "id", ctxItem);
        if (!idOrErr)
          return idOrErr.takeError();
        int id = *idOrErr;

        auto skOrErr = applyToField(asString, *obj, "subkind", ctxItem);
        if (!skOrErr)
          return skOrErr.takeError();
        StringRef subkind = *skOrErr;

        auto nameOrErr = applyToField(asString, *obj, "name", ctxItem);
        if (!nameOrErr)
          return nameOrErr.takeError();
        StringRef name = *nameOrErr;

        auto invTextValOrErr = requireField(*obj, "inv_text", ctxItem);
        if (!invTextValOrErr)
          return invTextValOrErr.takeError();
        std::optional<StringRef> invText;
        if (!(*invTextValOrErr)->getAsNull()) {
          auto invTextOrErr =
              asString(**invTextValOrErr, ctxItem + ": inv_text");
          if (!invTextOrErr)
            return invTextOrErr.takeError();
          invText = *invTextOrErr;
        }
        std::optional<StringRef> normalizedInvText =
            asOptString(*obj, "normalized_inv_text", /*canBeNull=*/true);

        auto spansOrErr = requireField(*obj, "spans", ctxItem);
        if (!spansOrErr)
          return spansOrErr.takeError();
        auto spans = parseSpans(**spansOrErr, "macro.spans");
        if (!spans)
          return spans.takeError();

        // optional
        std::optional<StringRef> invFile = asOptString(*obj, "inv_file");
        std::optional<uint64_t> invB = asOptUInt64(*obj, "inv_b");
        std::optional<uint64_t> invE = asOptUInt64(*obj, "inv_e");
        std::optional<uint64_t> invPPByteBegin =
            asOptUInt64(*obj, "inv_pp_byte_begin");
        std::optional<uint64_t> invPPByteEnd =
            asOptUInt64(*obj, "inv_pp_byte_end");
        std::optional<uint64_t> ownerIncludeId =
            asOptUInt64(*obj, "owner_include_id");
        std::optional<uint64_t> definitionDirectiveId =
            asOptUInt64(*obj, "definition_directive_id",
                        /*canBeNull=*/true);
        std::vector<MacroDefParam> defParams =
            parseMacroDefParams(*obj, "def_params", ctxItem);

        auto parseOptByteRanges = [&](StringRef fieldName,
                                      std::vector<MacroInvocation::OptByteRange>
                                          &out) {
          // These ranges intentionally preserve nullable endpoints from older
          // producer schemas; downstream proof code decides whether null ranges
          // are sufficient for the specific certificate being built.
          if (auto arr = asOptArray(*obj, fieldName, /*canBeNull=*/true)) {
            for (const auto &elem : **arr) {
              auto *rObj = elem.getAsObject();
              if (!rObj) {
                REFOLD_LOG_FATAL(
                    "model",
                    "invalid json value type on field '%s': expected object "
                    "value",
                    fieldName.str().c_str());
              }

              const json::Value *bVal = rObj->get("b");
              const json::Value *eVal = rObj->get("e");
              if (!bVal || !eVal) {
                REFOLD_LOG_FATAL(
                    "model",
                    "missing required fields on '%s' element: expected {b,e}",
                    fieldName.str().c_str());
              }

              std::optional<uint64_t> b;
              std::optional<uint64_t> e;
              if (!bVal->getAsNull())
                b = bVal->getAsUINT64();
              if (!eVal->getAsNull())
                e = eVal->getAsUINT64();

              out.emplace_back(b, e);
            }
          }
        };

        std::vector<MacroInvocation::OptByteRange> invArgRanges;
        parseOptByteRanges("inv_arg_ranges", invArgRanges);

        std::vector<MacroInvocation::OptByteRange> normalizedInvArgTextRanges;
        parseOptByteRanges("normalized_inv_arg_text_ranges",
                           normalizedInvArgTextRanges);

        std::vector<PPArgSpan> argSpans;
        if (const json::Value *spansVal = obj->get("arg_spans")) {
          auto sp = parseSpans<PPArgSpan>(*spansVal, "macro.arg_spans",
                                          PPArgSpanKind::Standard);
          if (!sp)
            return sp.takeError();
          argSpans = std::move(*sp);
        }

        std::vector<PPArgSpan> stringifySpans;
        if (const json::Value *spansVal = obj->get("stringify_spans")) {
          auto sp = parseSpans<PPArgSpan>(*spansVal, "macro.stringify_spans",
                                          PPArgSpanKind::Stringify);
          if (!sp)
            return sp.takeError();
          stringifySpans = std::move(*sp);
        }

        std::vector<PPArgSpan> pasteSpans;
        if (const json::Value *spansVal = obj->get("paste_spans")) {
          auto sp = parseSpans<PPArgSpan>(*spansVal, "macro.paste_spans",
                                          PPArgSpanKind::Paste);
          if (!sp)
            return sp.takeError();
          pasteSpans = std::move(*sp);
        }

        std::vector<PPSpan> bodySpans;
        if (const json::Value *spansVal = obj->get("body_spans")) {
          auto sp = parseSpans(*spansVal, "macro.body_spans");
          if (!sp)
            return sp.takeError();
          bodySpans = std::move(*sp);
        }

        // `paste_tokens` preserves the producer's exact `##` witnesses in the
        // order they were synthesized for this invocation. Loading the data
        // into the model lets later proof classes consume it without
        // reconstructing paste decomposition from spans alone.
        std::vector<RefoldModel::PasteToken> pasteTokens;
        if (auto tokensArr =
                asOptArray(*obj, "paste_tokens", /*allowNull=*/true)) {
          pasteTokens.reserve((**tokensArr).size());
          for (const json::Value &Entry : **tokensArr) {
            auto objOrErrLocal = asObject(Entry, ctxItem);
            if (!objOrErrLocal)
              REFOLD_LOG_FATAL(
                  "model", "{0}: paste_tokens entry is not an object", ctxItem);
            const json::Object &TokObj = **objOrErrLocal;

            const json::Value *SpellingVal = TokObj.get("spelling");
            if (!SpellingVal)
              REFOLD_LOG_FATAL(
                  "model", "{0}: paste_tokens entry missing spelling", ctxItem);
            auto spellingOrErr =
                asString(*SpellingVal, ctxItem + ": paste_tokens.spelling");
            if (!spellingOrErr)
              REFOLD_LOG_FATAL("model",
                               "{0}: paste_tokens.spelling is not a string",
                               ctxItem);

            const json::Value *PartsVal = TokObj.get("parts");
            if (!PartsVal)
              REFOLD_LOG_FATAL("model", "{0}: paste_tokens entry missing parts",
                               ctxItem);
            auto partsArrOrErr =
                asArray(*PartsVal, ctxItem + ": paste_tokens.parts");
            if (!partsArrOrErr)
              REFOLD_LOG_FATAL(
                  "model", "{0}: paste_tokens.parts is not an array", ctxItem);

            std::vector<RefoldModel::PastePart> parts;
            parts.reserve((**partsArrOrErr).size());
            for (const json::Value &PartVal : **partsArrOrErr) {
              auto partObjOrErr = asObject(PartVal, ctxItem);
              if (!partObjOrErr)
                REFOLD_LOG_FATAL("model",
                                 "{0}: paste_tokens part is not an object",
                                 ctxItem);
              const json::Object &PartObj = **partObjOrErr;

              std::optional<uint32_t> argIndex =
                  asOptUInt32(PartObj, "arg_index", /*canBeNull=*/true);

              std::optional<StringRef> kindText =
                  asOptString(PartObj, "kind", /*canBeNull=*/true);
              // Older maps can omit `kind`; infer it from arg_index, then
              // verify any explicit spelling agrees with the inferred payload
              // shape.
              RefoldModel::PastePartKind kindLocal =
                  argIndex ? RefoldModel::PastePartKind::Arg
                           : RefoldModel::PastePartKind::Literal;
              if (kindText) {
                if (*kindText == "arg")
                  kindLocal = RefoldModel::PastePartKind::Arg;
                else if (*kindText == "literal")
                  kindLocal = RefoldModel::PastePartKind::Literal;
                else
                  REFOLD_LOG_FATAL(
                      "model", "{0}: paste_tokens.part.kind is invalid: {1}",
                      ctxItem, *kindText);
              }
              if (kindLocal == RefoldModel::PastePartKind::Arg && !argIndex)
                REFOLD_LOG_FATAL("model",
                                 "{0}: arg paste_tokens part missing arg_index",
                                 ctxItem);
              if (kindLocal == RefoldModel::PastePartKind::Literal && argIndex)
                REFOLD_LOG_FATAL("model",
                                 "{0}: literal paste_tokens part has arg_index",
                                 ctxItem);

              const json::Value *ByteBVal = PartObj.get("byte_begin");
              if (!ByteBVal)
                REFOLD_LOG_FATAL("model",
                                 "{0}: paste_tokens part missing byte_begin",
                                 ctxItem);
              auto byteBOrErr = asUInt32(
                  *ByteBVal, ctxItem + ": paste_tokens.part.byte_begin");
              if (!byteBOrErr)
                REFOLD_LOG_FATAL(
                    "model",
                    "{0}: paste_tokens.part.byte_begin is not a uint32",
                    ctxItem);

              const json::Value *ByteEVal = PartObj.get("byte_end");
              if (!ByteEVal)
                REFOLD_LOG_FATAL("model",
                                 "{0}: paste_tokens part missing byte_end",
                                 ctxItem);
              auto byteEOrErr =
                  asUInt32(*ByteEVal, ctxItem + ": paste_tokens.part.byte_end");
              if (!byteEOrErr)
                REFOLD_LOG_FATAL(
                    "model", "{0}: paste_tokens.part.byte_end is not a uint32",
                    ctxItem);
              if (*byteEOrErr < *byteBOrErr ||
                  *byteEOrErr > spellingOrErr->size())
                REFOLD_LOG_FATAL("model",
                                 "{0}: paste_tokens part byte range is invalid",
                                 ctxItem);

              std::optional<StringRef> partSpelling =
                  asOptString(PartObj, "spelling", /*canBeNull=*/true);
              // The byte range is authoritative. A redundant part spelling is
              // accepted only when it exactly matches that slice.
              StringRef derivedSpelling =
                  spellingOrErr->slice(*byteBOrErr, *byteEOrErr);
              if (partSpelling && *partSpelling != derivedSpelling)
                REFOLD_LOG_FATAL(
                    "model",
                    "{0}: paste_tokens.part.spelling does not match byte range",
                    ctxItem);
              StringRef storedSpelling =
                  partSpelling ? *partSpelling : derivedSpelling;

              std::optional<uint32_t> argByteBegin =
                  asOptUInt32(PartObj, "arg_byte_begin", /*canBeNull=*/true);
              std::optional<uint32_t> argByteEnd =
                  asOptUInt32(PartObj, "arg_byte_end", /*canBeNull=*/true);
              if (argByteBegin.has_value() != argByteEnd.has_value())
                REFOLD_LOG_FATAL(
                    "model", "{0}: paste_tokens arg byte range is incomplete",
                    ctxItem);
              if (argByteBegin && *argByteEnd < *argByteBegin)
                REFOLD_LOG_FATAL("model",
                                 "{0}: paste_tokens arg byte range is invalid",
                                 ctxItem);
              if (kindLocal == RefoldModel::PastePartKind::Literal &&
                  argByteBegin)
                REFOLD_LOG_FATAL(
                    "model",
                    "{0}: literal paste_tokens part has arg byte range",
                    ctxItem);

              parts.push_back(RefoldModel::PastePart{
                  kindLocal, argIndex, *byteBOrErr, *byteEOrErr, storedSpelling,
                  argByteBegin, argByteEnd});
            }

            pasteTokens.push_back(
                RefoldModel::PasteToken{*spellingOrErr, std::move(parts)});
          }
        }

        std::optional<uint64_t> callerMacroId =
            asOptUInt64(*obj, "caller_macro_id", /*canBeNull=*/true);

        RefoldModel::MacroCalleeOrigin calleeOrigin;
        if (const json::Value *OriginVal = obj->get("callee_origin")) {
          auto originObjOrErr = asObject(*OriginVal, ctxItem);
          if (!originObjOrErr)
            REFOLD_LOG_FATAL("model", "{0}: callee_origin is not an object",
                             ctxItem);
          const json::Object &OriginObj = **originObjOrErr;

          auto kindOrErrLocal = applyToField(asString, OriginObj, "kind",
                                             ctxItem + ": callee_origin");
          if (!kindOrErrLocal)
            return kindOrErrLocal.takeError();

          if (*kindOrErrLocal == "literal_macro_name") {
            calleeOrigin.kind = MacroCalleeOriginKind::LiteralMacroName;
          } else if (*kindOrErrLocal == "caller_param") {
            calleeOrigin.kind = MacroCalleeOriginKind::CallerParam;
          } else if (*kindOrErrLocal == "paste") {
            calleeOrigin.kind = MacroCalleeOriginKind::Paste;
          } else if (*kindOrErrLocal == "opaque") {
            calleeOrigin.kind = MacroCalleeOriginKind::Opaque;
          } else {
            REFOLD_LOG_FATAL("model", "{0}: invalid callee_origin.kind '{1}'",
                             ctxItem, *kindOrErrLocal);
          }

          if (auto idxsArr = asOptArray(OriginObj, "caller_param_indices",
                                        /*allowNull=*/true)) {
            for (const json::Value &Elem : **idxsArr) {
              auto uOrErr = asUInt32(
                  Elem, ctxItem + ": callee_origin.caller_param_indices");
              if (!uOrErr)
                REFOLD_LOG_FATAL(
                    "model",
                    "{0}: callee_origin.caller_param_indices element is not "
                    "a uint32",
                    ctxItem);
              calleeOrigin.callerParamIndices.push_back(*uOrErr);
            }
          }

          // New producer maps can carry a direct decomposition of the final
          // callee token spelling.  Keep this proof material on the origin
          // record so selector substitution can consume producer-owned
          // paste/caller-slice evidence instead of reconstructing it from raw
          // source text.
          calleeOrigin.spelling =
              asOptString(OriginObj, "spelling", /*canBeNull=*/true);
          if (calleeOrigin.spelling)
            calleeOrigin.parts = parseCalleeOriginParts(
                OriginObj, *calleeOrigin.spelling, ctxItem);

          if (calleeOrigin.kind == MacroCalleeOriginKind::Paste &&
              (!calleeOrigin.spelling || calleeOrigin.parts.empty())) {
            REFOLD_LOG_FATAL(
                "model",
                "{0}: paste callee_origin is missing producer-owned spelling "
                "or parts",
                ctxItem);
          }
        } else if (callerMacroId) {
          // Nested invocations from older producer maps lack precise callee
          // provenance, so classify them as opaque instead of assuming a
          // literal macro-name owner.
          calleeOrigin.kind = MacroCalleeOriginKind::Opaque;
        } else {
          calleeOrigin.kind = MacroCalleeOriginKind::LiteralMacroName;
        }

        std::vector<std::vector<uint32_t>> argDeps;
        if (auto depsArr = asOptArray(*obj, "arg_deps", /*allowNull=*/true)) {
          for (const json::Value &Entry : **depsArr) {
            auto aOrErr = asArray(Entry, ctxItem);
            if (!aOrErr)
              REFOLD_LOG_FATAL("model", "{0}: arg_deps entry is not an array",
                               ctxItem);
            const json::Array &a = **aOrErr;

            std::vector<uint32_t> deps;
            deps.reserve(a.size());
            for (const json::Value &Elem : a) {
              auto uOrErr = asUInt32(Elem, ctxItem);
              if (!uOrErr)
                REFOLD_LOG_FATAL(
                    "model", "{0}: arg_deps element is not a uint32", ctxItem);
              deps.push_back(*uOrErr);
            }
            argDeps.push_back(std::move(deps));
          }
        }

        std::vector<std::vector<RefoldModel::InvArgRef>> argRefs;
        if (auto refsArr = asOptArray(*obj, "arg_refs", /*allowNull=*/true)) {
          for (const json::Value &Entry : **refsArr) {
            auto aOrErr = asArray(Entry, ctxItem);
            if (!aOrErr)
              REFOLD_LOG_FATAL("model", "{0}: arg_refs entry is not an array",
                               ctxItem);
            const json::Array &a = **aOrErr;

            std::vector<RefoldModel::InvArgRef> refs;
            refs.reserve(a.size());
            for (const json::Value &Elem : a) {
              auto objOrErrLocal = asObject(Elem, ctxItem);
              if (!objOrErrLocal)
                REFOLD_LOG_FATAL(
                    "model", "{0}: arg_refs element is not an object", ctxItem);
              const json::Object &RefObj = **objOrErrLocal;

              const json::Value *CallerIdxVal =
                  RefObj.get("caller_param_index");
              if (!CallerIdxVal)
                REFOLD_LOG_FATAL(
                    "model", "{0}: arg_refs element missing caller_param_index",
                    ctxItem);
              auto callerIdxOrErr = asUInt32(
                  *CallerIdxVal, ctxItem + ": arg_refs.caller_param_index");
              if (!callerIdxOrErr)
                REFOLD_LOG_FATAL(
                    "model", "{0}: arg_refs.caller_param_index is not a uint32",
                    ctxItem);

              const json::Value *ByteBVal = RefObj.get("byte_begin");
              if (!ByteBVal)
                REFOLD_LOG_FATAL("model",
                                 "{0}: arg_refs element missing byte_begin",
                                 ctxItem);
              auto byteBOrErr =
                  asUInt32(*ByteBVal, ctxItem + ": arg_refs.byte_begin");
              if (!byteBOrErr)
                REFOLD_LOG_FATAL("model",
                                 "{0}: arg_refs.byte_begin is not a uint32",
                                 ctxItem);

              const json::Value *ByteEVal = RefObj.get("byte_end");
              if (!ByteEVal)
                REFOLD_LOG_FATAL(
                    "model", "{0}: arg_refs element missing byte_end", ctxItem);
              auto byteEOrErr =
                  asUInt32(*ByteEVal, ctxItem + ": arg_refs.byte_end");
              if (!byteEOrErr)
                REFOLD_LOG_FATAL(
                    "model", "{0}: arg_refs.byte_end is not a uint32", ctxItem);

              refs.push_back(RefoldModel::InvArgRef{*callerIdxOrErr,
                                                    *byteBOrErr, *byteEOrErr});
            }
            argRefs.push_back(std::move(refs));
          }
        }

        std::vector<std::vector<RefoldModel::TupleArgRef>> argTupleRefs;
        if (auto refsArr =
                asOptArray(*obj, "arg_tuple_refs", /*allowNull=*/true)) {
          for (const json::Value &Entry : **refsArr) {
            auto aOrErr = asArray(Entry, ctxItem);
            if (!aOrErr) {
              REFOLD_LOG_FATAL("model",
                               "{0}: arg_tuple_refs entry is not an array",
                               ctxItem);
            }
            const json::Array &a = **aOrErr;

            std::vector<RefoldModel::TupleArgRef> refs;
            refs.reserve(a.size());
            for (const json::Value &Elem : a) {
              auto objOrErrLocal = asObject(Elem, ctxItem);
              if (!objOrErrLocal) {
                REFOLD_LOG_FATAL("model",
                                 "{0}: arg_tuple_refs element is not an object",
                                 ctxItem);
              }
              const json::Object &RefObj = **objOrErrLocal;

              const json::Value *CallerIdxVal =
                  RefObj.get("caller_param_index");
              if (!CallerIdxVal) {
                REFOLD_LOG_FATAL(
                    "model",
                    "{0}: arg_tuple_refs element missing caller_param_index",
                    ctxItem);
              }
              auto callerIdxOrErr =
                  asUInt32(*CallerIdxVal,
                           ctxItem + ": arg_tuple_refs.caller_param_index");
              if (!callerIdxOrErr) {
                REFOLD_LOG_FATAL(
                    "model",
                    "{0}: arg_tuple_refs.caller_param_index is not a uint32",
                    ctxItem);
              }

              const json::Value *ByteBVal = RefObj.get("caller_byte_begin");
              if (!ByteBVal) {
                REFOLD_LOG_FATAL(
                    "model",
                    "{0}: arg_tuple_refs element missing caller_byte_begin",
                    ctxItem);
              }
              auto byteBOrErr = asUInt32(
                  *ByteBVal, ctxItem + ": arg_tuple_refs.caller_byte_begin");
              if (!byteBOrErr) {
                REFOLD_LOG_FATAL(
                    "model",
                    "{0}: arg_tuple_refs.caller_byte_begin is not a uint32",
                    ctxItem);
              }

              const json::Value *ByteEVal = RefObj.get("caller_byte_end");
              if (!ByteEVal) {
                REFOLD_LOG_FATAL(
                    "model",
                    "{0}: arg_tuple_refs element missing caller_byte_end",
                    ctxItem);
              }
              auto byteEOrErr = asUInt32(
                  *ByteEVal, ctxItem + ": arg_tuple_refs.caller_byte_end");
              if (!byteEOrErr) {
                REFOLD_LOG_FATAL(
                    "model",
                    "{0}: arg_tuple_refs.caller_byte_end is not a uint32",
                    ctxItem);
              }

              refs.push_back(RefoldModel::TupleArgRef{
                  *callerIdxOrErr, *byteBOrErr, *byteEOrErr});
            }
            argTupleRefs.push_back(std::move(refs));
          }
        }

        MacroInvocation mi(/*id*/ id,
                           /*subkind*/ subkind,
                           /*name*/ name,
                           /*invText*/ invText,
                           /*normalizedInvText*/ normalizedInvText,
                           /*invFile*/ invFile,
                           /*invB*/ invB,
                           /*invE*/ invE,
                           /*invPPByteBegin*/ invPPByteBegin,
                           /*invPPByteEnd*/ invPPByteEnd,
                           /*ownerIncludeId*/ ownerIncludeId,
                           /*definitionDirectiveId*/ definitionDirectiveId,
                           /*defParams*/ std::move(defParams),
                           /*invArgRanges*/ std::move(invArgRanges),
                           /*normalizedInvArgTextRanges*/
                           std::move(normalizedInvArgTextRanges),
                           /*spans*/ std::move(*spans),
                           /*argSpans*/ std::move(argSpans),
                           /*stringifySpans*/ std::move(stringifySpans),
                           /*pasteSpans*/ std::move(pasteSpans),
                           /*pasteTokens*/ std::move(pasteTokens),
                           /*bodySpans*/ std::move(bodySpans),
                           /*callerMacroId*/ callerMacroId,
                           /*calleeOrigin*/ std::move(calleeOrigin),
                           /*argDeps*/ std::move(argDeps),
                           /*argRefs*/ std::move(argRefs),
                           /*argTupleRefs*/ std::move(argTupleRefs));

        model.macroInvs_.push_back(std::move(mi));
      } else if (kindStr == "file") {
        FileItem fi;

        const std::string ctxItem =
            (Twine("file items[") + Twine(i) + "]").str();

        // required
        auto idOrErr = applyToField(asUInt64, *obj, "id", ctxItem);
        if (!idOrErr)
          return idOrErr.takeError();
        fi.id = *idOrErr;

        auto pathOrErr = applyToField(asString, *obj, "path", ctxItem);
        if (!pathOrErr)
          return pathOrErr.takeError();
        fi.path = *pathOrErr;

        auto spansOrErr = requireField(*obj, "spans", ctxItem);
        if (!spansOrErr)
          return spansOrErr.takeError();
        auto spans = parseSpans(**spansOrErr, "file.spans");
        if (!spans)
          return spans.takeError();
        fi.spans = std::move(*spans);

        model.fileItems_.push_back(std::move(fi));
      } else {
        return createStringError(inconvertibleErrorCode(),
                                 "Unknown item.kind '%s' at items[%zu]",
                                 kindStr.str().c_str(), i);
      }
    }
  }

  // slots
  {
    auto arrOrErr = applyToField(asArray, root, "slots");
    if (!arrOrErr)
      return arrOrErr.takeError();
    const json::Array *arr = *arrOrErr;

    model.slots_.reserve(arr->size());
    for (std::size_t i = 0; i < arr->size(); ++i) {
      const std::string ctxItem = (Twine("slots[") + Twine(i) + "]").str();

      auto objOrErr = arrayObjElemAt(*arr, i, ctxItem);
      if (!objOrErr)
        return objOrErr.takeError();
      const json::Object *obj = *objOrErr;

      Slot slot;

      // required
      auto idOrErr = applyToField(asUInt64, *obj, "id", ctxItem);
      if (!idOrErr)
        return idOrErr.takeError();
      slot.id = *idOrErr;

      auto fileOrErr = applyToField(asString, *obj, "file", ctxItem);
      if (!fileOrErr)
        return fileOrErr.takeError();
      slot.file = *fileOrErr;

      auto kindOrErr = applyToField(asString, *obj, "kind", ctxItem);
      if (!kindOrErr)
        return kindOrErr.takeError();
      slot.kind = *kindOrErr;

      auto bOrErr = applyToField(asUInt64, *obj, "b", ctxItem);
      if (!bOrErr)
        return bOrErr.takeError();
      slot.b = *bOrErr;

      auto eOrErr = applyToField(asUInt64, *obj, "e", ctxItem);
      if (!eOrErr)
        return eOrErr.takeError();
      slot.e = *eOrErr;

      // optionals
      slot.pp = asOptUInt64(*obj, "pp");
      slot.ref = asOptUInt64(*obj, "ref");
      slot.ownerIncludeId = asOptUInt64(*obj, "owner_include_id");

      model.slots_.push_back(std::move(slot));
    }
  }

  // line_controls (optional; maps produced before schema 2.6 do not contain it)
  if (auto arr = asOptArray(root, "line_controls")) {
    model.lineControls_.reserve((*arr)->size());
    for (std::size_t i = 0; i < (*arr)->size(); ++i) {
      const std::string ctxItem =
          (Twine("line_controls[") + Twine(i) + "]").str();

      auto objOrErr = arrayObjElemAt(**arr, i, ctxItem);
      if (!objOrErr)
        return objOrErr.takeError();
      const json::Object *obj = *objOrErr;

      LineControlEvent event;

      auto idOrErr = applyToField(asUInt64, *obj, "id", ctxItem);
      if (!idOrErr)
        return idOrErr.takeError();
      event.id = *idOrErr;

      auto fileOrErr = applyToField(asString, *obj, "physical_file", ctxItem);
      if (!fileOrErr)
        return fileOrErr.takeError();
      event.physicalFile = *fileOrErr;

      event.siteB = asOptUInt64(*obj, "site_b", /*canBeNull=*/true);
      event.siteE = asOptUInt64(*obj, "site_e", /*canBeNull=*/true);

      auto activeOrErr = applyToField(asBool, *obj, "active", ctxItem);
      if (!activeOrErr)
        return activeOrErr.takeError();
      event.active = *activeOrErr;

      auto provenOrErr = applyToField(asBool, *obj, "producer_proven", ctxItem);
      if (!provenOrErr)
        return provenOrErr.takeError();
      event.producerProven = *provenOrErr;

      auto lineOrErr =
          applyToField(asUInt64, *obj, "logical_line_after", ctxItem);
      if (!lineOrErr)
        return lineOrErr.takeError();
      event.logicalLineAfter = *lineOrErr;

      auto logicalFileOrErr =
          applyToField(asString, *obj, "logical_file_after", ctxItem);
      if (!logicalFileOrErr)
        return logicalFileOrErr.takeError();
      event.logicalFileAfter = *logicalFileOrErr;

      event.ownerIncludeId = asOptUInt64(*obj, "owner_include_id");
      if (auto text = asOptString(*obj, "text"))
        event.text = *text;

      if (event.siteB && event.siteE && *event.siteE < *event.siteB)
        return createStringError(inconvertibleErrorCode(),
                                 "Invalid line-control site range at %s",
                                 ctxItem.c_str());

      model.lineControls_.push_back(std::move(event));
    }

    std::sort(model.lineControls_.begin(), model.lineControls_.end(),
              [](const LineControlEvent &a, const LineControlEvent &bLocal) {
                if (a.physicalFile != bLocal.physicalFile)
                  return a.physicalFile < bLocal.physicalFile;
                if (a.ownerIncludeId != bLocal.ownerIncludeId)
                  return a.ownerIncludeId < bLocal.ownerIncludeId;
                if (a.siteB != bLocal.siteB)
                  return a.siteB < bLocal.siteB;
                if (a.siteE != bLocal.siteE)
                  return a.siteE < bLocal.siteE;
                return a.id < bLocal.id;
              });
  }

  // conds
  {
    auto arrOrErr = applyToField(asArray, root, "conds");
    if (!arrOrErr)
      return arrOrErr.takeError();
    const json::Array *arr = *arrOrErr;

    model.conds_.reserve(arr->size());
    for (std::size_t i = 0; i < arr->size(); ++i) {
      const std::string ctxItem = (Twine("conds[") + Twine(i) + "]").str();

      auto objOrErr = arrayObjElemAt(*arr, i, ctxItem);
      if (!objOrErr)
        return objOrErr.takeError();
      const json::Object *obj = *objOrErr;

      CondGroup group;

      // required
      auto idOrErr = applyToField(asUInt64, *obj, "id", ctxItem);
      if (!idOrErr)
        return idOrErr.takeError();
      group.id = *idOrErr;

      auto fileOrErr = applyToField(asString, *obj, "file", ctxItem);
      if (!fileOrErr)
        return fileOrErr.takeError();
      group.file = *fileOrErr;

      auto gbOrErr = applyToField(asUInt64, *obj, "group_b", ctxItem);
      if (!gbOrErr)
        return gbOrErr.takeError();
      group.groupB = *gbOrErr;

      auto geOrErr = applyToField(asUInt64, *obj, "group_e", ctxItem);
      if (!geOrErr)
        return geOrErr.takeError();
      group.groupE = *geOrErr;

      // optional
      group.parentArmId = asOptUInt64(*obj, "parent_arm_id");
      group.parentIncludeId = asOptUInt64(*obj, "parent_include_id");

      // arms
      auto armsOrErr = applyToField(asArray, *obj, "arms", ctxItem);
      if (!armsOrErr)
        return armsOrErr.takeError();
      const json::Array *arms = *armsOrErr;

      group.arms.reserve(arms->size());
      for (std::size_t ai = 0; ai < arms->size(); ++ai) {
        const std::string ctxItem = (Twine("arm[") + Twine(ai) + "]").str();

        auto objOrErr = arrayObjElemAt(*arms, ai, ctxItem);
        if (!objOrErr)
          return objOrErr.takeError();
        const json::Object *armObj = *objOrErr;

        CondArm arm;
        arm.groupId = group.id;

        // required
        auto idOrErr = applyToField(asUInt64, *armObj, "id", ctxItem);
        if (!idOrErr)
          return idOrErr.takeError();
        arm.id = *idOrErr;

        auto kindOrErr = applyToField(asString, *armObj, "kind", ctxItem);
        if (!kindOrErr)
          return kindOrErr.takeError();
        arm.kind = *kindOrErr;

        auto bbOrErr = applyToField(asUInt64, *armObj, "body_b", ctxItem);
        if (!bbOrErr)
          return bbOrErr.takeError();
        arm.bodyB = *bbOrErr;

        auto beOrErr = applyToField(asUInt64, *armObj, "body_e", ctxItem);
        if (!beOrErr)
          return beOrErr.takeError();
        arm.bodyE = *beOrErr;

        auto selOrErr = applyToField(asBool, *armObj, "selected", ctxItem);
        if (!selOrErr)
          return selOrErr.takeError();
        arm.selected = *selOrErr;

        // optional
        arm.cond = asOptString(*armObj, "cond");

        if (const json::Value *ppSpanVal = armObj->get("pp_span")) {
          if (!ppSpanVal->getAsNull()) {
            auto ppSpanObjOrErr = asObject(*ppSpanVal, ctxItem + ".pp_span");
            if (!ppSpanObjOrErr)
              return ppSpanObjOrErr.takeError();
            const json::Object &ppSpanObj = **ppSpanObjOrErr;

            auto beginOrErr = applyToField(asUInt64, ppSpanObj, "begin",
                                           ctxItem + ".pp_span");
            if (!beginOrErr)
              return beginOrErr.takeError();
            auto endOrErr =
                applyToField(asUInt64, ppSpanObj, "end", ctxItem + ".pp_span");
            if (!endOrErr)
              return endOrErr.takeError();
            arm.span = PPSpan{*beginOrErr, *endOrErr};
          }
        }

        group.arms.push_back(std::move(arm));
      }

      model.conds_.push_back(std::move(group));
    }
  }

  // Deterministic ordering & indices
  model.SanitizeMacroCallerGraph();
  model.SanitizeMacroProofArtifacts();
  model.BuildIndicesAndSort();
  if (Error auditErr = validateIncludeMetadataAudit(model))
    return std::move(auditErr);
  model.CompleteIncludeNextDerivedProvenance();
  return model;
}

RefoldModel RefoldModel::CloneForReadOnlyConsumer() const {
  RefoldModel clone;
  clone.root_ = root_;
  clone.version_ = version_;
  clone.sourcePath_ = sourcePath_;
  clone.ppCwd_ = ppCwd_;
  clone.ppLang_ = ppLang_;
  clone.ppArgv_ = ppArgv_;
  clone.includeSearchChain_ = includeSearchChain_;
  clone.tokensCountA_ = tokensCountA_;
  clone.tokPPByteBeginA_ = tokPPByteBeginA_;
  clone.tokPPByteEndA_ = tokPPByteEndA_;
  clone.tokmapByPP_ = tokmapByPP_;
  clone.tokmap_ = tokmap_;
  clone.includes_ = includes_;
  clone.macroInvs_ = macroInvs_;
  clone.macroDirs_ = macroDirs_;
  clone.pragmas_ = pragmas_;
  clone.fileItems_ = fileItems_;
  clone.slots_ = slots_;
  clone.conds_ = conds_;
  clone.lineControls_ = lineControls_;

  // Rebuild all pointer-bearing and ordering-sensitive derived state against
  // the clone's own vectors.  The producer-backed StringRefs intentionally
  // continue to refer to the immutable JSON root, whose lifetime already
  // dominates every RefoldEngine created from this model.
  clone.BuildIndicesAndSort();
  return clone;
}

void RefoldModel::CompleteIncludeNextDerivedProvenance() {
  // The producer schema stores #include_next provenance in normalized form:
  //   * include_next.containing_file_include_id names the containing edge;
  //   * include_next.resume_search_chain_index stores the resume cursor;
  //   * this edge's lookup.search_chain_index stores the selected target;
  //   * the containing edge's lookup.search_chain_index stores the containing
  //     file's selected search position.
  //
  // Include-next replay proofs need all four values together.  Materializing
  // the two derived indices here avoids repeating id lookups and, more
  // importantly, preserves the old-map contract: absent or unknown provenance
  // stays empty and must be handled by the conservative include_next fallback.
  for (IncludeItem &include : includes_) {
    if (!include.includeNext)
      continue;

    include.includeNext->containingFileSearchChainIndex.reset();
    include.includeNext->selectedSearchChainIndex.reset();

    if (include.subkind != "#include_next" || !include.includeNext->known)
      continue;

    if (include.lookup)
      include.includeNext->selectedSearchChainIndex =
          include.lookup->searchChainIndex;

    if (!include.includeNext->containingFileIncludeId)
      continue;

    const IncludeItem *containing =
        GetIncludeById(*include.includeNext->containingFileIncludeId);
    if (containing && containing->lookup)
      include.includeNext->containingFileSearchChainIndex =
          containing->lookup->searchChainIndex;
  }
}

void RefoldModel::SanitizeMacroCallerGraph() {
  // Build an id lookup so caller_macro_id edges can be validated without
  // repeatedly scanning the invocation vector.
  DenseMap<uint64_t, MacroInvocation *> macroById;
  macroById.reserve(macroInvs_.size());
  for (MacroInvocation &mi : macroInvs_)
    macroById.try_emplace(mi.id, &mi);

  DenseSet<uint64_t> done;

  for (MacroInvocation &mi : macroInvs_) {
    if (done.find(mi.id) != done.end())
      continue;

    // Walk this invocation's caller chain while tracking the current DFS path.
    // PathIndex lets us distinguish a real cycle from a chain that simply
    // reaches a node already sanitized by an earlier walk.
    std::vector<uint64_t> path;
    DenseMap<uint64_t, unsigned> pathIndex;
    uint64_t cur = mi.id;

    while (true) {
      if (done.find(cur) != done.end())
        break;

      auto inserted = pathIndex.try_emplace(cur, path.size());
      if (!inserted.second) {
        // A caller chain must be a tree/forest edge toward an outer invocation.
        // If it loops back into the current path, drop the caller edge from
        // every node in the cycle so later upward walks cannot recurse forever.
        const unsigned cycleBegin = inserted.first->second;
        for (unsigned i = cycleBegin; i < path.size(); ++i) {
          MacroInvocation *cycleNode = macroById.lookup(path[i]);
          if (!cycleNode || !cycleNode->callerMacroId)
            continue;
          REFOLD_LOG_WARN(
              "model",
              "dropping cyclic caller_macro_id edge child={0} parent={1}",
              cycleNode->id, *cycleNode->callerMacroId);
          cycleNode->callerMacroId.reset();
        }
        break;
      }

      path.push_back(cur);

      auto curIt = macroById.find(cur);
      if (curIt == macroById.end())
        break;

      MacroInvocation *node = curIt->second;
      if (!node->callerMacroId)
        break;

      const uint64_t parentId = *node->callerMacroId;
      if (parentId == cur || macroById.find(parentId) == macroById.end()) {
        // Self-edges and references to missing invocations cannot be used as
        // structural caller links, so remove them before proof construction.
        REFOLD_LOG_WARN(
            "model",
            "dropping invalid caller_macro_id edge child={0} parent={1}",
            node->id, parentId);
        node->callerMacroId.reset();
        break;
      }

      cur = parentId;
    }

    for (uint64_t idLocal : path)
      done.insert(idLocal);
  }
}

void RefoldModel::SanitizeMacroProofArtifacts() {
  // Build an id -> invocation lookup once so that per-invocation validation can
  // cheaply consult the caller macro when caller-relative proof metadata
  // exists.
  DenseMap<uint64_t, MacroInvocation *> macroById;
  macroById.reserve(macroInvs_.size());
  for (MacroInvocation &mi : macroInvs_)
    macroById.try_emplace(mi.id, &mi);

  // Raw proof that depends on exact raw invocation spelling:
  //   - argDeps: caller-param provenance for each raw argument slice
  //   - argRefs: byte ranges inside this invocation's raw invText that witness
  //              where each caller-derived contribution landed
  auto clearRawProofRefsOnly = [](MacroInvocation &mi) {
    mi.argDeps.clear();
    mi.argRefs.clear();
  };

  // Normalized proof that depends on normalized invocation spelling:
  //   - normalizedInvArgTextRanges: per-argument ranges in normalizedInvText
  //   - argTupleRefs: caller-relative tuple forwarding metadata
  auto clearNormalizedProofRefsOnly = [](MacroInvocation &mi) {
    mi.normalizedInvArgTextRanges.clear();
    mi.argTupleRefs.clear();
  };

  for (MacroInvocation &mi : macroInvs_) {
    // Recover the caller invocation if it is still present in the model.
    // Caller presence matters because several proof artifacts store caller
    // parameter indices and are meaningless if the caller cannot be resolved.
    const MacroInvocation *caller = nullptr;
    if (mi.callerMacroId) {
      auto it = macroById.find(*mi.callerMacroId);
      if (it != macroById.end())
        caller = it->second;
    }

    // ----- Raw invocation proof validation -----
    //
    // Raw proof is only trustworthy if we still have:
    //   1. exact raw invocation text (`invText`)
    //   2. the source begin offset for that text (`invB`)
    //
    // If either is missing, any raw ranges/refs become unverifiable, so drop
    // them fail-closed.
    if (!mi.invText || !mi.invB) {
      if (!mi.invArgRanges.empty() || !mi.argDeps.empty() ||
          !mi.argRefs.empty()) {
        REFOLD_LOG_WARN(
            "model",
            "dropping raw invocation proof for macro id={0}: exact raw "
            "invocation text is unavailable",
            mi.id);
        mi.invArgRanges.clear();
        clearRawProofRefsOnly(mi);
      }
    } else {
      const uint64_t invBegin = *mi.invB;
      const uint64_t invTextSize = mi.invText->size();
      bool dropRawRanges = false;
      bool dropRawRefs = false;

      // Each raw argument range is stored in absolute source coordinates and
      // must lie wholly within the raw invocation spelling [invB,
      // invB+|invText|).
      for (const auto &rng : mi.invArgRanges) {
        if (!rng.first && !rng.second)
          continue;
        if (!rng.first || !rng.second || *rng.second < *rng.first ||
            *rng.first < invBegin || (*rng.second - invBegin) > invTextSize) {
          dropRawRanges = true;
          break;
        }
      }

      if (dropRawRanges) {
        REFOLD_LOG_WARN(
            "model",
            "dropping inconsistent raw invocation ranges for macro id={0}",
            mi.id);
        mi.invArgRanges.clear();
        clearRawProofRefsOnly(mi);
      } else {
        // argDeps / argRefs are parallel to invArgRanges: one entry per callee
        // argument. A size mismatch means the proof shape itself is corrupted.
        if ((!mi.argDeps.empty() &&
             mi.argDeps.size() != mi.invArgRanges.size()) ||
            (!mi.argRefs.empty() &&
             mi.argRefs.size() != mi.invArgRanges.size())) {
          dropRawRefs = true;
        }

        // Caller-param references are only valid if every referenced parameter
        // index exists in the resolved caller macro.
        if (!dropRawRefs && caller) {
          for (const auto &deps : mi.argDeps) {
            for (uint32_t dep : deps) {
              if (dep >= caller->defParams.size()) {
                dropRawRefs = true;
                break;
              }
            }
            if (dropRawRefs)
              break;
          }
        }

        if (!dropRawRefs) {
          for (size_t argIdx = 0; argIdx < mi.argRefs.size() && !dropRawRefs;
               ++argIdx) {
            if (argIdx >= mi.invArgRanges.size()) {
              dropRawRefs = true;
              break;
            }
            const auto &rng = mi.invArgRanges[argIdx];

            // A missing raw range means we should not have any byte-level refs
            // for that argument.
            if (!rng.first || !rng.second) {
              if (!mi.argRefs[argIdx].empty())
                dropRawRefs = true;
              continue;
            }

            // argRefs are recorded in callee-local invText coordinates, so
            // rebase the absolute raw argument span into [0, |invText|) before
            // checking containment.
            const uint64_t relB = *rng.first - invBegin;
            const uint64_t relE = *rng.second - invBegin;
            for (const InvArgRef &ref : mi.argRefs[argIdx]) {
              if (ref.byteEnd < ref.byteBegin || ref.byteEnd > invTextSize ||
                  ref.byteBegin < relB || ref.byteEnd > relE) {
                dropRawRefs = true;
                break;
              }
              if (caller && ref.callerParamIndex >= caller->defParams.size()) {
                dropRawRefs = true;
                break;
              }
            }
          }
        }

        if (dropRawRefs) {
          REFOLD_LOG_WARN(
              "model",
              "dropping inconsistent raw invocation dependency/ref proof for "
              "macro id={0}",
              mi.id);
          clearRawProofRefsOnly(mi);
        }
      }
    }

    // ----- Normalized invocation proof validation -----
    //
    // Normalized proof is independent from raw spelling, but it still requires
    // normalizedInvText to exist. Without that text, the normalized
    // ranges/tuple refs cannot be validated or replayed safely.
    if (!mi.normalizedInvText) {
      if (!mi.normalizedInvArgTextRanges.empty() || !mi.argTupleRefs.empty()) {
        REFOLD_LOG_WARN(
            "model",
            "dropping normalized invocation proof for macro id={0}: "
            "normalized_inv_text is unavailable",
            mi.id);
        clearNormalizedProofRefsOnly(mi);
      }
    } else {
      const uint64_t normalizedSize = mi.normalizedInvText->size();
      bool dropNormalizedRanges = false;
      bool dropTupleRefs = false;

      // Normalized argument ranges are stored directly in normalized-text
      // coordinates, so they only need to fit within [0, |normalizedInvText|).
      for (const auto &rng : mi.normalizedInvArgTextRanges) {
        if (!rng.first && !rng.second)
          continue;
        if (!rng.first || !rng.second || *rng.second < *rng.first ||
            *rng.second > normalizedSize) {
          dropNormalizedRanges = true;
          break;
        }
      }

      if (dropNormalizedRanges) {
        REFOLD_LOG_WARN(
            "model",
            "dropping inconsistent normalized invocation ranges for macro "
            "id={0}",
            mi.id);
        clearNormalizedProofRefsOnly(mi);
      } else {
        // Tuple refs are parallel to normalized argument ranges.
        if (!mi.argTupleRefs.empty() &&
            mi.argTupleRefs.size() != mi.normalizedInvArgTextRanges.size()) {
          dropTupleRefs = true;
        }

        if (!dropTupleRefs) {
          for (const auto &argRefsLocal : mi.argTupleRefs) {
            for (const TupleArgRef &ref : argRefsLocal) {
              if (ref.callerByteEnd < ref.callerByteBegin) {
                dropTupleRefs = true;
                break;
              }
              if (caller && ref.callerParamIndex >= caller->defParams.size()) {
                dropTupleRefs = true;
                break;
              }
            }
            if (dropTupleRefs)
              break;
          }
        }

        if (dropTupleRefs) {
          REFOLD_LOG_WARN(
              "model",
              "dropping inconsistent normalized invocation tuple proof for "
              "macro id={0}",
              mi.id);
          mi.argTupleRefs.clear();
        }
      }
    }

    // ----- Caller-dependent provenance cleanup -----
    //
    // calleeOrigin.callerParamIndices and the caller-relative proof artifacts
    // below are only meaningful if the caller exists and the referenced caller
    // parameter indices are valid.
    if (caller) {
      bool opaqueOrigin = false;
      for (uint32_t idx : mi.calleeOrigin.callerParamIndices) {
        if (idx >= caller->defParams.size()) {
          opaqueOrigin = true;
          break;
        }
      }
      if (opaqueOrigin) {
        REFOLD_LOG_WARN(
            "model",
            "downgrading invalid callee_origin caller-param metadata for "
            "macro id={0} to opaque",
            mi.id);
        mi.calleeOrigin.kind = MacroCalleeOriginKind::Opaque;
        mi.calleeOrigin.callerParamIndices.clear();
        mi.calleeOrigin.spelling.reset();
        mi.calleeOrigin.parts.clear();
      }
    }

    // The segmented callee-origin proof names a root invocation argument slice.
    // Validate that every caller_arg_slice part points at a real invocation and
    // a real formal parameter before selector-rewrite logic treats it as a
    // rewrite witness.  The model does not attempt to repair malformed producer
    // data; it simply downgrades the callee origin so existing conservative
    // paths remain authoritative.
    bool dropSegmentedOrigin = false;
    for (const CalleeOriginPart &part : mi.calleeOrigin.parts) {
      if (part.kind != CalleeOriginPartKind::CallerArgSlice)
        continue;
      if (!part.rootMacroId || !part.rootParamIndex || !part.byteBegin ||
          !part.byteEnd || *part.byteEnd < *part.byteBegin ||
          (*part.byteEnd - *part.byteBegin) != part.spelling.size()) {
        dropSegmentedOrigin = true;
        break;
      }

      auto rootIt = macroById.find(*part.rootMacroId);
      if (rootIt == macroById.end() ||
          *part.rootParamIndex >= rootIt->second->defParams.size()) {
        dropSegmentedOrigin = true;
        break;
      }
    }
    if (dropSegmentedOrigin) {
      REFOLD_LOG_WARN(
          "model",
          "downgrading invalid segmented callee_origin proof for macro id={0} "
          "to opaque",
          mi.id);
      mi.calleeOrigin.kind = MacroCalleeOriginKind::Opaque;
      mi.calleeOrigin.callerParamIndices.clear();
      mi.calleeOrigin.spelling.reset();
      mi.calleeOrigin.parts.clear();
    } else if (mi.calleeOrigin.spelling && !mi.calleeOrigin.parts.empty()) {
      std::string tiled;
      for (const CalleeOriginPart &part : mi.calleeOrigin.parts)
        tiled.append(part.spelling.begin(), part.spelling.end());
      if (tiled != *mi.calleeOrigin.spelling) {
        REFOLD_LOG_WARN(
            "model",
            "downgrading non-tiling segmented callee_origin proof for macro "
            "id={0} to opaque",
            mi.id);
        mi.calleeOrigin.kind = MacroCalleeOriginKind::Opaque;
        mi.calleeOrigin.callerParamIndices.clear();
        mi.calleeOrigin.spelling.reset();
        mi.calleeOrigin.parts.clear();
      }
    }

    if (!caller) {
      // If the caller is gone, degrade any caller-dependent origin metadata to
      // opaque and drop caller-relative proof that cannot be validated anymore.
      if (!mi.calleeOrigin.callerParamIndices.empty()) {
        REFOLD_LOG_WARN(
            "model",
            "downgrading callee_origin caller-param metadata for macro "
            "id={0}: caller invocation is unavailable",
            mi.id);
        mi.calleeOrigin.kind = MacroCalleeOriginKind::Opaque;
        mi.calleeOrigin.callerParamIndices.clear();
        mi.calleeOrigin.spelling.reset();
        mi.calleeOrigin.parts.clear();
      }
      if (!mi.calleeOrigin.parts.empty() ||
          mi.calleeOrigin.kind == MacroCalleeOriginKind::Paste) {
        REFOLD_LOG_WARN(
            "model",
            "dropping segmented callee_origin proof for macro id={0}: "
            "caller invocation is unavailable",
            mi.id);
        mi.calleeOrigin.kind = MacroCalleeOriginKind::Opaque;
        mi.calleeOrigin.spelling.reset();
        mi.calleeOrigin.parts.clear();
      }
      if (!mi.argDeps.empty() || !mi.argRefs.empty()) {
        REFOLD_LOG_WARN(
            "model",
            "dropping raw invocation dependency/ref proof for macro id={0}: "
            "caller invocation is unavailable",
            mi.id);
        clearRawProofRefsOnly(mi);
      }
      if (!mi.argTupleRefs.empty()) {
        REFOLD_LOG_WARN(
            "model",
            "dropping normalized invocation tuple proof for macro id={0}: "
            "caller invocation is unavailable",
            mi.id);
        mi.argTupleRefs.clear();
      }
    }
  }
}

void RefoldModel::BuildIndicesAndSort() {
  // Invalidate caches/indices derived from the raw parsed vectors.
  includeDepthCache_.clear();
  condGroupDepthCache_.clear();

  condGroupById_.clear();
  armById_.clear();
  segmentsByFile_.clear();
  macroDirById_.clear();
  macroDirsByName_.clear();

  // Some producer paths emit include items with an empty `spans` array even
  // though the slot stream already carries exact file_begin/file_end ownership
  // for the included file. That leaves the include with an invalid PP cover,
  // which in turn makes PP-only include ownership recovery fail for edits that
  // fall entirely inside the included header. Recover the missing PP cover
  // deterministically from the include-owned file_begin/file_end slots.
  DenseMap<uint64_t, uint64_t> includeBeginPPById;
  DenseMap<uint64_t, uint64_t> includeEndPPById;
  for (const auto &slot : slots_) {
    if (!slot.ownerIncludeId || !slot.pp)
      continue;
    if (slot.kind == "file_begin") {
      auto it = includeBeginPPById.find(*slot.ownerIncludeId);
      if (it == includeBeginPPById.end() || *slot.pp < it->second)
        includeBeginPPById[*slot.ownerIncludeId] = *slot.pp;
    } else if (slot.kind == "file_end") {
      auto it = includeEndPPById.find(*slot.ownerIncludeId);
      if (it == includeEndPPById.end() || *slot.pp > it->second)
        includeEndPPById[*slot.ownerIncludeId] = *slot.pp;
    }
  }
  for (auto &inc : includes_) {
    if (inc.cover.IsValid())
      continue;
    auto bIt = includeBeginPPById.find(inc.id);
    auto eIt = includeEndPPById.find(inc.id);
    if (bIt == includeBeginPPById.end() || eIt == includeEndPPById.end())
      continue;
    if (eIt->second <= bIt->second)
      continue;
    inc.spans.push_back(PPSpan{bIt->second, eIt->second});
    inc.cover.Init(inc.spans);
  }

  // includes: sort by (sitePath, siteB)
  std::sort(includes_.begin(), includes_.end(),
            [](const IncludeItem &a, const IncludeItem &b) {
              if (a.sitePath < b.sitePath)
                return true;
              if (a.sitePath > b.sitePath)
                return false;
              return a.siteB < b.siteB;
            });

  // include index
  includeById_.clear();
  for (const auto &inc : includes_)
    includeById_[inc.id] = &inc;

  for (const auto &inc : includes_) {
    if (inc.subkind == "#include" && inc.includeNext)
      REFOLD_LOG_FATAL("model",
                       "ordinary #include item id={0} carries include_next "
                       "provenance",
                       inc.id);

    if (!inc.includeNext || !inc.includeNext->known)
      continue;

    if (!inc.includeNext->containingFileIncludeId ||
        !inc.includeNext->resumeSearchChainIndex)
      REFOLD_LOG_FATAL(
          "model",
          "#include_next item id={0} has known provenance without containing "
          "include id and resume cursor",
          inc.id);

    if (!includeById_.count(*inc.includeNext->containingFileIncludeId))
      REFOLD_LOG_FATAL(
          "model",
          "#include_next item id={0} references missing containing include "
          "id={1}",
          inc.id, *inc.includeNext->containingFileIncludeId);

    if (inc.lookup && inc.lookup->searchChainIndex &&
        *inc.lookup->searchChainIndex <
            *inc.includeNext->resumeSearchChainIndex)
      REFOLD_LOG_FATAL(
          "model",
          "#include_next item id={0} selected search-chain index {1} before "
          "producer resume index {2}",
          inc.id, *inc.lookup->searchChainIndex,
          *inc.includeNext->resumeSearchChainIndex);
  }

  // macro-directive id and macro-state-key indices.  `macroDirs_` is never
  // reordered, so walking it in producer record order gives every name bucket
  // the same relative directive order a filtered linear scan would observe.
  macroDirById_.reserve(macroDirs_.size());
  for (const auto &directive : macroDirs_) {
    // `try_emplace` keeps the first record for a repeated id, which is exactly
    // what the linear `directive.id == wanted` scans this index replaces did.
    macroDirById_.try_emplace(directive.id, &directive);
    macroDirsByName_[directive.name].push_back(&directive);
  }

  // condsByFile / condsByFileByOwner
  condsByFile_.clear();
  condsByFileByOwner_.clear();

  for (const auto &group : conds_) {
    condGroupById_[group.id] = &group;
    for (const auto &arm : group.arms)
      armById_[arm.id] = ArmRef{&group, &arm};

    condsByFile_[group.file].push_back(&group);
    if (group.parentIncludeId) {
      condsByFileByOwner_[group.file][*group.parentIncludeId].push_back(&group);
    }
  }

  auto sortGroups = [](std::vector<const CondGroup *> &groups) {
    std::sort(groups.begin(), groups.end(),
              [](const CondGroup *a, const CondGroup *b) {
                return a->groupB < b->groupB;
              });
  };

  for (auto &kv : condsByFile_)
    sortGroups(kv.second);
  for (auto &kv : condsByFileByOwner_) {
    for (auto &kv2 : kv.second)
      sortGroups(kv2.second);
  }

  // segmentsByFile: build from slots (one segment list per file)
  StringMap<std::vector<const Slot *>> slotsByFile;
  for (const auto &slot : slots_)
    slotsByFile[slot.file].push_back(&slot);
  for (auto &kv : slotsByFile) {
    auto segs = BuildSegmentsForFile(kv.getKey(), kv.getValue());
    if (!segs.empty()) {
      segmentsByFile_[kv.getKey()] = std::move(segs);
    }
  }
}

// ====================== Query helpers (ported behavior) ======================

std::vector<const RefoldModel::CondGroup *>
RefoldModel::GetCondGroups(StringRef file,
                           std::optional<uint64_t> parentIncludeId) const {
  // Conditional groups are indexed separately for TU-level conditionals and
  // include-owned conditionals. Pick the index that matches the requested owner
  // domain so callers do not accidentally mix arms from another expansion site.
  if (!parentIncludeId.has_value()) {
    auto it = condsByFile_.find(file);
    if (it != condsByFile_.end())
      return it->second;
    return {};
  }

  auto fit = condsByFileByOwner_.find(file);
  if (fit == condsByFileByOwner_.end())
    return {};
  auto oit = fit->second.find(*parentIncludeId);
  if (oit == fit->second.end())
    return {};
  return oit->second;
}

std::optional<RefoldModel::ArmRef>
RefoldModel::FindArmRefForByte(StringRef file,
                               std::optional<uint64_t> parentIncludeId,
                               uint64_t byteOffset) const {
  std::optional<ArmRef> best;
  uint32_t bestDepth = 0;

  for (const CondGroup *group : GetCondGroups(file, parentIncludeId)) {
    if (!group || !group->ContainsByte(byteOffset))
      continue;

    for (const CondArm &arm : group->arms) {
      // Only selected arms can describe emitted source when selection metadata
      // is available.
      if (!arm.selected)
        continue;
      if (!arm.ContainsByte(byteOffset))
        continue;

      // Prefer the deepest selected arm containing the byte. Nested
      // conditionals should resolve to the most local arm witness, not an outer
      // enclosing arm.
      uint32_t depth = GetCondArmDepth(arm.id);
      if (depth > bestDepth) {
        bestDepth = depth;
        best = ArmRef{group, &arm};
      }
    }
  }

  return best;
}

std::optional<const RefoldModel::Slot *>
RefoldModel::GetArmBeginSlot(uint64_t armId) const {
  // When the arm is indexed, search in its exact file/include-owner domain
  // first so duplicate arm ids or slots from other materialization contexts
  // cannot satisfy the lookup.
  if (auto ref = GetArmRefById(armId)) {
    auto slots = FindSlots(ref->group->file, "arm_begin", armId,
                           ref->group->parentIncludeId);
    if (!slots.empty())
      return slots.front();
    return std::nullopt;
  }

  // Fallback for older or partially indexed metadata where the arm ref was not
  // recorded but the slot table still carries the arm id.
  auto slots = FindSlots(std::nullopt, "arm_begin", armId, std::nullopt);
  if (!slots.empty())
    return slots.front();
  return std::nullopt;
}

std::optional<const RefoldModel::Slot *>
RefoldModel::GetArmEndSlot(uint64_t armId) const {
  // When the arm is indexed, search in its exact file/include-owner domain
  // first so duplicate arm ids or slots from other materialization contexts
  // cannot satisfy the lookup.
  if (auto ref = GetArmRefById(armId)) {
    auto slots = FindSlots(ref->group->file, "arm_end", armId,
                           ref->group->parentIncludeId);
    if (!slots.empty())
      return slots.front();
    return std::nullopt;
  }

  // Fallback for older or partially indexed metadata where the arm ref was not
  // recorded but the slot table still carries the arm id.
  auto slots = FindSlots(std::nullopt, "arm_end", armId, std::nullopt);
  if (!slots.empty())
    return slots.front();
  return std::nullopt;
}

std::vector<RefoldModel::Segment>
RefoldModel::BuildSegmentsForFile(StringRef file,
                                  ArrayRef<const Slot *> fileSlots) const {
  std::vector<const Slot *> sorted = fileSlots;

  // Process slot events in source order. Ties are broken deterministically so
  // segment construction is stable even when several metadata events occur at
  // the same byte offset.
  std::sort(
      sorted.begin(), sorted.end(), [](const Slot *first, const Slot *second) {
        if (first->b != second->b)
          return first->b < second->b;
        if (first->e != second->e)
          return first->e < second->e;

        const uint64_t pp1 =
            first->pp ? *first->pp : std::numeric_limits<uint64_t>::max();
        const uint64_t pp2 =
            second->pp ? *second->pp : std::numeric_limits<uint64_t>::max();
        if (pp1 != pp2)
          return pp1 < pp2;

        return first->id < second->id;
      });

  std::vector<Segment> segs;

  // These track the active ownership state after applying all slot events at
  // the current byte position. Each emitted segment inherits this state.
  std::optional<uint64_t> currentIncludeId;
  std::optional<uint64_t> currentArmId;

  size_t i = 0;
  while (i < sorted.size()) {
    const uint64_t pos = sorted[i]->b;

    // Apply every metadata event at this byte offset before emitting the
    // segment that starts here. This makes begin/end slots define ownership for
    // the half-open interval [pos, nextPos).
    size_t j = i;
    for (; j < sorted.size() && sorted[j]->b == pos; ++j) {
      const Slot &s = *sorted[j];
      if (s.kind == "file_begin") {
        currentIncludeId = s.ownerIncludeId;
        currentArmId = std::nullopt;
      } else if (s.kind == "file_end") {
        currentIncludeId = std::nullopt;
        currentArmId = std::nullopt;
      } else if (s.kind == "arm_begin") {
        currentArmId = s.ref;
      } else if (s.kind == "arm_end") {
        currentArmId = std::nullopt;
      }
    }

    if (j >= sorted.size())
      break;

    // The next distinct slot position closes the segment opened by the state we
    // just computed. Empty ranges are ignored.
    const uint64_t nextPos = sorted[j]->b;
    if (pos < nextPos) {
      Segment seg;
      seg.file = file;
      seg.b = pos;
      seg.e = nextPos;
      seg.ownerIncludeId = currentIncludeId;
      seg.ownerCondArmId = currentArmId;
      segs.push_back(std::move(seg));
    }
    i = j;
  }

  return segs;
}

uint32_t RefoldModel::GetIncludeDepth(std::optional<uint64_t> includeId) const {
  // TU-owned source has depth 0. Include-owned source starts at depth 1 and
  // increases through the parent include chain.
  if (!includeId)
    return 0;

  auto it = includeDepthCache_.find(*includeId);
  if (it != includeDepthCache_.end())
    return it->second;

  const IncludeItem *inc = GetIncludeById(*includeId);
  uint32_t depth = 1;
  if (inc && inc->parent)
    depth = GetIncludeDepth(inc->parent) + 1;

  // Cache the computed depth because ownership queries may ask for the same
  // include repeatedly while resolving segments, conditionals, and PP tokens.
  includeDepthCache_[*includeId] = depth;
  return depth;
}

std::optional<uint64_t>
RefoldModel::InnermostIncludeAtPP(uint64_t ppIndex) const {
  std::optional<int> bestId;
  uint32_t bestDepth = 0;

  // Multiple include covers can contain the same PP token when includes are
  // nested. Pick the deepest containing include so the returned owner is the
  // most local include expansion, not an outer parent.
  for (const auto &inc : includes_) {
    if (inc.cover.begin <= ppIndex && ppIndex < inc.cover.end) {
      uint32_t depth = GetIncludeDepth(inc.id);
      if (depth > bestDepth) {
        bestDepth = depth;
        bestId = inc.id;
      }
    }
  }

  return bestId;
}

std::optional<uint64_t>
RefoldModel::LeastCommonAncestorInclude(std::optional<uint64_t> a,
                                        std::optional<uint64_t> b) const {
  if (a == b)
    return a;

  // The TU/root owner is represented as std::nullopt. If only one side is
  // include-owned, their only common owner is the TU/root domain.
  if (!a || !b)
    return std::nullopt;

  auto buildChainRootTo = [this](std::optional<uint64_t> id) {
    std::vector<uint64_t> chain;

    // Build the include-parent chain from the leaf include up to the outermost
    // include, then reverse it so both chains can be compared from the root
    // downward.
    while (id) {
      chain.push_back(*id);
      const IncludeItem *inc = GetIncludeById(*id);
      if (!inc || !inc->parent)
        break;
      id = inc->parent;
    }

    std::reverse(chain.begin(), chain.end());
    return chain;
  };

  std::vector<uint64_t> chainA = buildChainRootTo(a);
  std::vector<uint64_t> chainB = buildChainRootTo(b);

  // Walk both root-to-leaf chains until they diverge. The last equal include id
  // is the deepest common include owner.
  std::optional<uint64_t> lastCommon;
  const size_t n = std::min(chainA.size(), chainB.size());
  for (size_t i = 0; i < n; ++i) {
    if (chainA[i] != chainB[i])
      break;
    lastCommon = chainA[i];
  }
  return lastCommon;
}

uint32_t RefoldModel::GetCondGroupDepth(uint64_t groupId) const {
  auto it = condGroupDepthCache_.find(groupId);
  if (it != condGroupDepthCache_.end())
    return it->second;

  const CondGroup *group = GetCondGroupById(groupId);

  // Top-level conditional groups have depth 1. A group nested inside another
  // conditional arm is one level deeper than the parent arm's group.
  uint32_t depth = 1;
  if (group && group->parentArmId) {
    if (auto parentArm = GetArmRefById(*group->parentArmId))
      depth = GetCondGroupDepth(parentArm->group->id) + 1;
  }

  // Cache depths because conditional ownership lookups repeatedly compare
  // nesting levels while selecting the innermost arm/group witness.
  condGroupDepthCache_[groupId] = depth;
  return depth;
}

std::optional<RefoldModel::ArmRef>
RefoldModel::FindArmRefAtPP(uint64_t ppIndex) const {
  auto ent = MapPP(ppIndex);
  if (!ent)
    return std::nullopt;

  const TokMapEntry &t = *ent;
  const std::optional<uint64_t> ownerIncId = InnermostIncludeAtPP(ppIndex);

  if (auto direct = FindArmRefForByte(t.file, ownerIncId, t.b))
    return direct;

  // If the token is not inside a conditional arm in its own file, walk outward
  // through include sites.
  std::optional<uint64_t> cur = ownerIncId;
  while (cur) {
    const IncludeItem *inc = GetIncludeById(*cur);
    if (!inc)
      break;
    if (auto atSite = FindArmRefForByte(inc->sitePath, inc->parent, inc->siteB))
      return atSite;
    cur = inc->parent;
  }

  return std::nullopt;
}

std::optional<uint64_t>
RefoldModel::FirstConditionalArmStartA(const CondGroup &group) const {
  if (group.arms.empty())
    return std::nullopt;

  // Build an O(1) membership set for the group's arm ids. SmallDenseSet keeps
  // the common few-arm case inline while still handling larger conditionals.
  SmallDenseSet<uint64_t, 8> armIds;
  armIds.reserve(group.arms.size());
  for (const auto &arm : group.arms)
    armIds.insert(arm.id);

  uint64_t best = std::numeric_limits<uint64_t>::max();
  bool found = false;

  for (const auto &slot : slots_) {
    // Only arm-begin slots in the same recorded file and include-owner domain
    // can mark the first A-token position for this conditional group.
    if (slot.kind != "arm_begin" || slot.file != group.file)
      continue;

    if (slot.ownerIncludeId != group.parentIncludeId)
      continue;

    if (!slot.pp || !slot.ref)
      continue;

    // Reject slots for other conditional groups.
    if (!armIds.count(*slot.ref))
      continue;

    if (*slot.pp < best) {
      best = *slot.pp;
      found = true;
    }
  }

  return found ? std::optional<uint64_t>(best) : std::nullopt;
}

std::vector<const RefoldModel::Slot *> RefoldModel::FindSlots(
    std::optional<StringRef> file, std::optional<StringRef> kind,
    std::optional<uint64_t> ref, std::optional<uint64_t> ownerIncludeId) const {
  std::vector<const Slot *> out;

  // Apply each supplied field as an exact-match filter. Omitted filters are
  // wildcards, which lets callers search broadly and then rely on the stable
  // ordering below.
  for (const auto &slot : slots_) {
    if (file && slot.file != *file)
      continue;
    if (kind && slot.kind != *kind)
      continue;
    if (ref && slot.ref != ref)
      continue;
    if (ownerIncludeId && slot.ownerIncludeId != ownerIncludeId)
      continue;
    out.push_back(&slot);
  }

  // Return slots in deterministic source order. Missing PP indices sort after
  // concrete PP indices at the same byte range, and id is the final tie-breaker
  // for otherwise identical metadata.
  std::sort(out.begin(), out.end(), [](const Slot *a, const Slot *b) {
    if (a->b != b->b)
      return a->b < b->b;
    if (a->e != b->e)
      return a->e < b->e;

    constexpr auto maxLocal = std::numeric_limits<std::uint64_t>::max();
    uint64_t ap = a->pp ? *a->pp : maxLocal;
    uint64_t bp = b->pp ? *b->pp : maxLocal;
    if (ap != bp)
      return ap < bp;

    return a->id < b->id;
  });
  return out;
}

std::vector<RefoldModel::TokMapEntry>
RefoldModel::MapSpan(const PPSpan &span) const {
  if (!span.IsValid())
    return {};

  std::vector<TokMapEntry> out;

  const uint64_t count = span.end - span.begin;
  out.reserve(static_cast<std::size_t>(count));

  // Convert the half-open PP-token span to the source-token mappings recorded
  // by the producer. Missing PP indices are skipped because not every
  // preprocessed token necessarily has a concrete source spelling.
  for (uint64_t i = span.begin; i < span.end; ++i) {
    auto it = tokmapByPP_.find(i);
    if (it != tokmapByPP_.end()) {
      out.push_back(it->second);
    }
  }
  return out;
}

} // namespace refold
} // namespace clang
