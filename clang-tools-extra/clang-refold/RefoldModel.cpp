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

#include <cassert>
#include <functional>

using namespace llvm;

namespace {

// ================ Local JSON helpers (no exceptions) =================

Expected<const json::Object *> asObject(const json::Value &val, StringRef ctx) {
  if (auto *obj = val.getAsObject())
    return obj;
  return createStringError(inconvertibleErrorCode(), "Expected object at %s",
                           ctx.str().c_str());
}

Expected<const json::Value *> requireField(const json::Object &obj,
                                           StringRef key, StringRef ctx) {
  if (const json::Value *val = obj.get(key))
    return val;
  return createStringError(inconvertibleErrorCode(),
                           "Missing required field '%s' at %s",
                           key.str().c_str(), ctx.str().c_str());
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

[[maybe_unused]]
std::optional<int> asOptInt(const json::Object &obj, StringRef key) {
  if (const json::Value *val = obj.get(key)) {
    if (auto n = val->getAsInteger())
      return static_cast<int>(*n);
  }
  return std::nullopt;
}

[[maybe_unused]]
std::optional<std::string> asOptString(const json::Object &obj, StringRef key) {
  if (const json::Value *val = obj.get(key)) {
    if (auto str = val->getAsString())
      return std::string(*str);
  }
  return std::nullopt;
}

[[maybe_unused]]
std::optional<bool> asOptBool(const json::Object &obj, StringRef key) {
  if (const json::Value *val = obj.get(key)) {
    if (auto b = val->getAsBoolean())
      return *b;
  }
  return std::nullopt;
}

} // namespace

namespace clang {
namespace refold {

namespace {
Expected<std::vector<RefoldModel::PPSpan>> parsePPSpans(const json::Value &val,
                                                        StringRef ctx) {
  std::vector<RefoldModel::PPSpan> out;
  const json::Array *arr = val.getAsArray();
  if (!arr)
    return createStringError(inconvertibleErrorCode(), "Expected array at %s",
                             ctx.str().c_str());
  out.reserve(arr->size());
  for (std::size_t i = 0; i < arr->size(); ++i) {
    const json::Value &elem = (*arr)[i];
    auto *obj = elem.getAsObject();
    if (!obj)
      return createStringError(inconvertibleErrorCode(),
                               "Expected object for spans[%zu] at %s", i,
                               ctx.str().c_str());
    auto *bVal = obj->get("begin");
    auto *eVal = obj->get("end");
    if (!bVal || !eVal)
      return createStringError(inconvertibleErrorCode(),
                               "Missing begin/end in spans[%zu] at %s", i,
                               ctx.str().c_str());
    auto b = bVal->getAsInteger();
    auto e = eVal->getAsInteger();
    if (!b || !e)
      return createStringError(inconvertibleErrorCode(),
                               "Non-integer begin/end in spans[%zu] at %s", i,
                               ctx.str().c_str());
    out.push_back({static_cast<int>(*b), static_cast<int>(*e)});
  }
  return out;
}
} // namespace

// ================== RefoldModel construction =====================

Expected<RefoldModel> RefoldModel::FromJson(const json::Object &root) {
  RefoldModel model;

  // version
  {
    auto ev = requireField(root, "version", "root");
    if (!ev)
      return ev.takeError();
    auto str = asString(**ev, "version");
    if (!str)
      return str.takeError();
    model.version_ = *str;
  }

  // source
  {
    auto ev = requireField(root, "source", "root");
    if (!ev)
      return ev.takeError();
    auto str = asString(**ev, "source");
    if (!str)
      return str.takeError();
    model.sourcePath_ = *str;
  }

  // tokens.count
  {
    auto tokVal = requireField(root, "tokens", "root");
    if (!tokVal)
      return tokVal.takeError();
    auto tokObjE = asObject(**tokVal, "tokens");
    if (!tokObjE)
      return tokObjE.takeError();
    const json::Object *tokObj = *tokObjE;

    auto cntVal = requireField(*tokObj, "count", "tokens.count");
    if (!cntVal)
      return cntVal.takeError();
    auto cnt = asInt(**cntVal, "tokens.count");
    if (!cnt)
      return cnt.takeError();
    model.tokensCountA_ = *cnt;
  }

  // tokmap (file/b/e required; pp optional)
  {
    auto ev = requireField(root, "tokmap", "root");
    if (!ev)
      return ev.takeError();
    const json::Array *arr = (*ev)->getAsArray();
    if (!arr)
      return createStringError(inconvertibleErrorCode(),
                               "tokmap must be an array");
    int autoPP = 0;
    model.tokmap_.reserve(arr->size());
    for (std::size_t i = 0; i < arr->size(); ++i) {
      const json::Value &val = (*arr)[i];
      const json::Object *obj = nullptr;
      if (auto eo = asObject(val, "tokmap[]"))
        obj = *eo;
      else
        return eo.takeError();

      auto fileVal = obj->get("file");
      auto bVal = obj->get("b");
      auto eVal = obj->get("e");
      if (!fileVal || !bVal || !eVal)
        return createStringError(inconvertibleErrorCode(),
                                 "tokmap[%zu] missing file/b/e", i);

      auto file = fileVal->getAsString();
      auto bi = bVal->getAsInteger();
      auto ei = eVal->getAsInteger();
      if (!file || !bi || !ei)
        return createStringError(inconvertibleErrorCode(),
                                 "tokmap[%zu] has invalid types", i);

      int pp = autoPP;
      if (const json::Value *ppVal = obj->get("pp")) {
        if (auto ppi = ppVal->getAsInteger())
          pp = static_cast<int>(*ppi);
        else
          return createStringError(inconvertibleErrorCode(),
                                   "tokmap[%zu].pp must be integer", i);
      } else {
        ++autoPP;
      }

      TokMapEntry entry;
      entry.pp = pp;
      entry.file = std::string(*file);
      entry.b = static_cast<int>(*bi);
      entry.e = static_cast<int>(*ei);

      model.tokmap_.push_back(entry);
      model.tokmapByPP_[pp] = entry;
    }
  }

  // items
  {
    auto ev = requireField(root, "items", "root");
    if (!ev)
      return ev.takeError();
    const json::Array *arr = (*ev)->getAsArray();
    if (!arr)
      return createStringError(inconvertibleErrorCode(),
                               "'items' must be an array");

    for (std::size_t i = 0; i < arr->size(); ++i) {
      const json::Value &val = (*arr)[i];
      const json::Object *obj = nullptr;
      if (auto eo = asObject(val, "items[]"))
        obj = *eo;
      else
        return eo.takeError();

      // kind
      auto kind = obj->get("kind");
      if (!kind)
        return createStringError(inconvertibleErrorCode(),
                                 "items[%zu] missing 'kind'", i);
      auto kindStr = kind->getAsString();
      if (!kindStr)
        return createStringError(inconvertibleErrorCode(),
                                 "items[%zu].kind must be string", i);

      if (*kindStr == "directive") {
        // subkind
        auto skVal = obj->get("subkind");
        if (!skVal)
          return createStringError(inconvertibleErrorCode(),
                                   "directive items[%zu] missing 'subkind'", i);
        auto skStr = skVal->getAsString();
        if (!skStr)
          return createStringError(inconvertibleErrorCode(),
                                   "items[%zu].subkind must be string", i);

        if (*skStr == "#include" || *skStr == "#include_next") {
          // Required
          int id = 0, siteB = 0, siteE = 0;
          {
            auto idVal = obj->get("id");
            if (!idVal)
              return createStringError(inconvertibleErrorCode(),
                                       "include items[%zu] missing id", i);
            auto idInt = idVal->getAsInteger();
            if (!idInt)
              return createStringError(inconvertibleErrorCode(),
                                       "include id must be integer");
            id = static_cast<int>(*idInt);
          }
          std::string sitePath;
          {
            auto spVal = obj->get("site_path");
            if (!spVal)
              return createStringError(inconvertibleErrorCode(),
                                       "include items[%zu] missing site_path",
                                       i);
            auto spStr = spVal->getAsString();
            if (!spStr)
              return createStringError(inconvertibleErrorCode(),
                                       "include site_path must be string");
            sitePath = std::string(*spStr);
          }
          {
            auto sbVal = obj->get("site_b");
            if (!sbVal)
              return createStringError(inconvertibleErrorCode(),
                                       "include items[%zu] missing site_b", i);
            auto sb = sbVal->getAsInteger();
            if (!sb)
              return createStringError(inconvertibleErrorCode(),
                                       "include site_b must be integer");
            siteB = static_cast<int>(*sb);
          }
          {
            auto seVal = obj->get("site_e");
            if (!seVal)
              return createStringError(inconvertibleErrorCode(),
                                       "include items[%zu] missing site_e", i);
            auto se = seVal->getAsInteger();
            if (!se)
              return createStringError(inconvertibleErrorCode(),
                                       "include site_e must be integer");
            siteE = static_cast<int>(*se);
          }

          // Optionals
          std::string text, target;
          std::optional<std::string> resolved;
          bool angled = false;
          std::optional<int> parent;

          if (const json::Value *textVal = obj->get("text")) {
            if (auto textStr = textVal->getAsString())
              text = std::string(*textStr);
          }
          if (const json::Value *tgtVal = obj->get("target")) {
            if (auto tgtStr = tgtVal->getAsString())
              target = std::string(*tgtStr);
          }
          if (const json::Value *rpVal = obj->get("resolved_path")) {
            if (auto rpStr = rpVal->getAsString())
              resolved = std::string(*rpStr);
          }
          if (const json::Value *aVal = obj->get("angled")) {
            if (auto aBool = aVal->getAsBoolean())
              angled = *aBool;
          }
          if (const json::Value *pVal = obj->get("parent")) {
            if (auto pInt = pVal->getAsInteger())
              parent = static_cast<int>(*pInt);
          }

          // Spans
          std::vector<RefoldModel::PPSpan> spans;
          if (const json::Value *spansVal = obj->get("spans")) {
            auto sp = parsePPSpans(*spansVal, "include.spans");
            if (!sp)
              return sp.takeError();
            spans = std::move(*sp);
          }

          // Optional pp_cover fields
          std::optional<int> coverBOpt, coverEOpt;
          if (const json::Value *ppcVal = obj->get("pp_cover")) {
            auto ppc = asObject(*ppcVal, "include.pp_cover");
            if (!ppc)
              return ppc.takeError();
            const json::Object *ppcObj = *ppc;

            auto begVal =
                requireField(*ppcObj, "begin", "include.pp_cover.begin");
            if (!begVal)
              return begVal.takeError();
            auto endVal = requireField(*ppcObj, "end", "include.pp_cover.end");
            if (!endVal)
              return endVal.takeError();

            auto bi = asInt(**begVal, "include.pp_cover.begin");
            if (!bi)
              return bi.takeError();
            auto ei = asInt(**endVal, "include.pp_cover.end");
            if (!ei)
              return ei.takeError();

            coverBOpt = *bi;
            coverEOpt = *ei;
          }

          IncludeItem inc(/*Id*/ id,
                          /*Subkind*/ std::string(*skStr),
                          /*Text*/ std::move(text),
                          /*SitePath*/ std::move(sitePath),
                          /*SiteB*/ siteB,
                          /*SiteE*/ siteE,
                          /*Target*/ std::move(target),
                          /*ResolvedPath*/ std::move(resolved),
                          /*Angled*/ angled,
                          /*Parent*/ std::move(parent),
                          /*Spans*/ std::move(spans),
                          /*CoverBeginOpt*/ coverBOpt,
                          /*CoverEndOpt*/ coverEOpt);

          model.includes_.push_back(std::move(inc));
        } else if (*skStr == "#define" || *skStr == "#undef") {
          MacroDirective md;
          // required
          {
            auto idVal = obj->get("id");
            if (!idVal)
              return createStringError(inconvertibleErrorCode(),
                                       "macro directive missing id");
            auto id = idVal->getAsInteger();
            if (!id)
              return createStringError(inconvertibleErrorCode(),
                                       "macro directive id must be integer");
            md.id = static_cast<int>(*id);
          }
          {
            auto textVal = obj->get("text");
            if (!textVal)
              return createStringError(inconvertibleErrorCode(),
                                       "macro directive missing text");
            auto textStr = textVal->getAsString();
            if (!textStr)
              return createStringError(inconvertibleErrorCode(),
                                       "macro directive text must be string");
            md.text = std::string(*textStr);
          }
          {
            auto spVal = obj->get("site_path");
            if (!spVal)
              return createStringError(inconvertibleErrorCode(),
                                       "macro directive missing site_path");
            auto spStr = spVal->getAsString();
            if (!spStr)
              return createStringError(
                  inconvertibleErrorCode(),
                  "macro directive site_path must be string");
            md.sitePath = std::string(*spStr);
          }
          {
            auto sbVal = obj->get("site_b");
            if (!sbVal)
              return createStringError(inconvertibleErrorCode(),
                                       "macro directive missing site_b");
            auto sb = sbVal->getAsInteger();
            if (!sb)
              return createStringError(
                  inconvertibleErrorCode(),
                  "macro directive site_b must be integer");
            md.siteB = static_cast<int>(*sb);
          }
          {
            auto seVal = obj->get("site_e");
            if (!seVal)
              return createStringError(inconvertibleErrorCode(),
                                       "macro directive missing site_e");
            auto se = seVal->getAsInteger();
            if (!se)
              return createStringError(
                  inconvertibleErrorCode(),
                  "macro directive site_e must be integer");
            md.siteE = static_cast<int>(*se);
          }

          if (const json::Value *ownIncIdVal = obj->get("owner_include_id")) {
            if (auto ownIncId = ownIncIdVal->getAsInteger())
              md.ownerIncludeId = static_cast<int>(*ownIncId);
          }

          if (const json::Value *spansVal = obj->get("spans")) {
            auto sp = parsePPSpans(*spansVal, "macro.directive.spans");
            if (!sp)
              return sp.takeError();
            md.spans = std::move(*sp);
          }

          md.subkind = std::string(*skStr);
          model.macroDirs_.push_back(std::move(md));
        } else if (*skStr == "#pragma") {
          PragmaDirective pd;
          // required
          {
            auto idVal = obj->get("id");
            if (!idVal)
              return createStringError(inconvertibleErrorCode(),
                                       "pragma missing id");
            auto id = idVal->getAsInteger();
            if (!id)
              return createStringError(inconvertibleErrorCode(),
                                       "pragma id must be integer");
            pd.id = static_cast<int>(*id);
          }
          {
            auto textVal = obj->get("text");
            if (!textVal)
              return createStringError(inconvertibleErrorCode(),
                                       "pragma missing text");
            auto textStr = textVal->getAsString();
            if (!textStr)
              return createStringError(inconvertibleErrorCode(),
                                       "pragma text must be string");
            pd.text = std::string(*textStr);
          }
          {
            auto spVal = obj->get("site_path");
            if (!spVal)
              return createStringError(inconvertibleErrorCode(),
                                       "pragma missing site_path");
            auto spStr = spVal->getAsString();
            if (!spStr)
              return createStringError(inconvertibleErrorCode(),
                                       "pragma site_path must be string");
            pd.sitePath = std::string(*spStr);
          }
          {
            auto sbVal = obj->get("site_b");
            if (!sbVal)
              return createStringError(inconvertibleErrorCode(),
                                       "pragma missing site_b");
            auto sb = sbVal->getAsInteger();
            if (!sb)
              return createStringError(inconvertibleErrorCode(),
                                       "pragma site_b must be integer");
            pd.siteB = static_cast<int>(*sb);
          }
          {
            auto seVal = obj->get("site_e");
            if (!seVal)
              return createStringError(inconvertibleErrorCode(),
                                       "pragma missing site_e");
            auto se = seVal->getAsInteger();
            if (!se)
              return createStringError(inconvertibleErrorCode(),
                                       "pragma site_e must be integer");
            pd.siteE = static_cast<int>(*se);
          }
          model.pragmas_.push_back(std::move(pd));
        } else {
          return createStringError(
              inconvertibleErrorCode(),
              "Unknown directive subkind '%s' at items[%zu]",
              skStr->str().c_str(), i);
        }
      } else if (*kindStr == "macro") {
        // ===== Required fields =====
        int id = 0;
        {
          auto idVal = obj->get("id");
          if (!idVal)
            return createStringError(inconvertibleErrorCode(),
                                     "macro item missing id");
          auto idInt = idVal->getAsInteger();
          if (!idInt)
            return createStringError(inconvertibleErrorCode(),
                                     "macro.id must be integer");
          id = static_cast<int>(*idInt);
        }

        std::string subkind;
        {
          auto skVal = obj->get("subkind");
          if (!skVal)
            return createStringError(inconvertibleErrorCode(),
                                     "macro item missing subkind");
          auto skStr = skVal->getAsString();
          if (!skStr)
            return createStringError(inconvertibleErrorCode(),
                                     "macro.subkind must be string");
          subkind = std::string(*skStr);
        }

        std::string name;
        {
          auto nameVal = obj->get("name");
          if (!nameVal)
            return createStringError(inconvertibleErrorCode(),
                                     "macro item missing name");
          auto nameStr = nameVal->getAsString();
          if (!nameStr)
            return createStringError(inconvertibleErrorCode(),
                                     "macro.name must be string");
          name = std::string(*nameStr);
        }

        // ===== Optionals =====
        std::optional<std::string> invocationText;
        if (const json::Value *itVal = obj->get("invocation_text")) {
          if (auto itStr = itVal->getAsString())
            invocationText = std::string(*itStr);
        }

        std::optional<std::string> invFile;
        if (const json::Value *ifVal = obj->get("inv_file")) {
          if (auto ifStr = ifVal->getAsString())
            invFile = std::string(*ifStr);
        }

        std::optional<int> invB;
        if (const json::Value *ibVal = obj->get("inv_b")) {
          if (auto ib = ibVal->getAsInteger())
            invB = static_cast<int>(*ib);
        }

        std::optional<int> invE;
        if (const json::Value *ieVal = obj->get("inv_e")) {
          if (auto ie = ieVal->getAsInteger())
            invE = static_cast<int>(*ie);
        }

        std::optional<int> ownerIncludeId;
        if (const json::Value *ownIncIdVal = obj->get("owner_include_id")) {
          if (auto ownIncId = ownIncIdVal->getAsInteger())
            ownerIncludeId = static_cast<int>(*ownIncId);
        }

        // spans (optional array)
        std::vector<RefoldModel::PPSpan> spans;
        if (const json::Value *spansVal = obj->get("spans")) {
          auto sp = parsePPSpans(*spansVal, "macro.spans");
          if (!sp)
            return sp.takeError();
          spans = std::move(*sp);
        }

        // pp_cover (if present, require both begin and end to be ints)
        std::optional<int> coverBOpt, coverEOpt;
        if (const json::Value *ppcVal = obj->get("pp_cover")) {
          auto ppc = asObject(*ppcVal, "macro.pp_cover");
          if (!ppc)
            return ppc.takeError();
          const json::Object *ppcObj = *ppc;

          auto bv = requireField(*ppcObj, "begin", "macro.pp_cover.begin");
          if (!bv)
            return bv.takeError();
          auto ev = requireField(*ppcObj, "end", "macro.pp_cover.end");
          if (!ev)
            return ev.takeError();

          auto bi = asInt(**bv, "macro.pp_cover.begin");
          if (!bi)
            return bi.takeError();
          auto ei = asInt(**ev, "macro.pp_cover.end");
          if (!ei)
            return ei.takeError();

          coverBOpt = *bi;
          coverEOpt = *ei;
        }

        MacroInvocation mi(/*Id*/ id,
                           /*Subkind*/ std::move(subkind),
                           /*Name*/ std::move(name),
                           /*InvocationText*/ std::move(invocationText),
                           /*InvFile*/ std::move(invFile),
                           /*InvB*/ std::move(invB),
                           /*InvE*/ std::move(invE),
                           /*OwnerIncludeId*/ std::move(ownerIncludeId),
                           /*Spans*/ std::move(spans),
                           /*CoverBeginOpt*/ coverBOpt,
                           /*CoverEndOpt*/ coverEOpt);

        model.macroInvs_.push_back(std::move(mi));
      } else if (*kindStr == "file") {
        FileItem fi;
        {
          auto idVal = obj->get("id");
          if (!idVal)
            return createStringError(inconvertibleErrorCode(),
                                     "file item missing id");
          auto id = idVal->getAsInteger();
          if (!id)
            return createStringError(inconvertibleErrorCode(),
                                     "file.id must be integer");
          fi.id = static_cast<int>(*id);
        }
        if (const json::Value *pathVal = obj->get("path")) {
          if (auto pathStr = pathVal->getAsString())
            fi.path = std::string(*pathStr);
        }
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
                                 kindStr->str().c_str(), i);
      }
    }
  }

  // slots (optional)
  if (const json::Value *slotsVal = root.get("slots")) {
    const json::Array *arr = slotsVal->getAsArray();
    if (!arr)
      return createStringError(inconvertibleErrorCode(),
                               "'slots' must be an array");
    model.slots_.reserve(arr->size());
    for (std::size_t i = 0; i < arr->size(); ++i) {
      const json::Value &val = (*arr)[i];
      const json::Object *obj = nullptr;
      if (auto eo = asObject(val, "slots[]"))
        obj = *eo;
      else
        return eo.takeError();

      RefoldModel::Slot slot;
      // required
      {
        auto idVal = obj->get("id");
        if (!idVal)
          return createStringError(inconvertibleErrorCode(),
                                   "slots[%zu] missing id", i);
        auto id = idVal->getAsInteger();
        if (!id)
          return createStringError(inconvertibleErrorCode(),
                                   "slots[%zu].id must be integer", i);
        slot.id = static_cast<int>(*id);
      }
      {
        auto fileVal = obj->get("file");
        if (!fileVal)
          return createStringError(inconvertibleErrorCode(),
                                   "slots[%zu] missing file", i);
        auto fileStr = fileVal->getAsString();
        if (!fileStr)
          return createStringError(inconvertibleErrorCode(),
                                   "slots[%zu].file must be string", i);
        slot.file = std::string(*fileStr);
      }
      {
        auto kindVal = obj->get("kind");
        if (!kindVal)
          return createStringError(inconvertibleErrorCode(),
                                   "slots[%zu] missing kind", i);
        auto kindStr = kindVal->getAsString();
        if (!kindStr)
          return createStringError(inconvertibleErrorCode(),
                                   "slots[%zu].kind must be string", i);
        slot.kind = std::string(*kindStr);
      }
      {
        auto bVal = obj->get("b");
        if (!bVal)
          return createStringError(inconvertibleErrorCode(),
                                   "slots[%zu] missing b", i);
        auto b = bVal->getAsInteger();
        if (!b)
          return createStringError(inconvertibleErrorCode(),
                                   "slots[%zu].b must be integer", i);
        slot.b = static_cast<int>(*b);
      }
      {
        auto eVal = obj->get("e");
        if (!eVal)
          return createStringError(inconvertibleErrorCode(),
                                   "slots[%zu] missing e", i);
        auto e = eVal->getAsInteger();
        if (!e)
          return createStringError(inconvertibleErrorCode(),
                                   "slots[%zu].e must be integer", i);
        slot.e = static_cast<int>(*e);
      }

      // optionals
      if (const json::Value *refVal = obj->get("ref")) {
        if (auto ref = refVal->getAsInteger())
          slot.ref = static_cast<int>(*ref);
      }
      if (const json::Value *ownIncIdVal = obj->get("owner_include_id")) {
        if (auto ownIncId = ownIncIdVal->getAsInteger())
          slot.ownerIncludeId = static_cast<int>(*ownIncId);
      }
      if (const json::Value *ppVal = obj->get("pp")) {
        if (auto pp = ppVal->getAsInteger())
          slot.pp = static_cast<int>(*pp);
      }

      model.slots_.push_back(std::move(slot));
    }
  }

  // conds (optional)
  if (const json::Value *condsVal = root.get("conds")) {
    const json::Array *arr = condsVal->getAsArray();
    if (!arr)
      return createStringError(inconvertibleErrorCode(),
                               "'conds' must be an array");
    model.conds_.reserve(arr->size());
    for (std::size_t i = 0; i < arr->size(); ++i) {
      const json::Value &val = (*arr)[i];
      const json::Object *obj = nullptr;
      if (auto eo = asObject(val, "conds[]"))
        obj = *eo;
      else
        return eo.takeError();

      CondGroup group;
      {
        auto idVal = obj->get("id");
        if (!idVal)
          return createStringError(inconvertibleErrorCode(),
                                   "conds[%zu] missing id", i);
        auto id = idVal->getAsInteger();
        if (!id)
          return createStringError(inconvertibleErrorCode(),
                                   "conds[%zu].id must be integer", i);
        group.id = static_cast<int>(*id);
      }
      {
        auto fileVal = obj->get("file");
        if (!fileVal)
          return createStringError(inconvertibleErrorCode(),
                                   "conds[%zu] missing file", i);
        auto fileStr = fileVal->getAsString();
        if (!fileStr)
          return createStringError(inconvertibleErrorCode(),
                                   "conds[%zu].file must be string", i);
        group.file = std::string(*fileStr);
      }
      if (const json::Value *pVal = obj->get("parent")) {
        if (auto p = pVal->getAsInteger())
          group.parent = static_cast<int>(*p);
      }
      {
        auto gbVal = obj->get("group_b");
        if (!gbVal)
          return createStringError(inconvertibleErrorCode(),
                                   "conds[%zu] missing group_b", i);
        auto gb = gbVal->getAsInteger();
        if (!gb)
          return createStringError(inconvertibleErrorCode(),
                                   "conds[%zu].group_b must be integer", i);
        group.groupB = static_cast<int>(*gb);
      }
      {
        auto geVal = obj->get("group_e");
        if (!geVal)
          return createStringError(inconvertibleErrorCode(),
                                   "conds[%zu] missing group_e", i);
        auto ge = geVal->getAsInteger();
        if (!ge)
          return createStringError(inconvertibleErrorCode(),
                                   "conds[%zu].group_e must be integer", i);
        group.groupE = static_cast<int>(*ge);
      }
      if (const json::Value *parIncIdVal = obj->get("parent_include_id")) {
        if (auto parIncId = parIncIdVal->getAsInteger())
          group.parentIncludeId = static_cast<int>(*parIncId);
      }

      // arms
      auto armsVal = obj->get("arms");
      if (!armsVal)
        return createStringError(inconvertibleErrorCode(),
                                 "conds[%zu] missing arms", i);
      const json::Array *arms = armsVal->getAsArray();
      if (!arms)
        return createStringError(inconvertibleErrorCode(),
                                 "conds[%zu].arms must be an array", i);

      group.arms.reserve(arms->size());
      for (std::size_t ai = 0; ai < arms->size(); ++ai) {
        const json::Value &armVal = (*arms)[ai];
        const json::Object *armObj = nullptr;
        if (auto eo = asObject(armVal, "arm"))
          armObj = *eo;
        else
          return eo.takeError();

        CondArm arm;
        {
          auto idVal = armObj->get("id");
          if (!idVal)
            return createStringError(inconvertibleErrorCode(),
                                     "arm missing id");
          auto id = idVal->getAsInteger();
          if (!id)
            return createStringError(inconvertibleErrorCode(),
                                     "arm.id must be integer");
          arm.id = static_cast<int>(*id);
        }
        {
          auto tagVal = armObj->get("tag");
          if (!tagVal)
            return createStringError(inconvertibleErrorCode(),
                                     "arm missing tag");
          auto tagStr = tagVal->getAsString();
          if (!tagStr)
            return createStringError(inconvertibleErrorCode(),
                                     "arm.tag must be string");
          arm.tag = std::string(*tagStr);
        }
        if (const json::Value *condVal = armObj->get("cond")) {
          if (auto condStr = condVal->getAsString())
            arm.cond = std::string(*condStr);
        }
        {
          auto bbVal = armObj->get("body_b");
          if (!bbVal)
            return createStringError(inconvertibleErrorCode(),
                                     "arm missing body_b");
          auto bb = bbVal->getAsInteger();
          if (!bb)
            return createStringError(inconvertibleErrorCode(),
                                     "arm.body_b must be integer");
          arm.bodyB = static_cast<int>(*bb);
        }
        {
          auto beVal = armObj->get("body_e");
          if (!beVal)
            return createStringError(inconvertibleErrorCode(),
                                     "arm missing body_e");
          auto be = beVal->getAsInteger();
          if (!be)
            return createStringError(inconvertibleErrorCode(),
                                     "arm.body_e must be integer");
          arm.bodyE = static_cast<int>(*be);
        }
        if (const json::Value *selVal = armObj->get("selected")) {
          if (auto sel = selVal->getAsBoolean())
            arm.selected = *sel;
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
  std::vector<const CondGroup *> out;
  if (!parentIncludeId.has_value()) {
    auto it = condsByFile_.find(file.str());
    if (it != condsByFile_.end())
      out = it->second;
    return out;
  }
  auto fit = condsByFileByOwner_.find(file.str());
  if (fit == condsByFileByOwner_.end())
    return out;
  auto oit = fit->second.find(*parentIncludeId);
  if (oit == fit->second.end())
    return out;
  return oit->second;
}

std::optional<RefoldModel::ArmRef>
RefoldModel::FindArmRefForByte(StringRef file,
                               const std::optional<int> &parentIncludeId,
                               int byteOffset) const {
  for (const CondGroup *group : GetCondGroups(file, parentIncludeId)) {
    if (group->groupB <= byteOffset && byteOffset < group->groupE) {
      for (const CondArm &arm : group->arms) {
        if (arm.containsByte(byteOffset))
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
  std::vector<TokMapEntry> out;
  if (span.begin < 0 || span.end < span.begin)
    return out;
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
