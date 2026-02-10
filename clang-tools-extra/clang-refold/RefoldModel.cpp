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

#include "RefoldLog.h"
#include "RefoldModel.h"
#include "llvm/ADT/ArrayRef.h"
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

// ================ Local JSON helpers (no exceptions) =================

namespace {
using namespace clang::refold;

Expected<const json::Value *> requireField(const json::Object &obj,
                                           StringRef key, StringRef ctx) {
  if (const json::Value *val = obj.get(key))
    return val;
  return createStringError(inconvertibleErrorCode(),
                           "Missing required field '%s' at %s",
                           key.str().c_str(), ctx.str().c_str());
}

Expected<const json::Object *> asObject(const json::Value &val, StringRef ctx) {
  if (auto *obj = val.getAsObject())
    return obj;
  return createStringError(inconvertibleErrorCode(), "Expected object at %s",
                           ctx.str().c_str());
}

Expected<const json::Array *> asArray(const json::Value &val, StringRef ctx) {
  if (auto *arr = val.getAsArray())
    return arr;
  return createStringError(inconvertibleErrorCode(), "Expected array at %s",
                           ctx.str().c_str());
}

Expected<StringRef> asString(const json::Value &val, StringRef ctx) {
  if (auto str = val.getAsString())
    return *str;
  return createStringError(inconvertibleErrorCode(), "Expected string at %s",
                           ctx.str().c_str());
}

Expected<uint32_t> asUInt32(const json::Value &val, StringRef ctx) {
  if (auto n = val.getAsUINT64()) {
    if (*n <= std::numeric_limits<uint32_t>::max())
      return static_cast<uint32_t>(*n);
  }
  return createStringError(inconvertibleErrorCode(), "Expected uint32_t at %s",
                           ctx.str().c_str());
}

Expected<uint64_t> asUInt64(const json::Value &val, StringRef ctx) {
  if (auto n = val.getAsUINT64())
    return *n;
  return createStringError(inconvertibleErrorCode(), "Expected uint64_t at %s",
                           ctx.str().c_str());
}

Expected<bool> asBool(const json::Value &val, StringRef ctx) {
  if (auto b = val.getAsBoolean()) {
    return *b;
  }
  return createStringError(inconvertibleErrorCode(), "Expected bool at %s",
                           ctx.str().c_str());
}

std::optional<const json::Array *>
asOptArray(const json::Object &obj, StringRef key, bool canBeNull = false) {
  if (const json::Value *val = obj.get(key)) {
    if (val->getAsNull()) {
      if (canBeNull)
        return std::nullopt;
      fatal("model", "encountered unexpected null for property '{0}'", key);
    }
    auto arr = val->getAsArray();
    if (!arr) {
      fatal("model",
            "invalid json value type on field '{0}': expected array value",
            key);
    }
    return arr;
  }
  return std::nullopt;
}

std::optional<StringRef> asOptString(const json::Object &obj, StringRef key,
                                     bool canBeNull = false) {
  if (const json::Value *val = obj.get(key)) {
    if (val->getAsNull()) {
      if (canBeNull)
        return std::nullopt;
      fatal("model", "encountered unexpected null for property '{0}'", key);
    }
    return val->getAsString();
  }
  return std::nullopt;
}

[[maybe_unused]]
std::optional<uint32_t> asOptUInt32(const json::Object &obj, StringRef key,
                                    bool canBeNull = false) {
  if (const json::Value *val = obj.get(key)) {
    if (val->getAsNull()) {
      if (canBeNull)
        return std::nullopt;
      fatal("model", "encountered unexpected null for property '{0}'", key);
    }
    if (auto n = val->getAsUINT64()) {
      if (*n <= std::numeric_limits<uint32_t>::max())
        return static_cast<uint32_t>(*n);
      fatal("model",
            "uint32_t type out of bounds for value {0} on property '{1}'", *n,
            key);
    }
  }
  return std::nullopt;
}

std::optional<uint64_t> asOptUInt64(const json::Object &obj, StringRef key,
                                    bool canBeNull = false) {
  if (const json::Value *val = obj.get(key)) {
    if (val->getAsNull()) {
      if (canBeNull)
        return std::nullopt;
      fatal("model", "encountered unexpected null for property '{0}'", key);
    }
    return val->getAsUINT64();
  }
  return std::nullopt;
}

[[maybe_unused]]
std::optional<bool> asOptBool(const json::Object &obj, StringRef key,
                              bool canBeNull = false) {
  if (const json::Value *val = obj.get(key)) {
    if (val->getAsNull()) {
      if (canBeNull)
        return std::nullopt;
      fatal("model", "encountered unexpected null for property '{0}'", key);
    }
    return val->getAsBoolean();
  }
  return std::nullopt;
}

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

Expected<const json::Object *> arrayObjElemAt(const json::Array &arr,
                                              std::size_t idx, StringRef ctx) {
  const json::Value &val = arr[idx];
  return asObject(val, ctx);
}

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
      fatal("model", "expected uint64_t from field '{0}'", field);
  }
  return out;
}
} // namespace

namespace clang {
namespace refold {

namespace {
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

      // If this PPArgSpan has a byte range, then parse byte_begin/byte_end.
      if (RefoldModel::HasByteRange(*argKind)) {
        auto bbOrErr = applyToField(asUInt32, *obj, "byte_begin", ctxItem);
        if (!bbOrErr)
          return bbOrErr.takeError();
        span.byteBegin = *bbOrErr;
        auto beOrErr = applyToField(asUInt32, *obj, "byte_end", ctxItem);
        if (!beOrErr)
          return beOrErr.takeError();
        span.byteEnd = *beOrErr;
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

bool RefoldModel::PPArgSpan::IsValid() const {
  if (!PPSpan::IsValid())
    return false;

  bool okPaste = (kind != PPArgSpanKind::Paste) ||
                 (byteBegin && byteEnd && *byteEnd > *byteBegin);

  bool okPP = (!ppByteBegin && !ppByteEnd) || (ppByteBegin && ppByteEnd);

  return okPaste && okPP;
}

// ================== RefoldModel construction =====================

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

  // Make sure we have both tokPPByteBeginA_ and tokPPByteEndA_.
  if (model.tokPPByteBeginA_) {
    if (!model.tokPPByteEndA_)
      model.tokPPByteBeginA_ = std::nullopt;
    warn("model", "missing per-token pp byte end spans");
  } else if (model.tokPPByteEndA_) {
    model.tokPPByteEndA_ = std::nullopt;
    warn("model", "missing per-token pp byte begin spans");
  }

  // Make sure the token count matches the size of each tokPPByteBeginA_ and
  // tokPPByteEndA_ vector.
  if (model.tokPPByteBeginA_ &&
      (model.tokPPByteBeginA_->size() != model.tokPPByteEndA_->size() ||
       model.tokPPByteBeginA_->size() != (size_t)model.tokensCountA_)) {
    model.tokPPByteBeginA_ = std::nullopt;
    model.tokPPByteEndA_ = std::nullopt;
    warn("model",
         "pp byte begin/end spans size does not match the A-side token count");
  }

  // tokmap
  {
    auto arrOrErr = applyToField(asArray, root, "tokmap");
    if (!arrOrErr)
      return arrOrErr.takeError();
    const json::Array *arr = *arrOrErr;

    int autoPP = 0;
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

      // optional
      entry.pp = autoPP;
      if (auto ppVal = asOptUInt64(*obj, "pp")) {
        entry.pp = *ppVal;
      } else {
        ++autoPP;
      }

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
              auto hsFileOrErr =
                  applyToField(asString, hsObj, "file", headerCtxDecl);
              if (!hsFileOrErr)
                return hsFileOrErr.takeError();
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
              decl.file = *hsFileOrErr;
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

          auto textOrErr = applyToField(asString, *obj, "text", ctxItem);
          if (!textOrErr)
            return textOrErr.takeError();
          md.text = *textOrErr;

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

        auto invTextOrErr = applyToField(asString, *obj, "inv_text", ctxItem);
        if (!invTextOrErr)
          return invTextOrErr.takeError();
        StringRef invText = *invTextOrErr;

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

        std::vector<MacroInvocation::OptByteRange> invArgRanges;
        if (auto invArgRangesArr = asOptArray(*obj, "inv_arg_ranges",
                                              /*canBeNull=*/true)) {
          for (const auto &elem : **invArgRangesArr) {
            auto *rObj = elem.getAsObject();
            if (!rObj) {
              fatal("model",
                    "invalid json value type on field 'inv_arg_ranges': expected object value");
            }

            const json::Value *bVal = rObj->get("b");
            const json::Value *eVal = rObj->get("e");
            if (!bVal || !eVal) {
              fatal("model",
                    "missing required fields on 'inv_arg_ranges' element: expected {b,e}");
            }

            std::optional<uint64_t> b;
            std::optional<uint64_t> e;
            if (!bVal->getAsNull())
              b = bVal->getAsUINT64();
            if (!eVal->getAsNull())
              e = eVal->getAsUINT64();

            invArgRanges.emplace_back(b, e);
          }
        }

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

        MacroInvocation mi(/*id*/ id,
                           /*subkind*/ subkind,
                           /*name*/ name,
                           /*invText*/ invText,
                           /*invFile*/ invFile,
                           /*invB*/ invB,
                           /*invE*/ invE,
                           /*invPPByteBegin*/ invPPByteBegin,
                           /*invPPByteEnd*/ invPPByteEnd,
                           /*ownerIncludeId*/ ownerIncludeId,
                           /*invArgRanges*/ std::move(invArgRanges),
                           /*spans*/ std::move(*spans),
                           /*argSpans*/ std::move(argSpans),
                           /*stringifySpans*/ std::move(stringifySpans),
                           /*pasteSpans*/ std::move(pasteSpans),
                           /*bodySpans*/ std::move(bodySpans));

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
  model.BuildIndicesAndSort();
  return model;
}

void RefoldModel::BuildIndicesAndSort() {
  // Invalidate caches/indices derived from the raw parsed vectors.
  includeDepthCache_.clear();
  condGroupDepthCache_.clear();

  condGroupById_.clear();
  armById_.clear();
  segmentsByFile_.clear();

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

// ================== Query helpers (ported behavior) ==================

std::vector<const RefoldModel::CondGroup *>
RefoldModel::GetCondGroups(StringRef file,
                           std::optional<uint64_t> parentIncludeId) const {
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
      // Skip non-selected arms when the producer emitted selection metadata.
      if (!arm.selected)
        continue;
      if (!arm.ContainsByte(byteOffset))
        continue;

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
  if (auto ref = GetArmRefById(armId)) {
    auto slots = FindSlots(ref->group->file, "arm_begin", armId,
                           ref->group->parentIncludeId);
    if (!slots.empty())
      return slots.front();
    return std::nullopt;
  }
  auto slots = FindSlots(std::nullopt, "arm_begin", armId, std::nullopt);
  if (!slots.empty())
    return slots.front();
  return std::nullopt;
}

std::optional<const RefoldModel::Slot *>
RefoldModel::GetArmEndSlot(uint64_t armId) const {
  if (auto ref = GetArmRefById(armId)) {
    auto slots = FindSlots(ref->group->file, "arm_end", armId,
                           ref->group->parentIncludeId);
    if (!slots.empty())
      return slots.front();
    return std::nullopt;
  }
  auto slots = FindSlots(std::nullopt, "arm_end", armId, std::nullopt);
  if (!slots.empty())
    return slots.front();
  return std::nullopt;
}

std::vector<RefoldModel::Segment>
RefoldModel::BuildSegmentsForFile(StringRef file,
                                  ArrayRef<const Slot *> fileSlots) const {
  std::vector<const Slot *> sorted = fileSlots;
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
  std::optional<uint64_t> currentIncludeId;
  std::optional<uint64_t> currentArmId;

  size_t i = 0;
  while (i < sorted.size()) {
    const uint64_t pos = sorted[i]->b;

    // Apply all events at |pos|.
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
  if (!includeId)
    return 0;

  auto it = includeDepthCache_.find(*includeId);
  if (it != includeDepthCache_.end())
    return it->second;

  const IncludeItem *inc = GetIncludeById(*includeId);
  uint32_t depth = 1;
  if (inc && inc->parent)
    depth = GetIncludeDepth(inc->parent) + 1;

  includeDepthCache_[*includeId] = depth;
  return depth;
}

std::optional<uint64_t>
RefoldModel::InnermostIncludeAtPP(uint64_t ppIndex) const {
  std::optional<int> bestId;
  uint32_t bestDepth = 0;

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
  if (!a || !b)
    return std::nullopt;

  auto buildChainRootTo = [this](std::optional<uint64_t> id) {
    std::vector<uint64_t> chain;
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
  uint32_t depth = 1;
  if (group && group->parentArmId) {
    if (auto parentArm = GetArmRefById(*group->parentArmId))
      depth = GetCondGroupDepth(parentArm->group->id) + 1;
  }

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

  // 1. Use a stack-allocated set if arms are few (standard)
  // or a DenseSet for larger groups.
  // For LLVM, SmallPtrSet is great, but here we have IDs (integers).
  SmallDenseSet<uint64_t, 8> armIds;
  armIds.reserve(group.arms.size());
  for (const auto &arm : group.arms)
    armIds.insert(arm.id);

  uint64_t best = std::numeric_limits<uint64_t>::max();
  bool found = false;

  for (const auto &slot : slots_) {
    // Early exits remain the same (very fast)
    // TODO: We should replace this with a call to PathesEqual
    if (slot.kind != "arm_begin" || slot.file != group.file)
      continue;

    if (slot.ownerIncludeId != group.parentIncludeId)
      continue;

    if (!slot.pp || !slot.ref)
      continue;

    // 2. O(1) lookup instead of the O(M) any_of
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
  // ordered by (b, e, pp, id)
  std::sort(out.begin(), out.end(), [](const Slot *a, const Slot *b) {
    if (a->b != b->b)
      return a->b < b->b;
    if (a->e != b->e)
      return a->e < b->e;
    constexpr auto MAX = std::numeric_limits<std::uint64_t>::max();
    uint64_t ap = a->pp ? *a->pp : MAX;
    uint64_t bp = b->pp ? *b->pp : MAX;
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
