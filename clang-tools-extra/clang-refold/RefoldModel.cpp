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

#include "RefoldModel.h"
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

using namespace llvm;

// ================ Local JSON helpers (no exceptions) =================

namespace {
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

Expected<std::string> asString(const json::Value &val, StringRef ctx) {
  if (auto str = val.getAsString())
    return std::string(*str);
  return createStringError(inconvertibleErrorCode(), "Expected string at %s",
                           ctx.str().c_str());
}

Expected<int> asInt(const json::Value &val, StringRef ctx) {
  if (auto n = val.getAsInteger())
    return static_cast<int>(*n);
  return createStringError(inconvertibleErrorCode(), "Expected integer at %s",
                           ctx.str().c_str());
}

std::optional<const json::Array *> asOptArray(const json::Object &obj,
                                              StringRef key) {
  if (const json::Value *val = obj.get(key)) {
    if (val->getAsNull())
      return std::nullopt;
    auto arr = val->getAsArray();
    assert(
        arr &&
        formatv("invalid json value type on field '{0}': expected array value",
                key)
            .str()
            .c_str());
    return arr;
  }
  return std::nullopt;
}

std::optional<std::string> asOptString(const json::Object &obj, StringRef key) {
  if (const json::Value *val = obj.get(key)) {
    if (val->getAsNull())
      return std::nullopt;
    auto str = val->getAsString();
    assert(
        str &&
        formatv("invalid json value type on field '{0}': expected string value",
                key)
            .str()
            .c_str());
    return std::string(*str);
  }
  return std::nullopt;
}

std::optional<int> asOptInt(const json::Object &obj, StringRef key) {
  if (const json::Value *val = obj.get(key)) {
    if (val->getAsNull())
      return std::nullopt;
    auto n = val->getAsInteger();
    assert(n &&
           formatv(
               "invalid json value type on field '{0}': expected integer value",
               key)
               .str()
               .c_str());
    return static_cast<int>(*n);
  }
  return std::nullopt;
}

std::optional<bool> asOptBool(const json::Object &obj, StringRef key) {
  if (const json::Value *val = obj.get(key)) {
    if (val->getAsNull())
      return std::nullopt;
    auto b = val->getAsBoolean();
    assert(b &&
           formatv(
               "invalid json value type on field '{0}': expected boolean value",
               key)
               .str()
               .c_str());
    return static_cast<bool>(*b);
  }
  return std::nullopt;
}

template <typename Fn>
auto applyToField(Fn &&fn, const json::Object &obj, StringRef key,
                  StringRef ctx = "root")
    -> decltype(std::forward<Fn>(fn)(std::declval<const json::Value &>(),
                                     std::declval<StringRef>())) {
  auto fieldOrErr = requireField(obj, key, ctx); // Expected<const json::Value*>
  if (!fieldOrErr)
    return fieldOrErr.takeError();
  return std::forward<decltype(fn)>(fn)(**fieldOrErr, key);
}

Expected<const json::Object *> arrayObjElemAt(const json::Array &arr,
                                              std::size_t idx, StringRef ctx) {
  const json::Value &val = arr[idx];
  return asObject(val, ctx);
}
} // namespace

namespace clang {
namespace refold {

namespace {
template <typename T = RefoldModel::PPSpan>
Expected<std::vector<T>> parseSpans(const json::Value &val, StringRef ctx) {
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
    auto bOrErr = applyToField(asInt, *obj, "begin", ctxItem);
    if (!bOrErr)
      return bOrErr.takeError();

    auto eOrErr = applyToField(asInt, *obj, "end", ctxItem);
    if (!eOrErr)
      return eOrErr.takeError();

    T span;
    span.begin = *bOrErr;
    span.end = *eOrErr;

    // 2. Handle arg_index if T is PPArgSpan
    // Using 'if constexpr' ensures this code only exists for PPArgSpan
    if constexpr (std::is_same_v<T, RefoldModel::PPArgSpan>) {
      auto argOrErr = applyToField(asInt, *obj, "arg_index", ctxItem);
      if (!argOrErr)
        return argOrErr.takeError();
      span.argIdx = *argOrErr;
    }

    out.push_back(std::move(span));
  }
  return out;
}
} // namespace

// ================== RefoldModel construction =====================

Expected<RefoldModel> RefoldModel::FromJson(const json::Object &root) {
  RefoldModel model;

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
  auto cntOrErr = applyToField(asInt, tokObj, "count", "tokens.count");
  if (!cntOrErr)
    return cntOrErr.takeError();
  model.tokensCountA_ = *cntOrErr;

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

      auto bOrErr = applyToField(asInt, *obj, "b", ctxItem);
      if (!bOrErr)
        return bOrErr.takeError();
      entry.b = *bOrErr;

      auto eOrErr = applyToField(asInt, *obj, "e", ctxItem);
      if (!eOrErr)
        return eOrErr.takeError();
      entry.e = *eOrErr;

      // optional
      entry.pp = autoPP;
      if (const json::Value *ppVal = obj->get("pp")) {
        if (auto ppi = ppVal->getAsInteger())
          entry.pp = static_cast<int>(*ppi);
        else
          return createStringError(inconvertibleErrorCode(),
                                   "tokmap[%zu].pp must be integer", i);
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
      std::string kindStr = *kindOrErr;

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
          auto idOrErr = applyToField(asInt, *obj, "id", ctxItem);
          if (!idOrErr)
            return idOrErr.takeError();
          int id = *idOrErr;

          auto spOrErr = applyToField(asString, *obj, "site_path", ctxItem);
          if (!spOrErr)
            return spOrErr.takeError();
          std::string sitePath = *spOrErr;

          auto sbOrErr = applyToField(asInt, *obj, "site_b", ctxItem);
          if (!sbOrErr)
            return sbOrErr.takeError();
          int siteB = *sbOrErr;

          auto seOrErr = applyToField(asInt, *obj, "site_e", ctxItem);
          if (!seOrErr)
            return seOrErr.takeError();
          int siteE = *seOrErr;

          auto tgtOrErr = applyToField(asString, *obj, "target", ctxItem);
          if (!tgtOrErr)
            return tgtOrErr.takeError();
          std::string target = *tgtOrErr;

          // optional
          std::string text;
          if (const std::optional<std::string> textOpt =
                  asOptString(*obj, "text")) {
            text = *textOpt;
          }
          std::optional<std::string> resolved =
              asOptString(*obj, "resolved_path");
          bool angled = false;
          if (const std::optional<bool> angledOpt = asOptBool(*obj, "angled")) {
            angled = *angledOpt;
          }
          std::optional<int> parent = asOptInt(*obj, "parent");

          std::vector<PPSpan> spans;
          if (const json::Value *spansVal = obj->get("spans")) {
            auto sp = parseSpans(*spansVal, "include.spans");
            if (!sp)
              return sp.takeError();
            spans = std::move(*sp);
          }

          std::optional<int> coverBOpt, coverEOpt;
          if (const json::Value *ppcVal = obj->get("pp_cover")) {
            auto ppcOrErr = asObject(*ppcVal, "include.pp_cover");
            if (!ppcOrErr)
              return ppcOrErr.takeError();
            const json::Object &ppcObj = **ppcOrErr;

            auto beginOrErr =
                applyToField(asInt, ppcObj, "begin", "include.pp_cover.begin");
            if (!beginOrErr)
              return beginOrErr.takeError();
            coverBOpt = *beginOrErr;

            auto endOrErr =
                applyToField(asInt, ppcObj, "end", "include.pp_cover.end");
            if (!endOrErr)
              return endOrErr.takeError();
            coverEOpt = *endOrErr;
          }

          std::vector<RefoldModel::HeaderDecl> decls;
          if (const json::Value *declsVal = obj->get("decls")) {
            if (!declsVal->getAsNull()) {
              auto declArrOrErr = asArray(*declsVal, "include.decls");
              if (!declArrOrErr)
                return declArrOrErr.takeError();
              const json::Array &declArr = **declArrOrErr;
              decls.reserve(declArr.size());
              for (std::size_t di = 0; di < declArr.size(); ++di) {
                const std::string ctxDecl =
                    (Twine("include.decls[") + Twine(di) + "]").str();
                auto declObjOrErr = asObject(declArr[di], ctxDecl);
                if (!declObjOrErr)
                  return declObjOrErr.takeError();
                const json::Object &declObj = **declObjOrErr;

                auto kindOrErr = applyToField(asString, declObj, "kind", ctxDecl);
                if (!kindOrErr)
                  return kindOrErr.takeError();
                auto nameOrErr = applyToField(asString, declObj, "name", ctxDecl);
                if (!nameOrErr)
                  return nameOrErr.takeError();

                auto hsValOrErr = requireField(declObj, "header_span", ctxDecl);
                if (!hsValOrErr)
                  return hsValOrErr.takeError();
                auto hsObjOrErr = asObject(**hsValOrErr, ctxDecl + ".header_span");
                if (!hsObjOrErr)
                  return hsObjOrErr.takeError();
                const json::Object &hsObj = **hsObjOrErr;
                auto hsFileOrErr = applyToField(asString, hsObj, "file", ctxDecl);
                if (!hsFileOrErr)
                  return hsFileOrErr.takeError();
                auto hsBOrErr = applyToField(asInt, hsObj, "b", ctxDecl);
                if (!hsBOrErr)
                  return hsBOrErr.takeError();
                auto hsEOrErr = applyToField(asInt, hsObj, "e", ctxDecl);
                if (!hsEOrErr)
                  return hsEOrErr.takeError();

                auto psValOrErr = requireField(declObj, "pp_span", ctxDecl);
                if (!psValOrErr)
                  return psValOrErr.takeError();
                auto psObjOrErr = asObject(**psValOrErr, ctxDecl + ".pp_span");
                if (!psObjOrErr)
                  return psObjOrErr.takeError();
                const json::Object &psObj = **psObjOrErr;
                auto psBOrErr = applyToField(asInt, psObj, "begin", ctxDecl);
                if (!psBOrErr)
                  return psBOrErr.takeError();
                auto psEOrErr = applyToField(asInt, psObj, "end", ctxDecl);
                if (!psEOrErr)
                  return psEOrErr.takeError();

                RefoldModel::HeaderDecl decl;
                decl.kind = *kindOrErr;
                decl.name = *nameOrErr;
                decl.file = *hsFileOrErr;
                decl.headerB = *hsBOrErr;
                decl.headerE = *hsEOrErr;
                decl.ppSpan = PPSpan{*psBOrErr, *psEOrErr};
                decls.push_back(std::move(decl));
              }
            }
          }

          IncludeItem inc(/*id*/ id,
                          /*subkind*/ std::string(skStr),
                          /*text*/ std::move(text),
                          /*sitePath*/ std::move(sitePath),
                          /*siteB*/ siteB,
                          /*siteE*/ siteE,
                          /*target*/ std::move(target),
                          /*resolvedPath*/ std::move(resolved),
                          /*angled*/ angled,
                          /*parent*/ std::move(parent),
                          /*spans*/ std::move(spans),
                          /*coverBegin*/ coverBOpt,
                          /*coverEnd*/ coverEOpt,
                          /*decls*/ std::move(decls));

          model.includes_.push_back(std::move(inc));
        } else if (skStr == "#define" || skStr == "#undef") {
          MacroDirective md;

          const std::string ctxItem =
              (Twine("define/undef items[") + Twine(i) + "]").str();

          // required
          auto idOrErr = applyToField(asInt, *obj, "id", ctxItem);
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

          auto sbOrErr = applyToField(asInt, *obj, "site_b", ctxItem);
          if (!sbOrErr)
            return sbOrErr.takeError();
          md.siteB = *sbOrErr;

          auto seOrErr = applyToField(asInt, *obj, "site_e", ctxItem);
          if (!seOrErr)
            return seOrErr.takeError();
          md.siteE = *seOrErr;

          // optional
          md.ownerIncludeId = asOptInt(*obj, "owner_include_id");

          if (const json::Value *spansVal = obj->get("spans")) {
            auto sp = parseSpans(*spansVal, "macro.directive.spans");
            if (!sp)
              return sp.takeError();
            md.spans = std::move(*sp);
          }

          md.subkind = skStr;
          model.macroDirs_.push_back(std::move(md));
        } else if (skStr == "#pragma") {
          PragmaDirective pd;

          const std::string ctxItem =
              (Twine("pragma items[") + Twine(i) + "]").str();

          // required
          auto idOrErr = applyToField(asInt, *obj, "id", ctxItem);
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

          auto sbOrErr = applyToField(asInt, *obj, "site_b", ctxItem);
          if (!sbOrErr)
            return sbOrErr.takeError();
          pd.siteB = *sbOrErr;

          auto seOrErr = applyToField(asInt, *obj, "site_e", ctxItem);
          if (!seOrErr)
            return seOrErr.takeError();
          pd.siteE = *seOrErr;

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
        auto idOrErr = applyToField(asInt, *obj, "id", ctxItem);
        if (!idOrErr)
          return idOrErr.takeError();
        int id = *idOrErr;

        auto skOrErr = applyToField(asString, *obj, "subkind", ctxItem);
        if (!skOrErr)
          return skOrErr.takeError();
        std::string subkind = *skOrErr;

        auto nameOrErr = applyToField(asString, *obj, "name", ctxItem);
        if (!nameOrErr)
          return nameOrErr.takeError();
        std::string name = *nameOrErr;

        // optional
        std::optional<std::string> invText = asOptString(*obj, "inv_text");
        std::optional<std::string> invFile = asOptString(*obj, "inv_file");
        std::optional<int> invB = asOptInt(*obj, "inv_b");
        std::optional<int> invE = asOptInt(*obj, "inv_e");
        std::optional<int> ownerIncludeId = asOptInt(*obj, "owner_include_id");

        std::vector<PPSpan> spans;
        if (const json::Value *spansVal = obj->get("spans")) {
          auto sp = parseSpans(*spansVal, "macro.spans");
          if (!sp)
            return sp.takeError();
          spans = std::move(*sp);
        }

        std::vector<PPArgSpan> argSpans;
        if (const json::Value *spansVal = obj->get("arg_spans")) {
          auto sp = parseSpans<PPArgSpan>(*spansVal, "macro.arg_spans");
          if (!sp)
            return sp.takeError();
          argSpans = std::move(*sp);
        }

        std::vector<PPSpan> bodySpans;
        if (const json::Value *spansVal = obj->get("body_spans")) {
          auto sp = parseSpans(*spansVal, "macro.body_spans");
          if (!sp)
            return sp.takeError();
          bodySpans = std::move(*sp);
        }

        std::optional<int> coverBOpt, coverEOpt;
        if (const json::Value *ppcVal = obj->get("pp_cover")) {
          auto ppcOrErr = asObject(*ppcVal, "macro.pp_cover");
          if (!ppcOrErr)
            return ppcOrErr.takeError();
          const json::Object &ppcObj = **ppcOrErr;

          auto beginOrErr =
              applyToField(asInt, ppcObj, "begin", "macro.pp_cover.begin");
          if (!beginOrErr)
            return beginOrErr.takeError();
          coverBOpt = *beginOrErr;

          auto endOrErr =
              applyToField(asInt, ppcObj, "end", "macro.pp_cover.end");
          if (!endOrErr)
            return endOrErr.takeError();
          coverEOpt = *endOrErr;
        }

        MacroInvocation mi(/*id*/ id,
                           /*subkind*/ std::move(subkind),
                           /*name*/ std::move(name),
                           /*invText*/ std::move(invText),
                           /*invFile*/ std::move(invFile),
                           /*invB*/ std::move(invB),
                           /*invE*/ std::move(invE),
                           /*ownerIncludeId*/ std::move(ownerIncludeId),
                           /*spans*/ std::move(spans),
                           /*argSpans*/ std::move(argSpans),
                           /*bodySpans*/ std::move(bodySpans),
                           /*coverBegin*/ coverBOpt,
                           /*coverEnd*/ coverEOpt);

        model.macroInvs_.push_back(std::move(mi));
      } else if (kindStr == "file") {
        FileItem fi;

        const std::string ctxItem =
            (Twine("file items[") + Twine(i) + "]").str();

        // required
        auto idOrErr = applyToField(asInt, *obj, "id", ctxItem);
        if (!idOrErr)
          return idOrErr.takeError();
        fi.id = *idOrErr;

        // optional
        fi.path = asOptString(*obj, "path");

        if (const json::Value *spansVal = obj->get("spans")) {
          auto sp = parseSpans(*spansVal, "file.spans");
          if (!sp)
            return sp.takeError();
          fi.spans = std::move(*sp);
        }

        model.fileItems_.push_back(std::move(fi));
      } else {
        return createStringError(inconvertibleErrorCode(),
                                 "Unknown item.kind '%s' at items[%zu]",
                                 kindStr.c_str(), i);
      }
    }
  }

  // slots (optional)
  if (const auto slotsArr = asOptArray(root, "slots")) {
    const json::Array *arr = *slotsArr;
    model.slots_.reserve(arr->size());
    for (std::size_t i = 0; i < arr->size(); ++i) {
      const std::string ctxItem = (Twine("slots[") + Twine(i) + "]").str();

      auto objOrErr = arrayObjElemAt(*arr, i, ctxItem);
      if (!objOrErr)
        return objOrErr.takeError();
      const json::Object *obj = *objOrErr;

      Slot slot;

      // required
      auto idOrErr = applyToField(asInt, *obj, "id", ctxItem);
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

      auto bOrErr = applyToField(asInt, *obj, "b", ctxItem);
      if (!bOrErr)
        return bOrErr.takeError();
      slot.b = *bOrErr;

      auto eOrErr = applyToField(asInt, *obj, "e", ctxItem);
      if (!eOrErr)
        return eOrErr.takeError();
      slot.e = *eOrErr;

      // optionals
      slot.ref = asOptInt(*obj, "ref");
      slot.ownerIncludeId = asOptInt(*obj, "owner_include_id");
      slot.pp = asOptInt(*obj, "pp");

      model.slots_.push_back(std::move(slot));
    }
  }

  // conds (optional)
  if (const auto condsArr = asOptArray(root, "conds")) {
    const json::Array *arr = *condsArr;
    model.conds_.reserve(arr->size());
    for (std::size_t i = 0; i < arr->size(); ++i) {
      const std::string ctxItem = (Twine("conds[") + Twine(i) + "]").str();

      auto objOrErr = arrayObjElemAt(*arr, i, ctxItem);
      if (!objOrErr)
        return objOrErr.takeError();
      const json::Object *obj = *objOrErr;

      CondGroup group;

      // required
      auto idOrErr = applyToField(asInt, *obj, "id", ctxItem);
      if (!idOrErr)
        return idOrErr.takeError();
      group.id = *idOrErr;

      auto fileOrErr = applyToField(asString, *obj, "file", ctxItem);
      if (!fileOrErr)
        return fileOrErr.takeError();
      group.file = *fileOrErr;

      auto gbOrErr = applyToField(asInt, *obj, "group_b", ctxItem);
      if (!gbOrErr)
        return gbOrErr.takeError();
      group.groupB = *gbOrErr;

      auto geOrErr = applyToField(asInt, *obj, "group_e", ctxItem);
      if (!geOrErr)
        return geOrErr.takeError();
      group.groupE = *geOrErr;

      // optional
      group.parentArmId = asOptInt(*obj, "parent_arm_id");
      group.parentIncludeId = asOptInt(*obj, "parent_include_id");

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
        auto idOrErr = applyToField(asInt, *armObj, "id", ctxItem);
        if (!idOrErr)
          return idOrErr.takeError();
        arm.id = *idOrErr;

        auto tagOrErr = applyToField(asString, *armObj, "tag", ctxItem);
        if (!tagOrErr)
          return tagOrErr.takeError();
        arm.tag = *tagOrErr;

        auto bbOrErr = applyToField(asInt, *armObj, "body_b", ctxItem);
        if (!bbOrErr)
          return bbOrErr.takeError();
        arm.bodyB = *bbOrErr;

        auto beOrErr = applyToField(asInt, *armObj, "body_e", ctxItem);
        if (!beOrErr)
          return beOrErr.takeError();
        arm.bodyE = *beOrErr;

        // optional
        arm.cond = asOptString(*armObj, "cond");
        arm.selected = asOptBool(*armObj, "selected");

        if (const json::Value *ppSpanVal = armObj->get("pp_span")) {
          if (!ppSpanVal->getAsNull()) {
            auto ppSpanObjOrErr = asObject(*ppSpanVal, ctxItem + ".pp_span");
            if (!ppSpanObjOrErr)
              return ppSpanObjOrErr.takeError();
            const json::Object &ppSpanObj = **ppSpanObjOrErr;

            auto beginOrErr =
                applyToField(asInt, ppSpanObj, "begin", ctxItem + ".pp_span");
            if (!beginOrErr)
              return beginOrErr.takeError();
            auto endOrErr =
                applyToField(asInt, ppSpanObj, "end", ctxItem + ".pp_span");
            if (!endOrErr)
              return endOrErr.takeError();
            arm.ppSpan = PPSpan{*beginOrErr, *endOrErr};
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
                           const std::optional<int> &parentIncludeId) const {
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
                               const std::optional<int> &parentIncludeId,
                               int byteOffset) const {
  std::optional<ArmRef> best;
  int bestDepth = 0;

  for (const CondGroup *group : GetCondGroups(file, parentIncludeId)) {
    if (!group || !group->ContainsByte(byteOffset))
      continue;

    for (const CondArm &arm : group->arms) {
      // Skip non-selected arms when the producer emitted selection metadata.
      if (arm.selected && !*arm.selected)
        continue;
      if (!arm.ContainsByte(byteOffset))
        continue;

      int depth = GetCondArmDepth(arm.id);
      if (depth > bestDepth) {
        bestDepth = depth;
        best = ArmRef{group, &arm};
      }
    }
  }

  return best;
}

std::optional<const RefoldModel::Slot *> RefoldModel::GetArmBeginSlot(int armId) const {
  if (auto ref = GetArmRefById(armId)) {
    auto slots = FindSlots(std::optional<std::string>(ref->group->file),
                           std::optional<std::string>("arm_begin"),
                           std::optional<int>(armId),
                           ref->group->parentIncludeId);
    if (!slots.empty())
      return slots.front();
    return std::nullopt;
  }
  auto slots = FindSlots(std::nullopt, std::optional<std::string>("arm_begin"),
                         std::optional<int>(armId), std::nullopt);
  if (!slots.empty())
    return slots.front();
  return std::nullopt;
}

std::optional<const RefoldModel::Slot *> RefoldModel::GetArmEndSlot(int armId) const {
  if (auto ref = GetArmRefById(armId)) {
    auto slots = FindSlots(std::optional<std::string>(ref->group->file),
                           std::optional<std::string>("arm_end"),
                           std::optional<int>(armId),
                           ref->group->parentIncludeId);
    if (!slots.empty())
      return slots.front();
    return std::nullopt;
  }
  auto slots = FindSlots(std::nullopt, std::optional<std::string>("arm_end"),
                         std::optional<int>(armId), std::nullopt);
  if (!slots.empty())
    return slots.front();
  return std::nullopt;
}

std::vector<RefoldModel::Segment> RefoldModel::BuildSegmentsForFile(
    StringRef file, const std::vector<const Slot *> &fileSlots) const {
  std::vector<const Slot *> sorted = fileSlots;
  std::sort(sorted.begin(), sorted.end(),
            [](const Slot *first, const Slot *second) {
              if (first->b != second->b)
                return first->b < second->b;
              if (first->e != second->e)
                return first->e < second->e;

              const int pp1 =
                  first->pp ? *first->pp : std::numeric_limits<int>::max();
              const int pp2 =
                  second->pp ? *second->pp : std::numeric_limits<int>::max();
              if (pp1 != pp2)
                return pp1 < pp2;

              return first->id < second->id;
            });

  std::vector<Segment> segs;
  std::optional<int> currentIncludeId;
  std::optional<int> currentArmId;

  size_t i = 0;
  while (i < sorted.size()) {
    const int pos = sorted[i]->b;

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
    const int nextPos = sorted[j]->b;
    if (pos < nextPos) {
      Segment seg;
      seg.file = std::string(file);
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

unsigned
RefoldModel::GetIncludeDepth(const std::optional<int> &includeId) const {
  if (!includeId)
    return 0;

  auto it = includeDepthCache_.find(*includeId);
  if (it != includeDepthCache_.end())
    return it->second;

  const IncludeItem *inc = GetIncludeById(*includeId);
  int depth = 1;
  if (inc && inc->parent)
    depth = GetIncludeDepth(inc->parent) + 1;

  includeDepthCache_[*includeId] = depth;
  return depth;
}

std::optional<int> RefoldModel::InnermostIncludeAtPP(int ppIndex) const {
  std::optional<int> bestId;
  int bestDepth = 0;

  for (const auto &inc : includes_) {
    if (inc.cover.begin <= ppIndex && ppIndex < inc.cover.end) {
      int depth = GetIncludeDepth(inc.id);
      if (depth > bestDepth) {
        bestDepth = depth;
        bestId = inc.id;
      }
    }
  }

  return bestId;
}

std::optional<int>
RefoldModel::LeastCommonAncestorInclude(std::optional<int> a,
                                        std::optional<int> b) const {
  if (a == b)
    return a;
  if (!a || !b)
    return std::nullopt;

  auto buildChainRootTo = [this](std::optional<int> id) {
    std::vector<int> chain;
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

  std::vector<int> chainA = buildChainRootTo(a);
  std::vector<int> chainB = buildChainRootTo(b);

  std::optional<int> lastCommon;
  const size_t n = std::min(chainA.size(), chainB.size());
  for (size_t i = 0; i < n; ++i) {
    if (chainA[i] != chainB[i])
      break;
    lastCommon = chainA[i];
  }
  return lastCommon;
}

unsigned
RefoldModel::GetCondGroupDepth(const std::optional<int> &groupId) const {
  if (!groupId)
    return 0;

  auto it = condGroupDepthCache_.find(*groupId);
  if (it != condGroupDepthCache_.end())
    return it->second;

  const CondGroup *group = GetCondGroupById(*groupId);
  int depth = 1;
  if (group && group->parentArmId) {
    if (auto parentArm = GetArmRefById(*group->parentArmId))
      depth = GetCondGroupDepth(parentArm->group->id) + 1;
  }

  condGroupDepthCache_[*groupId] = depth;
  return depth;
}

std::optional<RefoldModel::ArmRef> RefoldModel::FindArmRefAtPP(int ppIndex) const {
  auto ent = MapPP(ppIndex);
  if (!ent)
    return std::nullopt;

  const TokMapEntry &t = *ent;
  const std::optional<int> ownerIncId = InnermostIncludeAtPP(ppIndex);

  if (auto direct = FindArmRefForByte(t.file, ownerIncId, t.b))
    return direct;

  // If the token is not inside a conditional arm in its own file, walk outward
  // through include sites.
  std::optional<int> cur = ownerIncId;
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

int RefoldModel::FirstConditionalArmStartA(const CondGroup &group) const {
  if (group.arms.empty())
    return -1;

  DenseSet<int> armIds;
  armIds.reserve(group.arms.size());
  for (const auto &arm : group.arms)
    armIds.insert(arm.id);

  int best = std::numeric_limits<int>::max();
  for (const auto &slot : slots_) {
    if (slot.kind != "arm_begin")
      continue;
    if (slot.file != group.file)
      continue;
    if (slot.ownerIncludeId != group.parentIncludeId)
      continue;
    if (!slot.pp || !slot.ref)
      continue;
    if (!armIds.contains(*slot.ref))
      continue;
    best = std::min(best, *slot.pp);
  }

  return best == std::numeric_limits<int>::max() ? -1 : best;
}

std::vector<const RefoldModel::Slot *>
RefoldModel::FindSlots(const std::optional<std::string> &file,
                       const std::optional<std::string> &kind,
                       const std::optional<int> &ref,
                       const std::optional<int> &ownerIncludeId) const {
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
    constexpr auto MAX = std::numeric_limits<std::int32_t>::max();
    int ap = a->pp ? *a->pp : MAX;
    int bp = b->pp ? *b->pp : MAX;
    if (ap != bp)
      return ap < bp;
    return a->id < b->id;
  });
  return out;
}

std::vector<RefoldModel::TokMapEntry> RefoldModel::MapSpan(PPSpan span) const {
  if (span.begin < 0 || span.end < span.begin)
    return {};
  std::vector<TokMapEntry> out;
  out.reserve(static_cast<std::size_t>(std::max(0, span.end - span.begin)));
  for (int i = span.begin; i < span.end; ++i) {
    auto it = tokmapByPP_.find(i);
    if (it != tokmapByPP_.end())
      out.push_back(it->second);
  }
  return out;
}

} // namespace refold
} // namespace clang
