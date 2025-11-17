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
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/JSON.h"

#include <cassert>
#include <functional>

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
auto applyToField(Fn &&fn, const json::Object &obj, llvm::StringRef key,
                  llvm::StringRef ctx = "root")
    -> decltype(std::forward<Fn>(fn)(std::declval<const json::Value &>(),
                                     std::declval<llvm::StringRef>())) {
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
Expected<std::vector<RefoldModel::PPSpan>> parsePPSpans(const json::Value &val,
                                                        StringRef ctx) {
  std::vector<RefoldModel::PPSpan> out;
  auto arrOrErr = asArray(val, ctx);
  if (!arrOrErr)
    return arrOrErr.takeError();
  const json::Array *arr = *arrOrErr;

  out.reserve(arr->size());
  for (std::size_t i = 0; i < arr->size(); ++i) {
    const std::string ctxItem =
        (ctx + llvm::Twine("[") + llvm::Twine(i) + "]").str();

    auto objOrErr = arrayObjElemAt(*arr, i, ctxItem);
    if (!objOrErr)
      return objOrErr.takeError();
    const json::Object *obj = *objOrErr;

    // required
    auto bOrErr = applyToField(asInt, *obj, "begin", ctxItem);
    if (!bOrErr)
      return bOrErr.takeError();
    int begin = *bOrErr;

    auto eOrErr = applyToField(asInt, *obj, "end", ctxItem);
    if (!eOrErr)
      return eOrErr.takeError();
    int end = *eOrErr;

    out.push_back({begin, end});
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
      const std::string ctxItem =
          (llvm::Twine("tokmap[") + llvm::Twine(i) + "]").str();

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
      const std::string ctxItem =
          (llvm::Twine("items[") + llvm::Twine(i) + "]").str();

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
              (llvm::Twine("include items[") + llvm::Twine(i) + "]").str();

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
            auto sp = parsePPSpans(*spansVal, "include.spans");
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
                          /*coverEnd*/ coverEOpt);

          model.includes_.push_back(std::move(inc));
        } else if (skStr == "#define" || skStr == "#undef") {
          MacroDirective md;

          const std::string ctxItem =
              (llvm::Twine("define/undef items[") + llvm::Twine(i) + "]").str();

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
            auto sp = parsePPSpans(*spansVal, "macro.directive.spans");
            if (!sp)
              return sp.takeError();
            md.spans = std::move(*sp);
          }

          md.subkind = skStr;
          model.macroDirs_.push_back(std::move(md));
        } else if (skStr == "#pragma") {
          PragmaDirective pd;

          const std::string ctxItem =
              (llvm::Twine("pragma items[") + llvm::Twine(i) + "]").str();

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
              (llvm::Twine("macro items[") + llvm::Twine(i) + "]").str();

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
          auto sp = parsePPSpans(*spansVal, "macro.spans");
          if (!sp)
            return sp.takeError();
          spans = std::move(*sp);
        }

        std::vector<PPSpan> argSpans;
        if (const json::Value *spansVal = obj->get("arg_spans")) {
          auto sp = parsePPSpans(*spansVal, "macro.arg_spans");
          if (!sp)
            return sp.takeError();
          argSpans = std::move(*sp);
        }

        std::vector<PPSpan> bodySpans;
        if (const json::Value *spansVal = obj->get("body_spans")) {
          auto sp = parsePPSpans(*spansVal, "macro.body_spans");
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
              (llvm::Twine("file items[") + llvm::Twine(i) + "]").str();

        // required
        auto idOrErr = applyToField(asInt, *obj, "id", ctxItem);
        if (!idOrErr)
          return idOrErr.takeError();
        fi.id = *idOrErr;

        // optional
        fi.path = asOptString(*obj, "path");

        if (const json::Value *spansVal = obj->get("spans")) {
          auto sp = parsePPSpans(*spansVal, "file.spans");
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
      const std::string ctxItem =
          (llvm::Twine("slots[") + llvm::Twine(i) + "]").str();

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
      slot.ref = asOptInt(*obj, "ret");
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
      const std::string ctxItem =
          (llvm::Twine("conds[") + llvm::Twine(i) + "]").str();

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
      group.parent = asOptInt(*obj, "parent");
      group.parentIncludeId = asOptInt(*obj, "parent_include_id");

      // arms
      auto armsOrErr = applyToField(asArray, *obj, "arms", ctxItem);
      if (!armsOrErr)
        return armsOrErr.takeError();
      const json::Array *arms = *armsOrErr;

      group.arms.reserve(arms->size());
      for (std::size_t ai = 0; ai < arms->size(); ++ai) {
        const std::string ctxItem =
            (llvm::Twine("arm[") + llvm::Twine(ai) + "]").str();

        auto objOrErr = arrayObjElemAt(*arms, ai, ctxItem);
        if (!objOrErr)
          return objOrErr.takeError();
        const json::Object *armObj = *objOrErr;

        CondArm arm;

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
}

// ================== Query helpers (ported behavior) ==================

std::vector<const RefoldModel::CondGroup *>
RefoldModel::GetCondGroups(StringRef file,
                           const std::optional<int> &parentIncludeId) const {
  if (!parentIncludeId.has_value()) {
    auto it = condsByFile_.find(file.str());
    if (it != condsByFile_.end())
      return it->second;
    return {};
  }
  auto fit = condsByFileByOwner_.find(file.str());
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
  for (const CondGroup *group : GetCondGroups(file, parentIncludeId)) {
    if (group->groupB <= byteOffset && byteOffset < group->groupE) {
      for (const CondArm &arm : group->arms) {
        if (arm.ContainsByte(byteOffset))
          return ArmRef{group, &arm};
      }
    }
  }
  return std::nullopt;
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
