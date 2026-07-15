//===--- RefoldMacroRecursiveTupleGeneratedReplay.cpp ----------*- C++ -*-===//
//
// Recursive tuple-generated-callee replay boundary implementation.
//
// This translation unit is the intentionally small home for the recursive
// tuple-generated-callee theorem. It owns the private producer-ancestry graph,
// exact whole-formal forwarding composition, terminal generated-callee
// recognition, tuple-slice proof, terminal replay solving, and root tuple edit
// construction while keeping `RefoldMacroStandardArgsOnlyPatchBuilder` from
// growing another local resolver stack.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroRecursiveTupleGeneratedReplay.h"

#include "macro/RefoldMacroGeneratedCalleeReplayEngine.h"
#include "macro/RefoldMacroPatchProofCertifier.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroTopology.h"
#include "macro/RefoldMacroTupleHelpers.h"
#include "line-control/RefoldLineObserverLayout.h"
#include "source/RefoldSourceMapper.h"
#include "util/StringUtils.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <tuple>
#include <utility>

namespace clang {
namespace refold {
namespace {

/// Maximum recursive tuple-forwarding ancestry depth accepted from a map.
///
/// The bound protects the consumer from malformed producer graphs while staying
/// well above normal macro-forwarding chains.  A path that reaches the bound is
/// rejected rather than truncated, because skipping part of the ancestry would
/// weaken the recursive generated-callee theorem.
constexpr unsigned RecursiveTupleForwardingDepthLimit = 64;

/// Deterministic graph view over producer-recorded macro invocation ancestry.
///
/// Parent/child edges are derived only from `caller_macro_id`; source-byte
/// overlap is deliberately ignored.  The graph is private to the recursive
/// replay resolver so later composition code can consume exactly the evidence
/// required by the theorem without expanding `RefoldMacroTopology`'s public API
/// or creating a premature forwarding-graph service.
class MacroForwardingGraph {
public:
  /// Build invocation, child, and definition-directive indices for one model.
  explicit MacroForwardingGraph(const RefoldModel &model) {
    llvm::ArrayRef<RefoldModel::MacroInvocation> invocations =
        model.GetMacroInvocations();
    invocationsById_.reserve(invocations.size());
    childrenByCallerId_.reserve(invocations.size());

    for (const RefoldModel::MacroInvocation &invocation : invocations) {
      auto inserted =
          invocationsById_.insert(std::make_pair(invocation.id, &invocation));
      if (!inserted.second)
        duplicateInvocationIds_.insert(invocation.id);

      if (invocation.callerMacroId)
        childrenByCallerId_[*invocation.callerMacroId].push_back(&invocation);
    }

    for (auto &entry : childrenByCallerId_) {
      llvm::SmallVectorImpl<const RefoldModel::MacroInvocation *> &children =
          entry.second;
      std::stable_sort(
          children.begin(), children.end(),
          [](const RefoldModel::MacroInvocation *lhs,
             const RefoldModel::MacroInvocation *rhs) {
            return lhs->id < rhs->id;
          });
    }

    llvm::ArrayRef<RefoldModel::MacroDirective> directives =
        model.GetMacroDirectives();
    definitionsById_.reserve(directives.size());
    for (const RefoldModel::MacroDirective &directive : directives) {
      if (directive.subkind != "#define")
        continue;
      auto inserted =
          definitionsById_.insert(std::make_pair(directive.id, &directive));
      if (!inserted.second)
        duplicateDefinitionIds_.insert(directive.id);
    }
  }

  /// Resolve a unique producer macro invocation ID.
  const RefoldModel::MacroInvocation *FindInvocation(uint64_t id) const {
    if (duplicateInvocationIds_.count(id))
      return nullptr;
    auto it = invocationsById_.find(id);
    if (it == invocationsById_.end())
      return nullptr;
    return it->second;
  }

  /// Return direct children whose `caller_macro_id` is \p callerId.
  llvm::ArrayRef<const RefoldModel::MacroInvocation *>
  ChildrenOf(uint64_t callerId) const {
    auto it = childrenByCallerId_.find(callerId);
    if (it == childrenByCallerId_.end())
      return {};
    return it->second;
  }

  /// Resolve the unique producer-recorded definition directive for an invocation.
  const RefoldModel::MacroDirective *
  DefinitionFor(const RefoldModel::MacroInvocation &invocation) const {
    if (!invocation.definitionDirectiveId ||
        duplicateDefinitionIds_.count(*invocation.definitionDirectiveId))
      return nullptr;
    auto it = definitionsById_.find(*invocation.definitionDirectiveId);
    if (it == definitionsById_.end())
      return nullptr;
    return it->second;
  }

  /// Return true iff every descendant path from \p root is acyclic and bounded.
  ///
  /// This helper performs the graph-level rejection required before later
  /// theorem composition.  Revisiting an invocation ID on the active DFS stack
  /// rejects the candidate path outright; exceeding the hard depth limit is
  /// also treated as a malformed graph rather than an invitation to continue
  /// with partial evidence.
  bool HasAcyclicBoundedDescendants(
      const RefoldModel::MacroInvocation &root) const {
    llvm::DenseSet<uint64_t> active;
    return HasAcyclicBoundedDescendants(root, 0, active);
  }

private:
  bool HasAcyclicBoundedDescendants(
      const RefoldModel::MacroInvocation &invocation, unsigned depth,
      llvm::DenseSet<uint64_t> &active) const {
    if (depth >= RecursiveTupleForwardingDepthLimit ||
        duplicateInvocationIds_.count(invocation.id))
      return false;
    if (!active.insert(invocation.id).second)
      return false;

    for (const RefoldModel::MacroInvocation *child : ChildrenOf(invocation.id)) {
      if (!child ||
          !HasAcyclicBoundedDescendants(*child, depth + 1, active)) {
        active.erase(invocation.id);
        return false;
      }
    }

    active.erase(invocation.id);
    return true;
  }

  llvm::DenseMap<uint64_t, const RefoldModel::MacroInvocation *>
      invocationsById_;
  llvm::DenseSet<uint64_t> duplicateInvocationIds_;
  llvm::DenseMap<uint64_t,
                 llvm::SmallVector<const RefoldModel::MacroInvocation *, 4>>
      childrenByCallerId_;
  llvm::DenseMap<uint64_t, const RefoldModel::MacroDirective *>
      definitionsById_;
  llvm::DenseSet<uint64_t> duplicateDefinitionIds_;
};

/// Composed provenance for one formal in the current invocation.
///
/// A present value is the root formal index that supplied the current formal by
/// exact whole-formal forwarding.  Missing values are never guessed through;
/// any later edge that depends on an unmapped formal is rejected.
using RootFormalBinding = std::optional<uint32_t>;

/// Formal provenance state at one invocation in a forwarding chain.
struct ComposedFormalState {
  /// Invocation whose local formals are described by `rootFormalByLocalFormal`.
  const RefoldModel::MacroInvocation *invocation = nullptr;
  /// Current formal index -> root formal index, when exactly proven.
  llvm::SmallVector<RootFormalBinding, 8> rootFormalByLocalFormal;
};

/// Terminal generated-callee edge composed to the root selector formal.
///
/// This carrier records only the proof established by the terminal-edge step:
/// the generated invocation's callee spelling came from one caller formal, and
/// that caller formal has already been composed to one root formal.  Tuple
/// actual slicing, replay solving, and root tuple editing are intentionally left
/// to later theorem stages.
struct TerminalGeneratedCalleeEdge {
  /// Invocation whose replacement text contains the generated callee use.
  const RefoldModel::MacroInvocation *parentInvocation = nullptr;
  /// Terminal generated function-like invocation, such as `ADD`.
  const RefoldModel::MacroInvocation *terminalInvocation = nullptr;
  /// Exact definition directive used by `terminalInvocation`.
  const RefoldModel::MacroDirective *terminalDefinition = nullptr;
  /// Parent formal index that produced the terminal callee token.
  uint32_t callerFormalIndex = 0;
  /// Root formal index reached by composing `callerFormalIndex`.
  uint32_t rootCalleeFormalIndex = 0;
};

/// Exact binding between one terminal actual and one root tuple element.
///
/// Absolute source byte ranges prove identity with the terminal invocation
/// actual.  Payload-relative byte ranges are the durable slice coordinates used
/// by the recursive tuple-generated-callee witness and later tuple edit step.
struct RootTupleSliceBinding {
  /// Terminal generated invocation actual index, in invocation order.
  uint32_t generatedActualIndex = 0;
  /// Terminal macro formal index matched by `generatedActualIndex`.
  uint32_t generatedFormalIndex = 0;
  /// Root invocation formal that owns the source-spelled tuple.
  uint32_t rootTupleFormalIndex = 0;
  /// Element payload begin, relative to the text between tuple parentheses.
  uint64_t rootTuplePayloadByteBegin = 0;
  /// Element payload end, relative to the text between tuple parentheses.
  uint64_t rootTuplePayloadByteEnd = 0;
  /// Absolute source begin for the same tuple element.
  uint64_t absoluteByteBegin = 0;
  /// Absolute source end for the same tuple element.
  uint64_t absoluteByteEnd = 0;
};

/// Proven mapping from terminal actuals to one unique root tuple formal.
struct RootTupleSliceDerivation {
  /// Root formal whose source argument parsed as the matching tuple.
  uint32_t rootTupleFormalIndex = 0;
  /// One exact source-range binding per terminal generated actual.
  llvm::SmallVector<RootTupleSliceBinding, 8> actualSlices;
};

/// Complete proof path from a root invocation to one terminal generated callee.
///
/// The carrier deliberately stores only evidence already proven by earlier
/// private theorem layers: producer ancestry, exact whole-formal composition,
/// terminal caller-param callee origin, and exact terminal-actual-to-root-tuple
/// byte-slice bindings.  Replay solving and tuple edit construction consume this
/// path in later steps rather than re-deriving graph facts.
struct ComposedGeneratedCalleePath {
  /// Root invocation whose source tuple argument may eventually be edited.
  const RefoldModel::MacroInvocation *rootInvocation = nullptr;
  /// Terminal generated function-like invocation reached from the root.
  const RefoldModel::MacroInvocation *terminalInvocation = nullptr;
  /// Exact definition directive used by `terminalInvocation`.
  const RefoldModel::MacroDirective *terminalDefinition = nullptr;
  /// Root formal whose spelling selected the generated callee.
  uint32_t rootCalleeFormalIndex = 0;
  /// Unique root tuple formal whose elements supplied terminal actuals.
  uint32_t rootTupleFormalIndex = 0;
  /// Exact terminal-actual-to-root-tuple element slice bindings.
  llvm::SmallVector<RootTupleSliceBinding, 8> actualSlices;
};


/// Formal slice state after following one generated-callee invocation.
///
/// The first recursive tuple theorem level proves that a root selector formal
/// selected a generated callee, and that each generated callee actual is a
/// top-level element of one root tuple formal.  A generated callee may itself
/// forward a tuple-shaped actual into another generated callee, for example
/// `ADD(f, t) -> f t` followed by `SUB(a, b)`.  This private state carries the
/// root-tuple slice owned by each current generated formal so such descendant
/// generated-callee edges can be followed without re-inferring anything from
/// source overlap.
struct GeneratedTupleFormalState {
  /// Current generated invocation described by this state.
  const RefoldModel::MacroInvocation *invocation = nullptr;
  /// Exact definition directive used by `invocation`.
  const RefoldModel::MacroDirective *definition = nullptr;
  /// Original root formal that selected the first generated callee.
  uint32_t rootCalleeFormalIndex = 0;
  /// Root tuple formal that owns every slice in this state.
  uint32_t rootTupleFormalIndex = 0;
  /// Current generated formal index -> exact root tuple payload slice.
  llvm::SmallVector<std::optional<RootTupleSliceBinding>, 8>
      rootTupleSliceByLocalFormal;
  /// Exact actual slices for `invocation`, in generated formal order.
  llvm::SmallVector<RootTupleSliceBinding, 8> actualSlices;
};

/// Unique terminal generated-callee replay result for the composed path.
///
/// The recursive resolver uses this internal carrier to keep the old terminal
/// actuals recovered from root tuple slices beside the generated-callee engine's
/// unique old/new replay solution.  The separate tuple edit applier consumes
/// this carrier after replay solving so text reconstruction stays isolated from
/// generated-callee inversion.
struct RecursiveTupleGeneratedReplaySolution {
  /// Old terminal actual spellings in terminal callee formal order.
  llvm::SmallVector<std::string, 8> oldActuals;
  /// Unique old replay solution in terminal callee formal order.
  llvm::SmallVector<std::string, 8> oldSolvedActuals;
  /// Unique edited replay solution in terminal callee formal order.
  llvm::SmallVector<std::string, 8> newSolvedActuals;
  /// True when terminal replay accepted stringification evidence.
  bool usesStringification = false;
  /// True when terminal replay accepted token-paste evidence.
  bool usesPaste = false;
};

/// One accepted edit inside the source-spelled root tuple payload.
///
/// The coordinates are relative to the complete root tuple actual spelling, not
/// the whole invocation.  They always name a trimmed tuple element slice already
/// proven to correspond to one terminal generated-callee actual.
struct RootTupleElementEdit {
  /// Half-open begin byte in the root tuple actual spelling.
  size_t begin = 0;
  /// Half-open end byte in the root tuple actual spelling.
  size_t end = 0;
  /// Replacement spelling for the tuple element slice.
  std::string replacement;
};

/// Rewritten root invocation after applying recursive tuple element edits.
struct RecursiveTupleInvocationRewrite {
  /// Complete root invocation spelling after tuple element replacement.
  std::string rewrittenInvocation;
  /// Rewritten tuple actual spelling, including its original parentheses.
  std::string rewrittenTupleActual;
};

/// Outcome of testing one child edge for terminal generated-callee admissibility.
enum class TerminalGeneratedCalleeEdgeStatus {
  /// The child is not a caller-param generated callee edge.
  NotTerminal,
  /// The child is a fully proven terminal generated-callee edge.
  Accepted,
  /// The child claims to be terminal generated-callee evidence but is invalid.
  Rejected
};

/// Return whether any formal can participate in variadic or `__VA_OPT__` replay.
bool macroDefinitionHasVariadicFormal(
    const RefoldModel::MacroDirective &definition);

/// Return the trimmed actual bounds in invocation-text coordinates.
///
/// `arg_refs` use raw invocation-text coordinates, while `inv_arg_ranges` use
/// physical source coordinates.  This helper rebases the latter and trims only
/// preprocessing whitespace.  Exact whole-formal forwarding is then just byte
/// equality between this range and the single producer `arg_ref` range.
std::optional<std::pair<uint32_t, uint32_t>> trimmedInvocationActualBounds(
    const RefoldModel::MacroInvocation &invocation, uint32_t formalIndex) {
  if (!invocation.invText || !invocation.invB ||
      formalIndex >= invocation.invArgRanges.size())
    return std::nullopt;

  const auto &range = invocation.invArgRanges[formalIndex];
  if (!range.first || !range.second || *range.second < *range.first ||
      *range.first < *invocation.invB)
    return std::nullopt;

  const uint64_t relBegin = *range.first - *invocation.invB;
  const uint64_t relEnd = *range.second - *invocation.invB;
  if (relEnd < relBegin || relEnd > invocation.invText->size() ||
      relEnd > std::numeric_limits<uint32_t>::max())
    return std::nullopt;

  llvm::StringRef actualText = invocation.invText->slice(
      static_cast<size_t>(relBegin), static_cast<size_t>(relEnd));
  size_t trimBegin = 0;
  size_t trimEnd = actualText.size();
  std::tie(trimBegin, trimEnd) =
      stringutils::trimWsRange(actualText, 0, actualText.size());
  if (trimBegin == trimEnd)
    return std::nullopt;

  const uint64_t trimmedBegin = relBegin + trimBegin;
  const uint64_t trimmedEnd = relBegin + trimEnd;
  if (trimmedEnd > std::numeric_limits<uint32_t>::max())
    return std::nullopt;
  return std::make_pair(static_cast<uint32_t>(trimmedBegin),
                        static_cast<uint32_t>(trimmedEnd));
}

/// Compose exact whole-formal forwarding through literal-callee descendants.
///
/// The composer admits only the forwarding shape required by the recursive
/// tuple-generated-callee theorem: every child formal must be spelled as exactly
/// one already-composed caller formal, modulo surrounding whitespace, and that
/// fact must be represented by exactly one producer `arg_ref`.  It deliberately
/// rejects transformed actuals, partial slices, tuple-ref element forwarding,
/// stringification, token paste, opaque callees, and paste-generated callees.
class WholeFormalForwardingComposer {
public:
  explicit WholeFormalForwardingComposer(const MacroForwardingGraph &graph)
      : graph_(graph) {}

  /// Collect all literal-forwarding states reachable from \p root.
  ///
  /// The root state maps each root formal to itself.  Descendant states are
  /// included only when the edge from the parent to the child is exact
  /// whole-formal forwarding.  Non-matching children are ignored here because
  /// terminal generated-callee recognition is handled by a separate theorem
  /// layer; malformed cycles or excessive depth still reject the traversal.
  bool CollectLiteralForwardingStates(
      const RefoldModel::MacroInvocation &root,
      const RefoldModel::MacroDirective &rootDefinition,
      llvm::SmallVectorImpl<ComposedFormalState> &states) const {
    std::optional<ComposedFormalState> rootState =
        MakeRootFormalState(root, rootDefinition);
    if (!rootState)
      return false;

    llvm::DenseSet<uint64_t> active;
    return CollectLiteralForwardingStates(*rootState, 0, active, states);
  }

private:
  std::optional<ComposedFormalState> MakeRootFormalState(
      const RefoldModel::MacroInvocation &root,
      const RefoldModel::MacroDirective &rootDefinition) const {
    if (!rootDefinition.functionLike)
      return std::nullopt;
    if (rootDefinition.defParams.size() >
        std::numeric_limits<uint32_t>::max())
      return std::nullopt;
    if (!root.definitionDirectiveId ||
        *root.definitionDirectiveId != rootDefinition.id)
      return std::nullopt;

    const uint32_t formalCount =
        static_cast<uint32_t>(rootDefinition.defParams.size());

    ComposedFormalState state;
    state.invocation = &root;
    state.rootFormalByLocalFormal.reserve(formalCount);
    for (uint32_t formalIndex = 0; formalIndex < formalCount; ++formalIndex)
      state.rootFormalByLocalFormal.push_back(formalIndex);
    return state;
  }

  bool CollectLiteralForwardingStates(
      const ComposedFormalState &state, unsigned depth,
      llvm::DenseSet<uint64_t> &active,
      llvm::SmallVectorImpl<ComposedFormalState> &states) const {
    if (!state.invocation || depth >= RecursiveTupleForwardingDepthLimit)
      return false;
    if (!active.insert(state.invocation->id).second)
      return false;

    states.push_back(state);
    for (const RefoldModel::MacroInvocation *child :
         graph_.ChildrenOf(state.invocation->id)) {
      if (!child) {
        active.erase(state.invocation->id);
        return false;
      }

      std::optional<ComposedFormalState> childState =
          TryComposeLiteralForwardingEdge(state, *child);
      if (!childState)
        continue;
      if (!CollectLiteralForwardingStates(*childState, depth + 1, active,
                                          states)) {
        active.erase(state.invocation->id);
        return false;
      }
    }

    active.erase(state.invocation->id);
    return true;
  }

  std::optional<ComposedFormalState> TryComposeLiteralForwardingEdge(
      const ComposedFormalState &parentState,
      const RefoldModel::MacroInvocation &child) const {
    if (!parentState.invocation || !child.callerMacroId ||
        *child.callerMacroId != parentState.invocation->id)
      return std::nullopt;
    if (child.calleeOrigin.kind != MacroCalleeOriginKind::LiteralMacroName)
      return std::nullopt;
    if (!child.stringifySpans.empty() || !child.pasteSpans.empty() ||
        !child.pasteTokens.empty())
      return std::nullopt;

    const RefoldModel::MacroDirective *childDefinition =
        graph_.DefinitionFor(child);
    if (!childDefinition || !childDefinition->functionLike ||
        macroDefinitionHasVariadicFormal(*childDefinition))
      return std::nullopt;

    if (childDefinition->defParams.size() >
        std::numeric_limits<uint32_t>::max())
      return std::nullopt;
    const uint32_t formalCount =
        static_cast<uint32_t>(childDefinition->defParams.size());
    if (formalCount == 0 || child.invArgRanges.size() != formalCount ||
        child.argRefs.size() != formalCount)
      return std::nullopt;
    if (!child.argDeps.empty() && child.argDeps.size() != formalCount)
      return std::nullopt;
    if (!child.argTupleRefs.empty() && child.argTupleRefs.size() != formalCount)
      return std::nullopt;

    ComposedFormalState childState;
    childState.invocation = &child;
    childState.rootFormalByLocalFormal.assign(formalCount, std::nullopt);

    for (uint32_t formalIndex = 0; formalIndex < formalCount; ++formalIndex) {
      const auto &refs = child.argRefs[formalIndex];
      if (refs.size() != 1)
        return std::nullopt;
      if (formalIndex < child.argTupleRefs.size() &&
          !child.argTupleRefs[formalIndex].empty())
        return std::nullopt;
      if (!child.argDeps.empty()) {
        const auto &deps = child.argDeps[formalIndex];
        if (deps.size() != 1 || deps.front() != refs.front().callerParamIndex)
          return std::nullopt;
      }

      std::optional<std::pair<uint32_t, uint32_t>> trimmedBounds =
          trimmedInvocationActualBounds(child, formalIndex);
      if (!trimmedBounds)
        return std::nullopt;

      const RefoldModel::InvArgRef &ref = refs.front();
      if (ref.byteBegin != trimmedBounds->first ||
          ref.byteEnd != trimmedBounds->second)
        return std::nullopt;
      if (ref.callerParamIndex >= parentState.rootFormalByLocalFormal.size())
        return std::nullopt;

      RootFormalBinding rootFormal =
          parentState.rootFormalByLocalFormal[ref.callerParamIndex];
      if (!rootFormal)
        return std::nullopt;
      childState.rootFormalByLocalFormal[formalIndex] = *rootFormal;
    }

    return childState;
  }

  const MacroForwardingGraph &graph_;
};

/// Source range for one invocation actual in physical file coordinates.
struct PhysicalInvocationActualRange {
  /// File that owns the source spelling.
  llvm::StringRef file;
  /// Half-open absolute source range for the actual spelling.
  uint64_t begin = 0;
  uint64_t end = 0;
};

/// Source text for one root invocation actual.
struct RootInvocationActualText {
  /// File that owns the source spelling.
  llvm::StringRef file;
  /// Half-open absolute source range for the actual spelling.
  uint64_t absoluteBegin = 0;
  uint64_t absoluteEnd = 0;
  /// Actual spelling sliced from the root invocation text.
  llvm::StringRef text;
};

/// Return the physical source range recorded for one invocation actual.
std::optional<PhysicalInvocationActualRange> physicalInvocationActualRange(
    const RefoldModel::MacroInvocation &invocation, uint32_t formalIndex) {
  if (!invocation.invFile || formalIndex >= invocation.invArgRanges.size())
    return std::nullopt;

  const auto &range = invocation.invArgRanges[formalIndex];
  if (!range.first || !range.second || *range.second <= *range.first)
    return std::nullopt;

  PhysicalInvocationActualRange out;
  out.file = *invocation.invFile;
  out.begin = *range.first;
  out.end = *range.second;
  return out;
}

/// Return source text and physical bounds for one root invocation actual.
std::optional<RootInvocationActualText> rootInvocationActualText(
    const RefoldModel::MacroInvocation &invocation, uint32_t formalIndex) {
  if (!invocation.invText || !invocation.invB)
    return std::nullopt;

  std::optional<PhysicalInvocationActualRange> range =
      physicalInvocationActualRange(invocation, formalIndex);
  if (!range)
    return std::nullopt;
  if (range->begin < *invocation.invB)
    return std::nullopt;

  const uint64_t textBegin = range->begin - *invocation.invB;
  const uint64_t textEnd = range->end - *invocation.invB;
  if (textEnd < textBegin || textEnd > invocation.invText->size())
    return std::nullopt;

  RootInvocationActualText out;
  out.file = range->file;
  out.absoluteBegin = range->begin;
  out.absoluteEnd = range->end;
  out.text = invocation.invText->slice(static_cast<size_t>(textBegin),
                                      static_cast<size_t>(textEnd));
  return out;
}

/// Return whether two half-open ranges overlap.
bool rangesOverlap(uint64_t lhsBegin, uint64_t lhsEnd, uint64_t rhsBegin,
                   uint64_t rhsEnd) {
  return lhsBegin < rhsEnd && rhsBegin < lhsEnd;
}

/// Return whether any formal can participate in variadic or `__VA_OPT__` replay.
///
/// The first recursive tuple theorem intentionally supports only fixed-arity
/// forwarding.  Treating variadic definitions as in-scope would require extra
/// proof that pack collection, empty-pack elision, and `__VA_OPT__` branch
/// selection stayed stable through the forwarding chain, so this resolver
/// rejects them before composing a path.
bool macroDefinitionHasVariadicFormal(
    const RefoldModel::MacroDirective &definition) {
  return llvm::any_of(definition.defParams,
                     [](const RefoldModel::MacroDefParam &param) {
                       return param.variadic;
                     });
}

/// Validate that the root replay request has exact producer callsite evidence.
///
/// Later theorem layers use the root invocation text as the byte coordinate
/// space for tuple edits and compare producer physical argument ranges against
/// parsed invocation-content ranges.  This gate rejects stale, normalized, or
/// partially recovered inputs before graph composition can attach a recursive
/// proof to them.
bool recursiveTupleReplayRequestHasUsableRootEvidence(
    const RecursiveTupleGeneratedReplayRequest &request) {
  const RefoldModel::MacroInvocation &root = request.rootInvocation;
  const RefoldModel::MacroDirective &definition = request.rootDefinition;

  if (!definition.functionLike || macroDefinitionHasVariadicFormal(definition))
    return false;
  if (!root.definitionDirectiveId || *root.definitionDirectiveId != definition.id)
    return false;
  if (root.calleeOrigin.kind != MacroCalleeOriginKind::LiteralMacroName)
    return false;
  if (!root.stringifySpans.empty() || !root.pasteSpans.empty() ||
      !root.pasteTokens.empty())
    return false;
  if (!root.invText || !root.invFile || !root.invB || !root.invE ||
      *root.invB >= *root.invE)
    return false;
  if (static_cast<uint64_t>(root.invText->size()) != *root.invE - *root.invB)
    return false;
  if (llvm::StringRef(*root.invText) != request.baseInvocationText)
    return false;

  if (definition.defParams.size() != request.invocationArgRanges.size() ||
      root.invArgRanges.size() != request.invocationArgRanges.size())
    return false;

  for (size_t formalIndex = 0; formalIndex < request.invocationArgRanges.size();
       ++formalIndex) {
    const auto requestedRange = request.invocationArgRanges[formalIndex];
    if (requestedRange.second < requestedRange.first ||
        requestedRange.second > request.baseInvocationText.size())
      return false;

    const auto &physicalRange = root.invArgRanges[formalIndex];
    if (!physicalRange.first || !physicalRange.second ||
        *physicalRange.second < *physicalRange.first ||
        *physicalRange.first < *root.invB || *physicalRange.second > *root.invE)
      return false;

    const uint64_t relativeBegin = *physicalRange.first - *root.invB;
    const uint64_t relativeEnd = *physicalRange.second - *root.invB;
    if (relativeBegin != requestedRange.first ||
        relativeEnd != requestedRange.second)
      return false;
  }

  return true;
}

/// Proves terminal actuals are exact elements of one root tuple argument.
///
/// This deriver uses source ranges only after the caller graph and formal
/// forwarding layers have already proven ancestry.  It accepts exactly one root
/// formal whose source-spelled actual is a parenthesized tuple and whose parsed
/// element ranges byte-match every terminal generated actual.  No tuple formal
/// is selected by spelling similarity, source overlap, or macro names.
class RootTupleSliceDeriver {
public:
  explicit RootTupleSliceDeriver(const clang::LangOptions &lexLang)
      : lexLang_(lexLang) {}

  /// Derive the unique root tuple formal and element slices for \p edge.
  std::optional<RootTupleSliceDerivation> DeriveUniqueRootTupleSlices(
      const RecursiveTupleGeneratedReplayRequest &request,
      const TerminalGeneratedCalleeEdge &edge) const {
    if (!edge.terminalInvocation || !edge.terminalDefinition)
      return std::nullopt;
    if (!edge.terminalDefinition->functionLike)
      return std::nullopt;
    if (edge.terminalDefinition->defParams.size() >
        std::numeric_limits<uint32_t>::max())
      return std::nullopt;

    const uint32_t terminalActualCount =
        static_cast<uint32_t>(edge.terminalDefinition->defParams.size());
    if (terminalActualCount == 0 ||
        edge.terminalInvocation->invArgRanges.size() != terminalActualCount)
      return std::nullopt;
    if (macroDefinitionHasVariadicFormal(*edge.terminalDefinition))
      return std::nullopt;

    llvm::SmallVector<PhysicalInvocationActualRange, 8> terminalActualRanges;
    terminalActualRanges.reserve(terminalActualCount);
    for (uint32_t actualIndex = 0; actualIndex < terminalActualCount;
         ++actualIndex) {
      std::optional<PhysicalInvocationActualRange> range =
          physicalInvocationActualRange(*edge.terminalInvocation, actualIndex);
      if (!range)
        return std::nullopt;
      terminalActualRanges.push_back(*range);
    }

    llvm::SmallVector<RootTupleSliceDerivation, 2> matches;
    if (request.rootDefinition.defParams.size() >
        std::numeric_limits<uint32_t>::max())
      return std::nullopt;
    const uint32_t rootFormalCount =
        static_cast<uint32_t>(request.rootDefinition.defParams.size());
    for (uint32_t rootFormalIndex = 0; rootFormalIndex < rootFormalCount;
         ++rootFormalIndex) {
      if (rootFormalIndex == edge.rootCalleeFormalIndex)
        continue;

      std::optional<RootTupleSliceDerivation> derivation =
          TryDeriveForRootFormal(request, terminalActualRanges,
                                 terminalActualCount, rootFormalIndex);
      if (derivation)
        matches.push_back(*derivation);
      if (matches.size() > 1)
        return std::nullopt;
    }

    if (matches.size() != 1)
      return std::nullopt;
    return matches.front();
  }

private:
  std::optional<RootTupleSliceDerivation> TryDeriveForRootFormal(
      const RecursiveTupleGeneratedReplayRequest &request,
      llvm::ArrayRef<PhysicalInvocationActualRange> terminalActualRanges,
      uint32_t terminalActualCount, uint32_t rootFormalIndex) const {
    std::optional<RootInvocationActualText> rootActual =
        rootInvocationActualText(request.rootInvocation, rootFormalIndex);
    if (!rootActual)
      return std::nullopt;

    llvm::SmallVector<TupleElementSlice, 8> tupleElements;
    size_t trimmedBegin = 0;
    size_t trimmedEnd = rootActual->text.size();
    std::tie(trimmedBegin, trimmedEnd) =
        stringutils::trimWsRange(rootActual->text, 0, rootActual->text.size());
    if (trimmedEnd <= trimmedBegin + 2)
      return std::nullopt;

    llvm::StringRef trimmedActual =
        rootActual->text.slice(trimmedBegin, trimmedEnd);
    if (!trimmedActual.starts_with("(") || !trimmedActual.ends_with(")"))
      return std::nullopt;

    llvm::StringRef tuplePayload = trimmedActual.drop_front().drop_back();
    if (!splitTopLevelTupleElementsWithLexer(tuplePayload, lexLang_,
                                             tupleElements))
      return std::nullopt;
    if (tupleElements.size() != terminalActualCount ||
        tupleElements.size() < 2)
      return std::nullopt;

    const uint64_t payloadAbsoluteBegin =
        rootActual->absoluteBegin + static_cast<uint64_t>(trimmedBegin) + 1;
    RootTupleSliceDerivation derivation;
    derivation.rootTupleFormalIndex = rootFormalIndex;
    derivation.actualSlices.reserve(tupleElements.size());

    for (uint32_t actualIndex = 0; actualIndex < terminalActualCount;
         ++actualIndex) {
      const TupleElementSlice &element = tupleElements[actualIndex];
      if (element.trimEnd <= element.trimBegin)
        return std::nullopt;

      const uint64_t elementAbsoluteBegin =
          payloadAbsoluteBegin + static_cast<uint64_t>(element.trimBegin);
      const uint64_t elementAbsoluteEnd =
          payloadAbsoluteBegin + static_cast<uint64_t>(element.trimEnd);
      if (elementAbsoluteBegin <= rootActual->absoluteBegin ||
          elementAbsoluteEnd >= rootActual->absoluteEnd)
        return std::nullopt;

      const PhysicalInvocationActualRange &actualRange =
          terminalActualRanges[actualIndex];
      if (actualRange.file != rootActual->file)
        return std::nullopt;
      if (actualRange.begin != elementAbsoluteBegin ||
          actualRange.end != elementAbsoluteEnd)
        return std::nullopt;

      for (const RootTupleSliceBinding &existing : derivation.actualSlices) {
        if (rangesOverlap(existing.rootTuplePayloadByteBegin,
                          existing.rootTuplePayloadByteEnd,
                          element.trimBegin, element.trimEnd))
          return std::nullopt;
      }

      RootTupleSliceBinding binding;
      binding.generatedActualIndex = actualIndex;
      binding.generatedFormalIndex = actualIndex;
      binding.rootTupleFormalIndex = rootFormalIndex;
      binding.rootTuplePayloadByteBegin = element.trimBegin;
      binding.rootTuplePayloadByteEnd = element.trimEnd;
      binding.absoluteByteBegin = elementAbsoluteBegin;
      binding.absoluteByteEnd = elementAbsoluteEnd;
      derivation.actualSlices.push_back(binding);
    }

    return derivation;
  }

  const clang::LangOptions &lexLang_;
};

/// Recognizes generated-callee terminal edges from composed forwarding states.
///
/// The recognizer consumes only producer-recorded ancestry and callee-origin
/// metadata.  It does not use source overlap or macro names to infer terminal
/// ancestry.  A caller-param child is accepted only when exactly one caller
/// formal produced the callee token, that formal has a composed root binding,
/// and the terminal invocation's definition directive resolves exactly.
class TerminalGeneratedCalleeEdgeRecognizer {
public:
  explicit TerminalGeneratedCalleeEdgeRecognizer(
      const MacroForwardingGraph &graph)
      : graph_(graph) {}

  /// Collect all proven terminal generated-callee edges under composed states.
  ///
  /// The result order is deterministic because forwarding states are produced
  /// by sorted `caller_macro_id` traversal, and children within each parent are
  /// also sorted by invocation id.  Invalid caller-param terminal evidence
  /// rejects the recursive theorem layer instead of being repaired or skipped.
  bool CollectTerminalGeneratedCalleeEdges(
      llvm::ArrayRef<ComposedFormalState> states,
      llvm::SmallVectorImpl<TerminalGeneratedCalleeEdge> &terminalEdges) const {
    for (const ComposedFormalState &state : states) {
      if (!state.invocation)
        return false;

      for (const RefoldModel::MacroInvocation *child :
           graph_.ChildrenOf(state.invocation->id)) {
        if (!child)
          return false;

        TerminalGeneratedCalleeEdge edge;
        TerminalGeneratedCalleeEdgeStatus status =
            TryRecognizeTerminalGeneratedCalleeEdge(state, *child, edge);
        switch (status) {
        case TerminalGeneratedCalleeEdgeStatus::NotTerminal:
          continue;
        case TerminalGeneratedCalleeEdgeStatus::Accepted:
          terminalEdges.push_back(edge);
          continue;
        case TerminalGeneratedCalleeEdgeStatus::Rejected:
          return false;
        }
        llvm_unreachable("Unhandled terminal generated-callee edge status");
      }
    }

    return true;
  }

private:
  TerminalGeneratedCalleeEdgeStatus TryRecognizeTerminalGeneratedCalleeEdge(
      const ComposedFormalState &parentState,
      const RefoldModel::MacroInvocation &child,
      TerminalGeneratedCalleeEdge &edge) const {
    switch (child.calleeOrigin.kind) {
    case MacroCalleeOriginKind::CallerParam:
      break;
    case MacroCalleeOriginKind::LiteralMacroName:
      return TerminalGeneratedCalleeEdgeStatus::NotTerminal;
    case MacroCalleeOriginKind::Paste:
    case MacroCalleeOriginKind::Opaque:
      return TerminalGeneratedCalleeEdgeStatus::Rejected;
    }

    if (!parentState.invocation || !child.callerMacroId ||
        *child.callerMacroId != parentState.invocation->id)
      return TerminalGeneratedCalleeEdgeStatus::Rejected;
    if (child.calleeOrigin.callerParamIndices.size() != 1)
      return TerminalGeneratedCalleeEdgeStatus::Rejected;

    const uint32_t callerFormalIndex =
        child.calleeOrigin.callerParamIndices.front();
    if (callerFormalIndex >= parentState.rootFormalByLocalFormal.size())
      return TerminalGeneratedCalleeEdgeStatus::Rejected;

    RootFormalBinding rootFormal =
        parentState.rootFormalByLocalFormal[callerFormalIndex];
    if (!rootFormal)
      return TerminalGeneratedCalleeEdgeStatus::Rejected;

    const RefoldModel::MacroDirective *terminalDefinition =
        graph_.DefinitionFor(child);
    if (!terminalDefinition || !terminalDefinition->functionLike ||
        macroDefinitionHasVariadicFormal(*terminalDefinition))
      return TerminalGeneratedCalleeEdgeStatus::Rejected;

    edge.parentInvocation = parentState.invocation;
    edge.terminalInvocation = &child;
    edge.terminalDefinition = terminalDefinition;
    edge.callerFormalIndex = callerFormalIndex;
    edge.rootCalleeFormalIndex = *rootFormal;
    return TerminalGeneratedCalleeEdgeStatus::Accepted;
  }

  const MacroForwardingGraph &graph_;
};

/// Composes the unique recursive tuple-generated-callee path for one root.
///
/// The composer enumerates every complete admissible theorem path instead of
/// accepting the first successful traversal.  This preserves the uniqueness
/// obligation required by the public proof identity: zero complete paths or more
/// than one complete path both fail closed and leave existing fallback behavior
/// untouched.
class MacroForwardingPathComposer {
public:
  MacroForwardingPathComposer(const MacroForwardingGraph &graph,
                              const clang::LangOptions &lexLang)
      : graph_(graph), forwardingComposer_(graph),
        terminalRecognizer_(graph), tupleSliceDeriver_(lexLang) {}

  /// Compose exactly one complete path from \p request's root invocation.
  std::optional<ComposedGeneratedCalleePath> ComposeUniquePath(
      const RecursiveTupleGeneratedReplayRequest &request) const {
    const RefoldModel::MacroInvocation *rootInvocation =
        graph_.FindInvocation(request.rootInvocation.id);
    if (!rootInvocation || rootInvocation != &request.rootInvocation)
      return std::nullopt;

    const RefoldModel::MacroDirective *rootDefinition =
        graph_.DefinitionFor(*rootInvocation);
    if (!rootDefinition || rootDefinition->id != request.rootDefinition.id)
      return std::nullopt;
    if (!graph_.HasAcyclicBoundedDescendants(*rootInvocation))
      return std::nullopt;

    llvm::SmallVector<ComposedFormalState, 8> forwardingStates;
    if (!forwardingComposer_.CollectLiteralForwardingStates(
            *rootInvocation, request.rootDefinition, forwardingStates))
      return std::nullopt;

    llvm::SmallVector<TerminalGeneratedCalleeEdge, 4> terminalEdges;
    if (!terminalRecognizer_.CollectTerminalGeneratedCalleeEdges(
            forwardingStates, terminalEdges))
      return std::nullopt;

    llvm::SmallVector<ComposedGeneratedCalleePath, 2> paths;
    for (const TerminalGeneratedCalleeEdge &edge : terminalEdges) {
      std::optional<ComposedGeneratedCalleePath> path =
          TryCompletePath(request, *rootInvocation, edge);
      if (!path)
        continue;
      paths.push_back(*path);
      if (paths.size() > 1)
        return std::nullopt;
    }

    if (paths.size() != 1)
      return std::nullopt;
    return paths.front();
  }

private:
  std::optional<ComposedGeneratedCalleePath> TryCompletePath(
      const RecursiveTupleGeneratedReplayRequest &request,
      const RefoldModel::MacroInvocation &rootInvocation,
      const TerminalGeneratedCalleeEdge &edge) const {
    std::optional<RootTupleSliceDerivation> derivation =
        tupleSliceDeriver_.DeriveUniqueRootTupleSlices(request, edge);
    if (!derivation)
      return std::nullopt;

    std::optional<GeneratedTupleFormalState> generatedState =
        MakeGeneratedTupleFormalState(edge, *derivation);
    if (!generatedState)
      return std::nullopt;

    llvm::SmallVector<ComposedGeneratedCalleePath, 2> nestedPaths;
    if (!CollectNestedGeneratedCalleePaths(request, rootInvocation,
                                          *generatedState, 0, nestedPaths))
      return std::nullopt;
    if (nestedPaths.size() > 1)
      return std::nullopt;
    if (nestedPaths.size() == 1)
      return nestedPaths.front();

    return BuildPathForGeneratedState(rootInvocation, *generatedState);
  }

  /// Initialize per-formal tuple slices for the first generated callee.
  ///
  /// `RootTupleSliceDeriver` has already proven that every actual of the first
  /// generated callee is exactly one top-level element of the unique root tuple
  /// formal.  This method reshapes that proof into a formal-indexed state so a
  /// generated callee body such as `f t` can be followed when `f` itself names a
  /// generated function-like macro and `t` supplies that macro's tuple actuals.
  std::optional<GeneratedTupleFormalState> MakeGeneratedTupleFormalState(
      const TerminalGeneratedCalleeEdge &edge,
      const RootTupleSliceDerivation &derivation) const {
    if (!edge.terminalInvocation || !edge.terminalDefinition ||
        !edge.terminalDefinition->functionLike)
      return std::nullopt;
    if (edge.terminalDefinition->defParams.empty() ||
        edge.terminalDefinition->defParams.size() >
            std::numeric_limits<uint32_t>::max())
      return std::nullopt;

    const uint32_t formalCount =
        static_cast<uint32_t>(edge.terminalDefinition->defParams.size());
    GeneratedTupleFormalState state;
    state.invocation = edge.terminalInvocation;
    state.definition = edge.terminalDefinition;
    state.rootCalleeFormalIndex = edge.rootCalleeFormalIndex;
    state.rootTupleFormalIndex = derivation.rootTupleFormalIndex;
    state.rootTupleSliceByLocalFormal.assign(formalCount, std::nullopt);
    state.actualSlices.assign(derivation.actualSlices.begin(),
                              derivation.actualSlices.end());

    for (const RootTupleSliceBinding &slice : derivation.actualSlices) {
      if (slice.generatedFormalIndex >= formalCount ||
          slice.rootTupleFormalIndex != derivation.rootTupleFormalIndex)
        return std::nullopt;
      if (state.rootTupleSliceByLocalFormal[slice.generatedFormalIndex])
        return std::nullopt;
      state.rootTupleSliceByLocalFormal[slice.generatedFormalIndex] = slice;
    }

    for (const std::optional<RootTupleSliceBinding> &slice :
         state.rootTupleSliceByLocalFormal) {
      if (!slice)
        return std::nullopt;
    }
    return state;
  }

  /// Follow generated-callee descendants that are selected by tuple elements.
  ///
  /// The first generated-callee edge may be only an intermediate higher-order
  /// forwarder.  In `ADD(f, t) -> f t`, the `ADD` invocation's first actual
  /// selects the next generated callee and its second actual supplies the tuple
  /// consumed by that callee.  This traversal follows only producer-recorded
  /// `caller_param` children and requires each descendant actual to carry exact
  /// `arg_tuple_refs` back to a currently proven generated formal slice.
  bool CollectNestedGeneratedCalleePaths(
      const RecursiveTupleGeneratedReplayRequest &request,
      const RefoldModel::MacroInvocation &rootInvocation,
      const GeneratedTupleFormalState &state, unsigned depth,
      llvm::SmallVectorImpl<ComposedGeneratedCalleePath> &paths) const {
    if (!state.invocation || !state.definition ||
        depth >= RecursiveTupleForwardingDepthLimit)
      return false;

    bool sawGeneratedCalleeChild = false;
    for (const RefoldModel::MacroInvocation *child :
         graph_.ChildrenOf(state.invocation->id)) {
      if (!child)
        return false;

      switch (child->calleeOrigin.kind) {
      case MacroCalleeOriginKind::LiteralMacroName:
        continue;
      case MacroCalleeOriginKind::Paste:
      case MacroCalleeOriginKind::Opaque:
        return false;
      case MacroCalleeOriginKind::CallerParam:
        break;
      }

      sawGeneratedCalleeChild = true;
      std::optional<GeneratedTupleFormalState> childState =
          TryComposeNestedGeneratedCalleeState(request, state, *child);
      if (!childState)
        return false;

      llvm::SmallVector<ComposedGeneratedCalleePath, 2> descendantPaths;
      if (!CollectNestedGeneratedCalleePaths(request, rootInvocation,
                                            *childState, depth + 1,
                                            descendantPaths))
        return false;
      if (descendantPaths.empty()) {
        std::optional<ComposedGeneratedCalleePath> leafPath =
            BuildPathForGeneratedState(rootInvocation, *childState);
        if (!leafPath)
          return false;
        paths.push_back(*leafPath);
      } else {
        paths.append(descendantPaths.begin(), descendantPaths.end());
      }

      if (paths.size() > 1)
        return true;
    }

    (void)sawGeneratedCalleeChild;
    return true;
  }

  /// Compose one generated-callee child through exact tuple-element refs.
  std::optional<GeneratedTupleFormalState> TryComposeNestedGeneratedCalleeState(
      const RecursiveTupleGeneratedReplayRequest &request,
      const GeneratedTupleFormalState &parentState,
      const RefoldModel::MacroInvocation &child) const {
    if (!parentState.invocation || !child.callerMacroId ||
        *child.callerMacroId != parentState.invocation->id)
      return std::nullopt;
    if (child.calleeOrigin.kind != MacroCalleeOriginKind::CallerParam ||
        child.calleeOrigin.callerParamIndices.size() != 1)
      return std::nullopt;

    const uint32_t calleeFormalIndex =
        child.calleeOrigin.callerParamIndices.front();
    if (calleeFormalIndex >= parentState.rootTupleSliceByLocalFormal.size() ||
        !parentState.rootTupleSliceByLocalFormal[calleeFormalIndex])
      return std::nullopt;

    const RefoldModel::MacroDirective *childDefinition =
        graph_.DefinitionFor(child);
    if (!childDefinition || !childDefinition->functionLike ||
        macroDefinitionHasVariadicFormal(*childDefinition))
      return std::nullopt;
    if (childDefinition->defParams.empty() ||
        childDefinition->defParams.size() >
            std::numeric_limits<uint32_t>::max())
      return std::nullopt;

    const uint32_t formalCount =
        static_cast<uint32_t>(childDefinition->defParams.size());
    if (child.invArgRanges.size() != formalCount ||
        child.argTupleRefs.size() != formalCount)
      return std::nullopt;

    GeneratedTupleFormalState childState;
    childState.invocation = &child;
    childState.definition = childDefinition;
    childState.rootCalleeFormalIndex = parentState.rootCalleeFormalIndex;
    childState.rootTupleFormalIndex = parentState.rootTupleFormalIndex;
    childState.rootTupleSliceByLocalFormal.assign(formalCount, std::nullopt);
    childState.actualSlices.reserve(formalCount);

    for (uint32_t formalIndex = 0; formalIndex < formalCount; ++formalIndex) {
      const auto &refs = child.argTupleRefs[formalIndex];
      if (refs.size() != 1)
        return std::nullopt;

      const RefoldModel::TupleArgRef &ref = refs.front();
      if (ref.callerParamIndex >=
              parentState.rootTupleSliceByLocalFormal.size() ||
          !parentState.rootTupleSliceByLocalFormal[ref.callerParamIndex])
        return std::nullopt;
      if (ref.callerByteEnd <= ref.callerByteBegin)
        return std::nullopt;

      const RootTupleSliceBinding &parentSlice =
          *parentState.rootTupleSliceByLocalFormal[ref.callerParamIndex];
      if (parentSlice.rootTupleFormalIndex != parentState.rootTupleFormalIndex)
        return std::nullopt;

      const uint64_t parentByteWidth =
          parentSlice.absoluteByteEnd - parentSlice.absoluteByteBegin;
      if (ref.callerByteEnd > parentByteWidth)
        return std::nullopt;

      RootTupleSliceBinding binding;
      binding.generatedActualIndex = formalIndex;
      binding.generatedFormalIndex = formalIndex;
      binding.rootTupleFormalIndex = parentState.rootTupleFormalIndex;
      binding.rootTuplePayloadByteBegin =
          parentSlice.rootTuplePayloadByteBegin + ref.callerByteBegin;
      binding.rootTuplePayloadByteEnd =
          parentSlice.rootTuplePayloadByteBegin + ref.callerByteEnd;
      binding.absoluteByteBegin =
          parentSlice.absoluteByteBegin + ref.callerByteBegin;
      binding.absoluteByteEnd = parentSlice.absoluteByteBegin + ref.callerByteEnd;
      if (binding.rootTuplePayloadByteEnd <=
              binding.rootTuplePayloadByteBegin ||
          binding.absoluteByteEnd <= binding.absoluteByteBegin ||
          binding.rootTuplePayloadByteEnd > parentSlice.rootTuplePayloadByteEnd)
        return std::nullopt;

      std::optional<PhysicalInvocationActualRange> actualRange =
          physicalInvocationActualRange(child, formalIndex);
      if (!actualRange || actualRange->begin != binding.absoluteByteBegin ||
          actualRange->end != binding.absoluteByteEnd ||
          !request.rootInvocation.invFile ||
          actualRange->file != *request.rootInvocation.invFile)
        return std::nullopt;

      for (const RootTupleSliceBinding &existing : childState.actualSlices) {
        if (rangesOverlap(existing.rootTuplePayloadByteBegin,
                          existing.rootTuplePayloadByteEnd,
                          binding.rootTuplePayloadByteBegin,
                          binding.rootTuplePayloadByteEnd))
          return std::nullopt;
      }

      childState.rootTupleSliceByLocalFormal[formalIndex] = binding;
      childState.actualSlices.push_back(binding);
    }

    return childState;
  }

  /// Convert a generated formal state into the public composed path carrier.
  std::optional<ComposedGeneratedCalleePath> BuildPathForGeneratedState(
      const RefoldModel::MacroInvocation &rootInvocation,
      const GeneratedTupleFormalState &state) const {
    if (!state.invocation || !state.definition || state.actualSlices.empty())
      return std::nullopt;

    ComposedGeneratedCalleePath path;
    path.rootInvocation = &rootInvocation;
    path.terminalInvocation = state.invocation;
    path.terminalDefinition = state.definition;
    path.rootCalleeFormalIndex = state.rootCalleeFormalIndex;
    path.rootTupleFormalIndex = state.rootTupleFormalIndex;
    path.actualSlices.assign(state.actualSlices.begin(),
                             state.actualSlices.end());
    return path;
  }

  const MacroForwardingGraph &graph_;
  WholeFormalForwardingComposer forwardingComposer_;
  TerminalGeneratedCalleeEdgeRecognizer terminalRecognizer_;
  RootTupleSliceDeriver tupleSliceDeriver_;
};

/// Slice an absolute physical range from the root invocation spelling.
///
/// The range must already have been proven to be inside the root tuple actual.
/// This helper only rebases that producer source range into `invText` so the
/// terminal replay solver receives the old actual spelling in callee-formal
/// order.  No source-overlap ancestry inference is performed here.
std::optional<std::string> rootInvocationSourceSlice(
    const RefoldModel::MacroInvocation &rootInvocation, uint64_t absoluteBegin,
    uint64_t absoluteEnd) {
  if (!rootInvocation.invText || !rootInvocation.invB || !rootInvocation.invE)
    return std::nullopt;
  if (absoluteEnd <= absoluteBegin || absoluteBegin < *rootInvocation.invB ||
      absoluteEnd > *rootInvocation.invE)
    return std::nullopt;

  const uint64_t relativeBegin = absoluteBegin - *rootInvocation.invB;
  const uint64_t relativeEnd = absoluteEnd - *rootInvocation.invB;
  if (relativeEnd < relativeBegin ||
      relativeEnd > rootInvocation.invText->size())
    return std::nullopt;

  llvm::StringRef text = rootInvocation.invText->slice(
      static_cast<size_t>(relativeBegin), static_cast<size_t>(relativeEnd));
  if (text.trim().empty())
    return std::nullopt;
  return text.str();
}

/// Solves the terminal generated-callee replay for a composed path.
///
/// This adapter is intentionally thin: it recovers the old terminal actual
/// spellings from already-proven root tuple slices, then delegates replacement-
/// list parsing and unique old/new expansion solving to
/// `RefoldMacroGeneratedCalleeReplayEngine`.  It does not build a `MacroPatch`
/// and does not edit the root tuple payload; those are later theorem steps.
class RecursiveTupleGeneratedReplaySolver {
public:
  explicit RecursiveTupleGeneratedReplaySolver(
      const RefoldMacroGeneratedCalleeReplayEngine &generatedCalleeReplayEngine,
      const RefoldSourceMapper &sourceMapper)
      : generatedCalleeReplayEngine_(generatedCalleeReplayEngine),
        sourceMapper_(sourceMapper) {}

  /// Return the unique terminal replay solution for \p path, if provable.
  std::optional<RecursiveTupleGeneratedReplaySolution> Solve(
      const RecursiveTupleGeneratedReplayRequest &request,
      const ComposedGeneratedCalleePath &path) const {
    if (!path.rootInvocation || !path.terminalDefinition ||
        path.actualSlices.empty())
      return std::nullopt;
    if (path.terminalDefinition->defParams.empty() ||
        path.terminalDefinition->defParams.size() >
            std::numeric_limits<uint32_t>::max())
      return std::nullopt;

    const uint32_t formalCount =
        static_cast<uint32_t>(path.terminalDefinition->defParams.size());
    llvm::SmallVector<std::optional<std::string>, 8> oldActualByFormal;
    oldActualByFormal.resize(formalCount);

    for (const RootTupleSliceBinding &slice : path.actualSlices) {
      if (slice.generatedFormalIndex >= formalCount ||
          slice.generatedActualIndex >= formalCount)
        return std::nullopt;
      if (oldActualByFormal[slice.generatedFormalIndex])
        return std::nullopt;

      std::optional<std::string> oldActual = rootInvocationSourceSlice(
          *path.rootInvocation, slice.absoluteByteBegin, slice.absoluteByteEnd);
      if (!oldActual)
        return std::nullopt;
      oldActualByFormal[slice.generatedFormalIndex] = std::move(*oldActual);
    }

    RecursiveTupleGeneratedReplaySolution solution;
    solution.oldActuals.reserve(formalCount);
    for (uint32_t formalIndex = 0; formalIndex < formalCount; ++formalIndex) {
      if (!oldActualByFormal[formalIndex])
        return std::nullopt;
      solution.oldActuals.push_back(std::move(*oldActualByFormal[formalIndex]));
    }

    std::optional<std::pair<uint64_t, uint64_t>> replayATokens =
        terminalReplayATokenRange(request, path);
    if (!replayATokens)
      return std::nullopt;

    std::optional<std::pair<size_t, size_t>> replayBEnvelope =
        mapReplayBTokenEnvelope(*replayATokens);
    if (!replayBEnvelope)
      return std::nullopt;

    TerminalGeneratedCalleeReplayRequest terminalRequest{
        *path.terminalDefinition,
        llvm::ArrayRef<std::string>(solution.oldActuals.data(),
                                    solution.oldActuals.size()),
        *replayATokens, *replayBEnvelope};
    std::optional<TerminalGeneratedCalleeReplaySolution> terminalSolution =
        generatedCalleeReplayEngine_.SolveTerminalGeneratedCalleeReplay(
            terminalRequest);
    if (!terminalSolution ||
        terminalSolution->oldSolvedActuals.size() !=
            solution.oldActuals.size() ||
        terminalSolution->newSolvedActuals.size() != solution.oldActuals.size())
      return std::nullopt;

    solution.oldSolvedActuals.assign(terminalSolution->oldSolvedActuals.begin(),
                                     terminalSolution->oldSolvedActuals.end());
    solution.newSolvedActuals.assign(terminalSolution->newSolvedActuals.begin(),
                                     terminalSolution->newSolvedActuals.end());
    solution.usesStringification = terminalSolution->usesStringification;
    solution.usesPaste = terminalSolution->usesPaste;
    return solution;
  }

private:
  /// Return the exact A-token replay surface for the terminal generated callee.
  ///
  /// The recursive proof edits the source-spelled root tuple, but terminal
  /// generated-callee inversion must be solved against the terminal callee's
  /// own expansion surface.  In nested cases such as `ADD(f, t) -> ((f t)+10)`,
  /// the root whole-cover surface contains literal wrapper tokens from `ADD`
  /// that are not part of the terminal `SUB` replacement list.  Solving `SUB`
  /// against the enclosing root cover would fail correctly and then allow a
  /// lossy whole-cover fallback.  The producer-recorded terminal cover is the
  /// deterministic replay surface for the terminal definition.
  std::optional<std::pair<uint64_t, uint64_t>> terminalReplayATokenRange(
      const RecursiveTupleGeneratedReplayRequest &request,
      const ComposedGeneratedCalleePath &path) const {
    if (!path.terminalInvocation || !path.terminalInvocation->cover.IsValid())
      return std::nullopt;

    const uint64_t begin = path.terminalInvocation->cover.begin;
    const uint64_t end = path.terminalInvocation->cover.end;
    if (begin >= end || begin < request.wholeCoverATokens.first ||
        end > request.wholeCoverATokens.second)
      return std::nullopt;
    return std::make_pair(begin, end);
  }

  /// Map the terminal A-token replay surface to the edited B-token surface.
  ///
  /// This mirrors the normal whole-cover mapping policy, including the
  /// boundary-insertion preserving fallback, but applies it to the terminal
  /// generated callee's own expansion surface rather than the enclosing root
  /// invocation cover.
  std::optional<std::pair<size_t, size_t>> mapReplayBTokenEnvelope(
      const std::pair<uint64_t, uint64_t> &replayATokens) const {
    std::optional<std::pair<size_t, size_t>> bEnvelope =
        sourceMapper_.MapATokRangeAToBTokenEnvelope(
            replayATokens.first, replayATokens.second);
    if (!bEnvelope || bEnvelope->first >= bEnvelope->second)
      bEnvelope = sourceMapper_
                      .MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
                          replayATokens.first, replayATokens.second);
    if (!bEnvelope || bEnvelope->first >= bEnvelope->second)
      return std::nullopt;
    return bEnvelope;
  }

  const RefoldMacroGeneratedCalleeReplayEngine &generatedCalleeReplayEngine_;
  const RefoldSourceMapper &sourceMapper_;
};

/// Applies solved terminal generated-callee replacements to the root tuple.
///
/// This applier owns only source-spelling reconstruction.  The path composer has
/// already proven that each slice is an exact tuple element, and the replay
/// solver has already proven a unique edited value for each terminal formal.
/// The applier preserves every byte outside changed tuple element slices, edits
/// right-to-left, and rejects unchanged, overlapping, or delimiter-crossing
/// edits instead of normalizing the tuple.
class RootTupleEditApplier {
public:
  /// Rebuild the root invocation by editing the unique root tuple argument.
  std::optional<RecursiveTupleInvocationRewrite> Apply(
      const RecursiveTupleGeneratedReplayRequest &request,
      const ComposedGeneratedCalleePath &path,
      const RecursiveTupleGeneratedReplaySolution &solution) const {
    if (!path.rootInvocation || path.rootInvocation != &request.rootInvocation)
      return std::nullopt;
    if (path.rootTupleFormalIndex >= request.invocationArgRanges.size())
      return std::nullopt;
    if (solution.newSolvedActuals.empty() ||
        solution.oldSolvedActuals.size() != solution.newSolvedActuals.size())
      return std::nullopt;

    const auto tupleArgRange =
        request.invocationArgRanges[path.rootTupleFormalIndex];
    if (tupleArgRange.second < tupleArgRange.first ||
        tupleArgRange.second > request.baseInvocationText.size())
      return std::nullopt;

    llvm::StringRef tupleActual = request.baseInvocationText.slice(
        tupleArgRange.first, tupleArgRange.second);
    size_t trimmedBegin = 0;
    size_t trimmedEnd = tupleActual.size();
    std::tie(trimmedBegin, trimmedEnd) =
        stringutils::trimWsRange(tupleActual, 0, tupleActual.size());
    if (trimmedEnd <= trimmedBegin + 2)
      return std::nullopt;

    llvm::StringRef trimmedTuple = tupleActual.slice(trimmedBegin, trimmedEnd);
    if (!trimmedTuple.starts_with("(") || !trimmedTuple.ends_with(")"))
      return std::nullopt;

    const size_t payloadBegin = trimmedBegin + 1;
    const size_t payloadEnd = trimmedEnd - 1;
    llvm::SmallVector<RootTupleElementEdit, 8> edits;
    edits.reserve(path.actualSlices.size());

    for (const RootTupleSliceBinding &slice : path.actualSlices) {
      if (slice.rootTupleFormalIndex != path.rootTupleFormalIndex ||
          slice.generatedFormalIndex >= solution.newSolvedActuals.size() ||
          slice.generatedFormalIndex >= solution.oldSolvedActuals.size())
        return std::nullopt;
      if (slice.rootTuplePayloadByteEnd <=
              slice.rootTuplePayloadByteBegin ||
          slice.rootTuplePayloadByteEnd > payloadEnd - payloadBegin ||
          slice.rootTuplePayloadByteEnd >
              std::numeric_limits<size_t>::max())
        return std::nullopt;

      const size_t editBegin =
          payloadBegin + static_cast<size_t>(slice.rootTuplePayloadByteBegin);
      const size_t editEnd =
          payloadBegin + static_cast<size_t>(slice.rootTuplePayloadByteEnd);
      if (editEnd <= editBegin || editBegin < payloadBegin ||
          editEnd > payloadEnd)
        return std::nullopt;

      llvm::StringRef currentText = tupleActual.slice(editBegin, editEnd);
      llvm::StringRef oldText =
          llvm::StringRef(solution.oldSolvedActuals[slice.generatedFormalIndex])
              .trim();
      llvm::StringRef newText =
          llvm::StringRef(solution.newSolvedActuals[slice.generatedFormalIndex])
              .trim();
      if (oldText.empty() || newText.empty())
        return std::nullopt;

      // The source slice was already byte-matched to the terminal actual.  For
      // this initial theorem, require the generated-callee old solution to name
      // the same trimmed source spelling before replacing the whole slice.
      if (currentText.trim() != oldText)
        return std::nullopt;
      if (currentText == newText)
        continue;

      RootTupleElementEdit edit;
      edit.begin = editBegin;
      edit.end = editEnd;
      edit.replacement = newText.str();
      edits.push_back(std::move(edit));
    }

    if (edits.empty())
      return std::nullopt;

    llvm::sort(edits, [](const RootTupleElementEdit &lhs,
                         const RootTupleElementEdit &rhs) {
      if (lhs.begin != rhs.begin)
        return lhs.begin < rhs.begin;
      return lhs.end < rhs.end;
    });

    size_t previousEnd = 0;
    bool havePrevious = false;
    for (const RootTupleElementEdit &edit : edits) {
      if (edit.end <= edit.begin || edit.end > tupleActual.size())
        return std::nullopt;
      if (havePrevious && edit.begin < previousEnd)
        return std::nullopt;
      previousEnd = edit.end;
      havePrevious = true;
    }

    std::string rewrittenTupleActual = tupleActual.str();
    for (const RootTupleElementEdit &edit : llvm::reverse(edits)) {
      rewrittenTupleActual = stringutils::replaceRange(
          rewrittenTupleActual, edit.begin, edit.end, edit.replacement);
    }

    std::string rewrittenInvocation = stringutils::replaceRange(
        request.baseInvocationText.str(), tupleArgRange.first,
        tupleArgRange.second, rewrittenTupleActual);
    if (llvm::StringRef(rewrittenInvocation) == request.baseInvocationText)
      return std::nullopt;

    RecursiveTupleInvocationRewrite rewrite;
    rewrite.rewrittenInvocation = std::move(rewrittenInvocation);
    rewrite.rewrittenTupleActual = std::move(rewrittenTupleActual);
    return rewrite;
  }
};

/// Final local validation before the recursive resolver exposes a candidate.
///
/// The global final-candidate selector still owns cross-candidate replay
/// stability and macro-state observation checks.  This local gate verifies the
/// recursive theorem's own final envelope: the patch is still a root-preserving
/// callsite rewrite, its A/B materialized ranges are non-empty and bounded, and
/// the emitted replacement remains tied to the exact root invocation span.
bool recursiveTupleGeneratedReplayPatchHasFinalLocalEnvelope(
    const RecursiveTupleGeneratedReplayRequest &request,
    const MacroPatch &patch) {
  if (!request.rootInvocation.invB || !request.rootInvocation.invE)
    return false;
  if (patch.invRange.begin != *request.rootInvocation.invB ||
      patch.invRange.end != *request.rootInvocation.invE ||
      patch.invRange.begin >= patch.invRange.end)
    return false;
  if (patch.macroId != request.rootInvocation.id || patch.replacement.empty() ||
      llvm::StringRef(patch.replacement) == request.baseInvocationText)
    return false;
  if (!RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(
          patch.replacement, request.rootInvocation))
    return false;
  if (!patch.materialized.hasBTokenRange ||
      patch.materialized.bTokStart >= patch.materialized.bTokEnd)
    return false;
  if (patch.materialized.bTokStart != request.bTokenEnvelope.first ||
      patch.materialized.bTokEnd != request.bTokenEnvelope.second)
    return false;
  if (!patch.materialized.hasOutputByteRange ||
      patch.materialized.outputByteStart >= patch.materialized.outputByteEnd ||
      patch.materialized.outputByteEnd > patch.replacement.size())
    return false;
  return true;
}

} // namespace

RefoldMacroRecursiveTupleGeneratedReplay::
    RefoldMacroRecursiveTupleGeneratedReplay(Dependencies deps)
    : deps_(deps) {}

std::optional<MacroPatch>
RefoldMacroRecursiveTupleGeneratedReplay::BuildCandidate(
    const RecursiveTupleGeneratedReplayRequest &request) const {
  if (!recursiveTupleReplayRequestHasUsableRootEvidence(request))
    return std::nullopt;

  MacroForwardingGraph graph(deps_.model);
  MacroForwardingPathComposer pathComposer(graph, deps_.lexLang);

  // This theorem layer composes exactly one complete producer-recorded path
  // from the root invocation to a terminal generated callee, proves that the
  // terminal actuals are exact slices of one root tuple argument, delegates
  // old/new expansion solving to the existing generated-callee replay engine,
  // rebuilds the root invocation by editing only changed tuple elements, and
  // finally attaches the distinct recursive tuple proof carrier.
  std::optional<ComposedGeneratedCalleePath> path =
      pathComposer.ComposeUniquePath(request);
  if (!path)
    return std::nullopt;

  RecursiveTupleGeneratedReplaySolver replaySolver(
      deps_.generatedCalleeReplayEngine, deps_.sourceMapper);
  std::optional<RecursiveTupleGeneratedReplaySolution> replaySolution =
      replaySolver.Solve(request, *path);
  if (!replaySolution)
    return std::nullopt;

  RootTupleEditApplier tupleEditApplier;
  std::optional<RecursiveTupleInvocationRewrite> invocationRewrite =
      tupleEditApplier.Apply(request, *path, *replaySolution);
  if (!invocationRewrite)
    return std::nullopt;

  if (!request.rootInvocation.invB || !request.rootInvocation.invE)
    return std::nullopt;

  MacroPatch patch{*request.rootInvocation.invB, *request.rootInvocation.invE,
                   std::move(invocationRewrite->rewrittenInvocation),
                   request.rootInvocation.id};

  // Recursive tuple replay materializes the edited terminal expansion through a
  // preserved root callsite.  The whole patch replacement is therefore the
  // materialized output witness, matching the direct tuple-generated-callee
  // replay certification convention.
  patch.materialized.hasOutputByteRange = true;
  patch.materialized.outputByteStart = 0;
  patch.materialized.outputByteEnd = patch.replacement.size();
  certifyMacroPatchMaterializedBTokenRange(
      patch, static_cast<uint64_t>(request.bTokenEnvelope.first),
      static_cast<uint64_t>(request.bTokenEnvelope.second));

  RecursiveTupleGeneratedCalleeReplayWitness witness;
  witness.rootInvocationId = request.rootInvocation.id;
  witness.terminalGeneratedInvocationId = path->terminalInvocation->id;
  witness.terminalCalleeDefinitionDirectiveId = path->terminalDefinition->id;
  witness.rootCalleeFormalIndex = path->rootCalleeFormalIndex;
  witness.rootTupleFormalIndex = path->rootTupleFormalIndex;
  witness.uniquePath = true;
  witness.uniqueTupleFormal = true;
  witness.uniqueReplaySolution = true;
  witness.actualSlices.reserve(path->actualSlices.size());
  for (const RootTupleSliceBinding &slice : path->actualSlices) {
    GeneratedActualRootTupleSlice durableSlice;
    durableSlice.generatedActualIndex = slice.generatedActualIndex;
    durableSlice.generatedFormalIndex = slice.generatedFormalIndex;
    durableSlice.rootTupleFormalIndex = slice.rootTupleFormalIndex;
    durableSlice.rootTuplePayloadByteBegin = slice.rootTuplePayloadByteBegin;
    durableSlice.rootTuplePayloadByteEnd = slice.rootTuplePayloadByteEnd;
    witness.actualSlices.push_back(durableSlice);
  }

  if (!recursiveTupleGeneratedReplayPatchHasFinalLocalEnvelope(request, patch))
    return std::nullopt;

  deps_.proofCertifier.SetRecursiveTupleGeneratedCalleeReplayProof(
      patch, request.rootInvocation, std::move(witness));
  (void)deps_.sourceMapper;
  (void)deps_.macroTopology;
  return patch;
}

} // namespace refold
} // namespace clang
