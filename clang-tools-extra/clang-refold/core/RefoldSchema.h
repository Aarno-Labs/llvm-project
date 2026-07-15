//===--- RefoldSchema.h ----------------------------------------*- C++ -*-===//
//
// JSON schema for the producer-emitted clang-refold map.
//
// This header embeds the schema string used to validate the on-disk “refold
// map” produced by the modified Clang preprocessor and consumed by the
// clang-refold tool. The map records how the unmodified preprocessed
// token stream A was derived from source constructs so that edits made to a
// later, edited preprocessed stream B can be deterministically projected back
// into partially expanded C/C++ source.
//
// High-level model
// ----------------
//   * A: the original, unedited preprocessed token stream (indices 0..N-1).
//   * B: a later, edited preprocessed token stream (not stored in the map).
//   * The schema ties A’s tokens back to source files/bytes and PP constructs:
//       - macro invocations and their expansion spans
//       - include directives and the spans they expanded into
//       - explicit origin partitioning within macro expansions:
//         * arg_spans       : expansion slices sourced from actual arguments
//         * stringify_spans : slices sourced from argument stringification (#X)
//         * paste_spans     : slices sourced from argument token-paste (X##Y)
//         * body_spans      : slices sourced from macro body (non-arguments)
//       - macro definition/undef directives and pragmas
//       - conditional groups (#if/#elif/#else/#endif) with selected arms
//       - token→file byte mapping for A (absolute/canonical paths)
//       - explicit “slots” (stable insertion anchors in original source)
//
// Top-level object
// ----------------
// {
//   "version": "<opaque format version string>",
//   "pp_ctx": {
//     "cwd":  "<working directory for the preprocessing invocation>",
//     "argv": ["<clang arg token>", ...],   // replayed for deterministic
//     --check "lang": "<high-level language>"       // e.g. "c", "c++", "objc"
//   },
//   "source": "<path of the TU we refold back into>",
//   "tokens": {
//     "count": <N>,
//     "pp_byte_begin": [ ... ],             // optional; per-token A byte begin
//     "pp_byte_end":   [ ... ]              // optional; per-token A byte end
//   },
//   "tokmap": [ TokMapEntry, ... ],         // A-token -> (file,[b,e)) mapping
//   "slots":  [ Slot, ... ],                // explicit insertion anchors
//   "conds":  [ Cond, ... ],                // conditional groups
//   "line_controls": [ LineControlEvent, ... ] // active #line events
//   "items":  [ Item, ... ]                 //
//   macros/includes/defs/pragmas/files
// }
//
// Core definitions (selected)
// ---------------------------
// * PPSpan
//     Half-open A-token interval [begin, end). Every construct that contributes
//     preprocessed tokens reports one or more spans.
//
// * PPArgSpan
//     Like PPSpan, but additionally records:
//       - arg_index: the 0-based argument index this span originates from
//       - optional byte envelopes for that occurrence:
//         * byte_begin/byte_end      : bytes in the original source (when
//         known)
//         * pp_byte_begin/pp_byte_end: bytes in the A preprocessed output
//
// * MacroItem
//     A macro expansion in A, with:
//       - name, subkind (func/obj), spans[]
//       - optional origin partitioning within the expansion:
//         arg_spans / stringify_spans / paste_spans / body_spans
//       - invocation call-site bytes: inv_file, [inv_b, inv_e) (BYTES in file)
//       - optional invocation envelope in A output bytes:
//         inv_pp_byte_begin / inv_pp_byte_end
//       - owner_include_id: include instance that opened inv_file (when nested)
//       - optional immediate-caller and argument-flow provenance:
//         caller_macro_id / callee_origin / arg_refs / arg_tuple_refs
//       - optional normalized invocation spelling and argument ranges for
//         generated function-like calls without an ordinary NAME(...) site
//
// * DirectiveIncludeItem
//     A single #include/#include_next instance, with:
//       - site_path and directive byte range [site_b, site_e) in that file
//         (site_b/site_e are present but may be null if unavailable)
//       - target (as written), optional resolved_path, angled, optional parent
//       - spans[]: A-token spans produced by this include instance
//       - optional decls[]: logical header-level declarations (requires
//         resolved_path)
//
// * DirectiveMacroItem
//     A #define/#undef directive line (definition), with:
//       - name: the producer-recorded macro-state key for the directive
//       - site_path and directive byte range [site_b, site_e) (nullable bounds)
//       - spans[]: A-token spans this directive contributed to (may be empty)
//       - optional owner_include_id to disambiguate repeated header instances
//       - function_like for #define shape, plus optional #define replay proof
//         data: def_params and replacement_tokens
//
// * DirectivePragmaItem
//     A #pragma line with exact text and location (site_path, [site_b, site_e))
//     (nullable bounds when unavailable).  Newer producers may include
//     owner_include_id to bind repeated header instances deterministically.
//
// * FileItem
//     Represents tokens emitted while the main TU (top-level) was active;
//     includes a path label and spans[].
//
// * TokMapEntry
//     A-token → source mapping entry:
//       - pp: token index in A
//       - file: absolute/canonical path
//       - [b,e): byte range in that file corresponding to token pp
//     Consumers should key by pp; producers typically emit one entry per A
//     token.
//
// * Slot
//     Explicit insertion anchors in original source bytes. Kinds include:
//     { file_begin, file_end, after_last_include, before_include,
//     after_include,
//       arm_begin, arm_end }.
//       - file, [b,e): byte range in file (point slots have b == e)
//       - ref: required for include/arm kinds; identifies include item id or
//         conditional arm id (per-kind)
//       - pp: optional stabilizer for ordering when multiple slots share b/e
//       - owner_include_id: include instance that opened file (when nested)
//
// * Cond / Arm
//     Conditional groups with absolute byte bounds [group_b, group_e) in a
//     file, plus ordered arms. Each Arm records:
//       - kind: if/ifdef/ifndef/elif/else
//       - cond: textual condition / macro name (required except for else)
//       - body byte range [body_b, body_e)
//       - selected: true iff this arm contributed tokens to A
//       - pp_span: present iff selected is true
//
// Invariants & conventions
// ------------------------
// * All token intervals are half-open A-token ranges [begin, end) with end >=
// begin.
// * All byte intervals are half-open file byte ranges [b, e) with e >= b.
// * Some location fields are nullable when the producer cannot compute bytes;
//   optional fields may be omitted entirely as allowed by the schema.
// * Item ids are unique within the file. Relationships (e.g. owner_include_id,
//   Slot.ref) use those ids to bind constructs deterministically.
// * "additionalProperties" is disabled across the schema to keep the format
//   closed and fully specified.
// * The schema uses JSON Schema draft 2020-12.
//
// Typical workflow
// ----------------
//   1) clang -E -P --refold-map=TU.refold.json TU.c -o TU.i
//      -> emits the refold map (this schema) plus unmodified A = TU.i
//   2) edit TU.i -> TU.i.mod                                       (B tokens)
//   3) clang-refold -p TU.i -P TU.i.mod -r TU.refold.json -o TU.c.mod
//      -> projects edits in B back into partially expanded source TU.c.mod
//
// Rationale
// ---------
// The map is intentionally sufficient for a deterministic refolder: it records
// A-token spans for PP constructs, byte locations for directive/invocation
// sites when available, and the conditional/include structure governing token
// production, plus explicit insertion anchors (slots) to avoid heuristic
// placement.
//
// Author:
//   jeikenberry
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSCHEMA_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSCHEMA_H

namespace clang {
namespace refold {

static constexpr const char *RefoldSchema = R"json(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "clang-refold map",
  "description": "Mapping from the unmodified preprocessed token stream A back to source constructs for refolding. Token stream B refers to the retransformed preprocessed tokens.",
  "type": "object",
  "required": [
    "version",
    "pp_ctx",
    "source",
    "tokens",
    "tokmap",
    "slots",
    "conds",
    "items"
  ],
  "additionalProperties": false,
  "properties": {
    "version": {
      "type": "string",
      "minLength": 1,
      "description": "File format version."
    },
    "pp_ctx": {
      "type": "object",
      "description": "Preprocessor invocation context used to produce stream a; used by clang-refold --check to re-run preprocessing and validate alignment against edited stream B.",
      "required": [
        "cwd",
        "argv",
        "lang"
      ],
      "additionalProperties": false,
      "properties": {
        "cwd": {
          "type": "string",
          "minLength": 1,
          "description": "Working directory where the original clang invocation was run. Relative paths in argv (e.g. -I ./headers) are interpreted relative to this directory."
        },
        "argv": {
          "type": "array",
          "description": "Argument vector tokens that materially affect preprocessing and must be replayed for deterministic checking (e.g. -D/-U/-I/-isystem/-include/--sysroot/-resource-dir/-triple/-target-cpu/-std/-x). Stored as tokens in original order.",
          "items": {
            "type": "string",
            "minLength": 1
          }
        },
        "lang": {
          "type": "string",
          "minLength": 1,
          "description": "High-level source language for the preprocessing invocation (e.g. 'c', 'c++', 'objc', 'objc++')."
        },
        "include_search_chain": {
          "type": "array",
          "description": "Producer-normalized effective include search chain in the exact order Clang used for header lookup. Entries are indexed by 'index'. This is optional for backward compatibility; when present, consumers should prefer it over reconstructing include lookup order from argv.",
          "items": {
            "$ref": "#/$defs/IncludeSearchEntry"
          }
        }
      }
    },
    "source": {
      "type": "string",
      "minLength": 1,
      "description": "Path to the main source file (the file we refold back into)."
    },
    "tokens": {
      "type": "object",
      "required": [
        "count"
      ],
      "additionalProperties": false,
      "properties": {
        "count": {
          "type": "integer",
          "minimum": 0,
          "description": "Number of tokens in the preprocessed stream A."
        },
        "pp_byte_begin": {
          "type": "array",
          "items": {
            "type": "integer",
            "minimum": 0
          },
          "description": "Per-token begin byte offsets in the preprocessed output (A stream). Index i corresponds to A token i."
        },
        "pp_byte_end": {
          "type": "array",
          "items": {
            "type": "integer",
            "minimum": 0
          },
          "description": "Per-token end byte offsets (exclusive) in the preprocessed output (A stream). Index i corresponds to A token i."
        }
      },
      "dependentRequired": {
        "pp_byte_begin": [
          "pp_byte_end"
        ],
        "pp_byte_end": [
          "pp_byte_begin"
        ]
      },
      "description": "Token metadata for the preprocessed stream A."
    },
    "tokmap": {
      "type": "array",
      "items": {
        "$ref": "#/$defs/TokMapEntry"
      },
      "description": "Per-token mapping from preprocessed tokens (A) to original source byte spans."
    },
    "slots": {
      "type": "array",
      "items": {
        "$ref": "#/$defs/Slot"
      },
      "description": "Explicit insertion anchors in the ORIGINAL source (bytes) that the refolder can attach new B-only code to without heuristics."
    },
    "conds": {
      "type": "array",
      "items": {
        "$ref": "#/$defs/Cond"
      },
      "description": "Conditional groups (#if/#elif/#else/#endif) discovered in source files; used to deterministically place edits into the correct arm."
    },
    "line_controls": {
      "type": "array",
      "items": {
        "$ref": "#/$defs/LineControlEvent"
      },
      "description": "Producer-proven active source #line / GNU line-marker events with post-expansion logical state. Optional for backward compatibility with older refold maps."
    },
    "items": {
      "type": "array",
      "items": {
        "$ref": "#/$defs/Item"
      },
      "description": "Preprocessor constructs (macros, directives, file segments) associated with tokens in stream A."
    }
  },
  "$defs": {
    "PPSpan": {
      "type": "object",
      "required": [
        "begin",
        "end"
      ],
      "additionalProperties": false,
      "properties": {
        "begin": {
          "type": "integer",
          "minimum": 0,
          "description": "Begin A-token index (inclusive)."
        },
        "end": {
          "type": "integer",
          "minimum": 0,
          "description": "End A-token index (exclusive)."
        }
      },
      "description": "Half-open token index range in the preprocessed token stream A: [begin, end). 'end' MUST be >= 'begin'."
    },
    "PPArgSpan": {
      "type": "object",
      "required": [
        "begin",
        "end",
        "arg_index"
      ],
      "additionalProperties": false,
      "properties": {
        "begin": {
          "type": "integer",
          "minimum": 0,
          "description": "Begin A-token index (inclusive) for this argument-origin span."
        },
        "end": {
          "type": "integer",
          "minimum": 0,
          "description": "End A-token index (exclusive) for this argument-origin span."
        },
        "arg_index": {
          "type": "integer",
          "minimum": 0,
          "description": "Zero-based macro parameter index that produced this span."
        },
        "byte_begin": {
          "type": "integer",
          "minimum": 0,
          "description": "For paste_spans: begin byte offset (inclusive) within the spelled output token."
        },
        "byte_end": {
          "type": "integer",
          "minimum": 0,
          "description": "For paste_spans: end byte offset (exclusive) within the spelled output token."
        },
        "pp_byte_begin": {
          "type": "integer",
          "minimum": 0,
          "description": "Byte offset in the preprocessed output (A stream) where this span begins (elided if unavailable)."
        },
        "pp_byte_end": {
          "type": "integer",
          "minimum": 0,
          "description": "Byte offset in the preprocessed output (A stream) where this span ends (exclusive) (elided if unavailable)."
        }
      },
      "dependentRequired": {
        "byte_begin": [
          "byte_end"
        ],
        "byte_end": [
          "byte_begin"
        ],
        "pp_byte_begin": [
          "pp_byte_end"
        ],
        "pp_byte_end": [
          "pp_byte_begin"
        ]
      },
      "description": "Inherits logic from PPSpan but manually flattened for validator compatibility."
    },
    "TokMapEntry": {
      "type": "object",
      "required": [
        "file",
        "pp",
        "b",
        "e"
      ],
      "additionalProperties": false,
      "properties": {
        "file": {
          "type": "string",
          "minLength": 1,
          "description": "Absolute or canonicalized file path this token range maps to"
        },
        "pp": {
          "type": "integer",
          "minimum": 0,
          "description": "Index of a token in the preprocess stream A."
        },
        "b": {
          "type": "integer",
          "minimum": 0,
          "description": "Begin byte offset within 'file' (inclusive)"
        },
        "e": {
          "type": "integer",
          "minimum": 0,
          "description": "End byte offset within 'file' (exclusive)"
        }
      }
    },
    "Slot": {
      "type": "object",
      "required": [
        "id",
        "kind",
        "file",
        "b",
        "e"
      ],
      "additionalProperties": false,
      "properties": {
        "id": {
          "type": "integer",
          "minimum": 0,
          "description": "Stable slot id"
        },
        "kind": {
          "type": "string",
          "enum": [
            "file_begin",
            "file_end",
            "after_last_include",
            "before_include",
            "after_include",
            "arm_begin",
            "arm_end"
          ],
          "description": "Semantic insertion anchor"
        },
        "file": {
          "type": "string",
          "minLength": 1,
          "description": "Path of the file whose offsets b/e apply to"
        },
        "ref": {
          "type": "integer",
          "minimum": 0,
          "description": "For include/arm slots: include item id or cond-arm id"
        },
        "pp": {
          "type": "integer",
          "minimum": 0,
          "description": "Optional A-token index for stable ordering when b/e are identical"
        },
        "b": {
          "type": "integer",
          "minimum": 0,
          "description": "Start byte offset in 'file'"
        },
        "e": {
          "type": "integer",
          "minimum": 0,
          "description": "End byte offset in 'file' (b == e for point slots)"
        },
        "owner_include_id": {
          "type": "integer",
          "minimum": 0,
          "description": "If this slot belongs to a specific included-file instance, the include item id that opened it"
        }
      },
      "allOf": [
        {
          "if": {
            "properties": {
              "kind": {
                "enum": [
                  "before_include",
                  "after_include"
                ]
              }
            },
            "required": [
              "kind"
            ]
          },
          "then": {
            "required": [
              "ref"
            ]
          }
        },
        {
          "if": {
            "properties": {
              "kind": {
                "enum": [
                  "arm_begin",
                  "arm_end"
                ]
              }
            },
            "required": [
              "kind"
            ]
          },
          "then": {
            "required": [
              "ref"
            ]
          }
        }
      ]
    },
    "Arm": {
      "type": "object",
      "required": [
        "id",
        "kind",
        "body_b",
        "body_e",
        "selected"
      ],
      "additionalProperties": false,
      "properties": {
        "id": {
          "type": "integer",
          "minimum": 0,
          "description": "Unique arm id"
        },
        "kind": {
          "type": "string",
          "enum": [
            "if",
            "ifdef",
            "ifndef",
            "elif",
            "else"
          ],
          "description": "Arm directive kind (#if/#ifdef/#ifndef/#elif/#else)."
        },
        "cond": {
          "type": "string",
          "minLength": 1,
          "description": "As written after #if/#elif, or the macro name for ifdef/ifndef (optional)"
        },
        "body_b": {
          "type": "integer",
          "minimum": 0,
          "description": "First byte of this arms body (after directive line)"
        },
        "body_e": {
          "type": "integer",
          "minimum": 0,
          "description": "Byte after the last byte belonging to this arms body (before next directive line)"
        },
        "pp_span": {
          "description": "A-token span [begin,end) in the preprocessed stream for this arm.",
          "$ref": "#/$defs/PPSpan"
        },
        "selected": {
          "type": "boolean",
          "description": "True iff this arm contributed tokens in the preprocessed output for this file instance"
        }
      },
      "allOf": [
        {
          "if": {
            "properties": {
              "selected": {
                "const": true
              }
            },
            "required": [
              "selected"
            ]
          },
          "then": {
            "required": [
              "pp_span"
            ]
          },
          "else": {
            "not": {
              "required": [
                "pp_span"
              ]
            }
          }
        },
        {
          "if": {
            "properties": {
              "kind": {
                "not": {
                  "const": "else"
                }
              }
            },
            "required": [
              "kind"
            ]
          },
          "then": {
            "required": [
              "cond"
            ]
          }
        }
      ]
    },
    "Cond": {
      "type": "object",
      "required": [
        "id",
        "file",
        "group_b",
        "group_e",
        "arms"
      ],
      "additionalProperties": false,
      "properties": {
        "id": {
          "type": "integer",
          "minimum": 0,
          "description": "Unique per map id"
        },
        "file": {
          "type": "string",
          "minLength": 1,
          "description": "Path of the source file"
        },
        "parent_arm_id": {
          "type": "integer",
          "minimum": 0,
          "description": "Enclosing conditional arm id if nested (elided for top-level)."
        },
        "parent_include_id": {
          "type": "integer",
          "minimum": 0,
          "description": "Include item id that opened this file instance (elided in the main TU)."
        },
        "group_b": {
          "type": "integer",
          "minimum": 0,
          "description": "Byte of '#' starting the #if/ifdef/ifndef line"
        },
        "group_e": {
          "type": "integer",
          "minimum": 0,
          "description": "Byte AFTER the newline ending the #endif line"
        },
        "arms": {
          "type": "array",
          "minItems": 1,
          "items": {
            "$ref": "#/$defs/Arm"
          },
          "description": "Conditional arms in source order."
        }
      }
    },
    "HeaderDecl": {
      "type": "object",
      "description": "Logical header-level declaration inside an include instance.",
      "required": [
        "kind",
        "name",
        "header_span",
        "pp_span"
      ],
      "additionalProperties": false,
      "properties": {
        "kind": {
          "type": "string",
          "enum": [
            "function",
            "variable",
            "typedef",
            "enum",
            "struct",
            "union",
            "unknown"
          ],
          "description": "Coarse decl kind: function, variable, typedef, etc."
        },
        "name": {
          "type": "string",
          "minLength": 1,
          "description": "Primary identifier for this declaration (e.g. function name)."
        },
        "header_span": {
          "description": "Byte span [b,e) in the *header file* containing this decl.",
          "type": "object",
          "required": [
            "b",
            "e"
          ],
          "additionalProperties": false,
          "properties": {
            "file": {
              "type": "string",
              "minLength": 1,
              "description": "Header file path containing this declaration. If omitted, it is implied by the enclosing include's opened_path when present, otherwise by legacy resolved_path."
            },
            "b": {
              "type": "integer",
              "minimum": 0,
              "description": "Begin byte offset (inclusive) within the header file."
            },
            "e": {
              "type": "integer",
              "minimum": 0,
              "description": "End byte offset (exclusive) within the header file."
            }
          }
        },
        "pp_span": {
          "description": "A-token span [begin,end) in the preprocessed stream for this decl.",
          "$ref": "#/$defs/PPSpan"
        }
      }
    },
    "MacroItem": {
      "type": "object",
      "required": [
        "id",
        "kind",
        "subkind",
        "name",
        "spans",
        "inv_text"
      ],
      "additionalProperties": false,
      "properties": {
        "id": {
          "type": "integer",
          "minimum": 0,
          "description": "Unique item id."
        },
        "kind": {
          "const": "macro",
          "description": "Item kind discriminator (always 'macro')."
        },
        "subkind": {
          "type": "string",
          "enum": [
            "func",
            "obj"
          ],
          "description": "Macro form: function-like ('func') or object-like ('obj')."
        },
        "name": {
          "type": "string",
          "minLength": 1,
          "description": "Macro identifier."
        },
        "spans": {
          "type": "array",
          "minItems": 1,
          "items": {
            "$ref": "#/$defs/PPSpan"
          },
          "description": "Token spans in the preprocessed stream A that together represent this macro's expansion."
        },
        "arg_spans": {
          "type": "array",
          "minItems": 1,
          "items": {
            "$ref": "#/$defs/PPArgSpan"
          },
          "description": "A-token spans within pp_cover that originate from any actual macro arguments."
        },
        "stringify_spans": {
          "type": "array",
          "minItems": 1,
          "items": {
            "$ref": "#/$defs/PPArgSpan"
          },
          "description": "A-token spans within pp_cover that originate from macro-body stringification of an argument (e.g. '#X')."
        },
        "paste_spans": {
          "type": "array",
          "minItems": 1,
          "items": {
            "$ref": "#/$defs/PPArgSpan"
          },
          "description": "A-token spans within pp_cover that originate from macro-body token-paste involving an argument (e.g. 'X##Y'). Multiple spans may overlap when a single pasted token depends on multiple arguments. For pasted tokens, byte_begin/byte_end refer to the substring within the emitted token spelling."
        },
        "body_spans": {
          "type": "array",
          "minItems": 1,
          "items": {
            "$ref": "#/$defs/PPSpan"
          },
          "description": "A-token spans within pp_cover that originate from the macro body (non-argument tokens)."
        },
        "inv_text": {
          "type": [
            "string",
            "null"
          ],
          "minLength": 1,
          "description": "Exact bytes from the producer-selected source envelope for this macro invocation. For an ordinary source call this is usually a normal NAME(...) spelling such as 'FOO(1, 2)'. For generated function-like invocations, however, the exact source envelope may be noncanonical, such as a caller argument segment 'ADD, (1, 2)' that is not itself a valid NAME(...) call. Null means the producer could not prove an exact raw source envelope and intentionally withheld raw-invocation proof material. Consumers that require a canonical function-like invocation must use normalized_inv_text when present rather than reinterpret inv_text."
        },
        "normalized_inv_text": {
          "type": "string",
          "minLength": 1,
          "description": "Canonical function-like invocation text synthesized from the resolved callee spelling and actual argument spellings when no ordinary source NAME(...) invocation spelling is available or suitable. This is producer-owned proof material for generated calls, including literal or caller-parameter callees whose arguments are unpacked from a caller tuple. When present, this field is the invocation text consumers must use for generated-call replay and NAME(...)-shape parsing; inv_text remains only the exact raw source envelope and may be noncanonical. Consumers must not reconstruct this field heuristically."
        },
        "inv_file": {
          "type": "string",
          "minLength": 1,
          "description": "File containing the macro invocation (elided if unknown)."
        },
        "inv_b": {
          "type": "integer",
          "minimum": 0,
          "description": "Begin byte offset of the invocation within 'inv_file' (elided if unknown)."
        },
        "inv_e": {
          "type": "integer",
          "minimum": 0,
          "description": "End byte offset (exclusive) of the invocation within 'inv_file' (elided if unknown)."
        },
        "inv_pp_byte_begin": {
          "type": "integer",
          "minimum": 0,
          "description": "Byte offset in the preprocessed output (A stream) for the start of the expansion associated with this invocation (elided if unknown/unavailable)."
        },
        "inv_pp_byte_end": {
          "type": "integer",
          "minimum": 0,
          "description": "Byte offset in the preprocessed output (A stream) for the end (exclusive) of the expansion associated with this invocation (elided if unknown/unavailable)."
        },
        "owner_include_id": {
          "type": "integer",
          "minimum": 0,
          "description": "Include item id that opened inv_file (when it's an included header instance)"
        },
        "inv_arg_ranges": {
          "type": "array",
          "description": "Per-formal-parameter source byte ranges for the macro invocation arguments. Entry i corresponds to formal parameter index i. For variadic macros, the variadic parameter entry spans the entire variadic tail (including commas). Ranges use the same coordinate space as inv_b/inv_e (byte offsets within inv_file); to index into inv_text, subtract inv_b. Endpoints may be null if the range cannot be recovered.",
          "items": {
            "$ref": "#/$defs/OptByteRange"
          }
        },
        "normalized_inv_arg_text_ranges": {
          "type": "array",
          "description": "Half-open byte ranges [b,e) within normalized_inv_text, one per generated invocation argument in invocation order. Entry i selects only argument i's spelling and is parallel to arg_tuple_refs[i] when tuple provenance is present. These offsets are relative to normalized_inv_text, not inv_file or inv_text; null endpoints mean the producer withheld that range.",
          "items": {
            "$ref": "#/$defs/OptByteRange"
          }
        },
        "definition_directive_id": {
          "type": [
            "integer",
            "null"
          ],
          "minimum": 0,
          "description": "Directive item id for the active macro definition used by this invocation, when known. This allows the consumer to detect preserved call sites whose defining #define was consumed by a TU edit."
        },
        "caller_macro_id": {
          "type": "integer",
          "minimum": 0,
          "description": "If this invocation was generated while expanding another recorded macro invocation, this is the item id of that immediate generating caller. The edge is local to one expansion step, never a transitive root link. Argument flow across the edge is described separately by arg_deps/arg_refs/arg_tuple_refs, while callee_origin describes the invoked-name token. The field may be omitted when the producer cannot prove the immediate caller."
        },
        "callee_origin": {
          "$ref": "#/$defs/CalleeOrigin",
          "description": "Producer-proven immediate-edge origin of the complete callee token for this invocation. literal_macro_name identifies a fixed replacement-list name; caller_param identifies direct contribution from formal(s) of caller_macro_id; paste and opaque retain their conservative meanings. Consumers may compose caller_param provenance through caller edges only when each required edge is independently proven."
        },
        "def_params": {
          "type": "array",
          "items": {
            "$ref": "#/$defs/MacroParam"
          },
          "description": "Formal parameter list from the macro definition for this invocation (object-like macros omit this field)."
        },
        "arg_deps": {
          "type": "array",
          "items": {
            "type": "array",
            "items": {
              "type": "integer",
              "minimum": 0
            },
            "uniqueItems": true
          },
          "description": "For each argument in this invocation (in invocation order), the set of caller formal indices referenced in the raw argument text. This captures argument-text dependencies only; it does not describe higher-order callee-token provenance, which is recorded separately in callee_origin."
        },
        "arg_refs": {
          "type": [
            "array",
            "null"
          ],
          "items": {
            "type": "array",
            "items": {
              "type": "object",
              "required": [
                "caller_param_index",
                "byte_begin",
                "byte_end"
              ],
              "additionalProperties": false,
              "properties": {
                "caller_param_index": {
                  "type": "integer",
                  "minimum": 0
                },
                "byte_begin": {
                  "type": "integer",
                  "minimum": 0
                },
                "byte_end": {
                  "type": "integer",
                  "minimum": 0
                }
              }
            }
          },
          "description": "Immediate-edge argument provenance in invocation order. For child argument i, arg_refs[i] is an ordered sequence of (caller_param_index, byte_begin, byte_end) triples identifying half-open byte slices [byte_begin,byte_end) within that child's raw argument spelling that came from formals of caller_macro_id. The indices therefore name only the immediate caller's formals; transitive root provenance must be composed by the consumer. This refines arg_deps for argument-text lifting and never describes the callee token."
        },
        "arg_tuple_refs": {
          "type": [
            "array",
            "null"
          ],
          "items": {
            "type": "array",
            "items": {
              "$ref": "#/$defs/TupleArgRef"
            }
          },
          "description": "Immediate-edge structural provenance for generated invocation arguments unpacked from a caller tuple/signature. For generated argument i, arg_tuple_refs[i] is an ordered sequence of exact half-open byte slices within trimmed actual-argument text for a formal of caller_macro_id. The array is parallel to normalized_inv_arg_text_ranges when both are present. This supports literal and caller-parameter generated callees such as 'G z' and 'f t'; it is not transitive root provenance and must be composed by the consumer."
        },
        "paste_tokens": {
          "type": "array",
          "items": {
            "$ref": "#/$defs/PasteToken"
          },
          "description": "Exact producer-side witnesses for tokens synthesized by `##` within this invocation, recorded in deterministic expansion order. Each witness records the final pasted spelling and an ordered replay segmentation tree containing argument slices and fixed literal spans."
        }
      },
      "dependentRequired": {
        "inv_pp_byte_begin": [
          "inv_pp_byte_end"
        ],
        "inv_pp_byte_end": [
          "inv_pp_byte_begin"
        ],
        "inv_b": [
          "inv_e",
          "inv_file"
        ],
        "inv_e": [
          "inv_b",
          "inv_file"
        ]
      },
      "description": "inv_b/inv_e are BYTES in the main source, not token indices."
    },
    "IncludeLookupKind": {
      "type": "string",
      "enum": [
        "source_relative",
        "quote_dir",
        "user_I",
        "system",
        "idirafter",
        "framework",
        "builtin",
        "absolute_operand",
        "unknown"
      ],
      "description": "Producer-owned classification for how an include edge was resolved. source_relative and absolute_operand are not entries in pp_ctx.include_search_chain. quote_dir, user_I, system, idirafter, framework, and builtin identify effective search-chain entries."
    },
    "IncludeSearchEntryKind": {
      "type": "string",
      "enum": [
        "quote_dir",
        "user_I",
        "system",
        "idirafter",
        "framework",
        "builtin",
        "unknown"
      ],
      "description": "Kind of an effective include search-chain entry. source_relative and absolute_operand are per-edge lookup kinds, not global search-chain entries."
    },
    "IncludeSearchEntry": {
      "type": "object",
      "required": [
        "index",
        "kind",
        "spelling",
        "path"
      ],
      "additionalProperties": false,
      "properties": {
        "index": {
          "type": "integer",
          "minimum": 0,
          "description": "Stable zero-based effective search-chain index used by include lookup provenance."
        },
        "kind": {
          "$ref": "#/$defs/IncludeSearchEntryKind",
          "description": "Search-chain entry kind."
        },
        "spelling": {
          "type": "string",
          "minLength": 1,
          "description": "Directory spelling as supplied/observed by Clang for this search entry, preserving relative/symlink spelling when available."
        },
        "path": {
          "type": "string",
          "minLength": 1,
          "description": "Physical or FileManager path for this search entry, suitable for filesystem identity checks after canonicalization."
        }
      },
      "description": "One producer-normalized effective header-search entry. Indices are referenced by include.lookup.search_chain_index and include_next.resume_search_chain_index."
    },
    "IncludeLookupProvenance": {
      "type": "object",
      "required": [
        "kind"
      ],
      "additionalProperties": false,
      "properties": {
        "kind": {
          "$ref": "#/$defs/IncludeLookupKind",
          "description": "How this include edge was found."
        },
        "search_chain_index": {
          "type": "integer",
          "minimum": 0,
          "description": "Effective pp_ctx.include_search_chain index that selected this include edge. Required for search-chain lookup kinds and omitted for source_relative, absolute_operand, and unknown."
        },
        "directory_spelling": {
          "type": "string",
          "minLength": 1,
          "description": "Spelling of the directory that selected this include edge. Required for source_relative and absolute_operand. Deprecated audit redundancy for search-chain hits; when present it must match pp_ctx.include_search_chain[search_chain_index].spelling."
        },
        "directory_path": {
          "type": "string",
          "minLength": 1,
          "description": "Physical/FileManager path of the directory that selected this include edge. Required for source_relative and absolute_operand. Deprecated audit redundancy for search-chain hits; when present it must match pp_ctx.include_search_chain[search_chain_index].path."
        }
      },
      "allOf": [
        {
          "if": {
            "properties": {
              "kind": {
                "enum": [
                  "quote_dir",
                  "user_I",
                  "system",
                  "idirafter",
                  "framework",
                  "builtin"
                ]
              }
            },
            "required": [
              "kind"
            ]
          },
          "then": {
            "required": [
              "search_chain_index"
            ]
          }
        },
        {
          "if": {
            "properties": {
              "kind": {
                "enum": [
                  "source_relative",
                  "absolute_operand"
                ]
              }
            },
            "required": [
              "kind"
            ]
          },
          "then": {
            "required": [
              "directory_spelling",
              "directory_path"
            ],
            "not": {
              "required": [
                "search_chain_index"
              ]
            }
          }
        },
        {
          "if": {
            "properties": {
              "kind": {
                "const": "unknown"
              }
            },
            "required": [
              "kind"
            ]
          },
          "then": {
            "not": {
              "anyOf": [
                {
                  "required": [
                    "search_chain_index"
                  ]
                },
                {
                  "required": [
                    "directory_spelling"
                  ]
                },
                {
                  "required": [
                    "directory_path"
                  ]
                }
              ]
            }
          }
        }
      ],
      "description": "Producer-owned lookup provenance for one include edge. Search-chain hits are keyed by search_chain_index into pp_ctx.include_search_chain; source_relative and absolute_operand carry per-edge directory spelling/path. Legacy redundant directory fields on search-chain hits are accepted only as audited copies."
    },
    "IncludeNextProvenance": {
      "type": "object",
      "required": [
        "provenance"
      ],
      "additionalProperties": false,
      "properties": {
        "provenance": {
          "type": "string",
          "enum": [
            "known",
            "unknown"
          ],
          "description": "known means Clang's include-next resume cursor was represented in this object. unknown means the consumer must fail closed for general include_next preservation/replay proof."
        },
        "containing_file_include_id": {
          "type": "integer",
          "minimum": 0,
          "description": "Include item id for the file containing this #include_next directive. The containing file's selected lookup index is derived from that include's lookup.search_chain_index when needed."
        },
        "resume_search_chain_index": {
          "type": "integer",
          "minimum": 0,
          "description": "Effective pp_ctx.include_search_chain index where #include_next lookup resumed in the producer run. This is the non-redundant cursor needed to replay #include_next."
        }
      },
      "allOf": [
        {
          "if": {
            "properties": {
              "provenance": {
                "const": "known"
              }
            },
            "required": [
              "provenance"
            ]
          },
          "then": {
            "required": [
              "containing_file_include_id",
              "resume_search_chain_index"
            ]
          }
        },
        {
          "if": {
            "properties": {
              "provenance": {
                "const": "unknown"
              }
            },
            "required": [
              "provenance"
            ]
          },
          "then": {
            "not": {
              "anyOf": [
                {
                  "required": [
                    "containing_file_include_id"
                  ]
                },
                {
                  "required": [
                    "resume_search_chain_index"
                  ]
                }
              ]
            }
          }
        }
      ],
      "description": "Producer-owned #include_next resume provenance. The selected target is not duplicated here; it is represented by the include edge's opened_path, entered_file_spelling, and lookup.search_chain_index."
    },
    "DirectiveIncludeItem": {
      "type": "object",
      "required": [
        "id",
        "kind",
        "subkind",
        "text",
        "site_path",
        "target",
        "angled",
        "site_b",
        "site_e",
        "spans"
      ],
      "additionalProperties": false,
      "properties": {
        "id": {
          "type": "integer",
          "minimum": 0,
          "description": "Unique item id."
        },
        "kind": {
          "const": "directive",
          "description": "Item kind discriminator (always 'directive')."
        },
        "subkind": {
          "type": "string",
          "enum": [
            "#include",
            "#include_next"
          ],
          "description": "Preprocessor include directive kind."
        },
        "text": {
          "type": "string",
          "minLength": 1,
          "description": "Exact directive text as written (e.g., '#include <...>')."
        },
        "site_path": {
          "type": "string",
          "minLength": 1,
          "description": "File containing this #include/#include_next directive"
        },
        "target": {
          "type": "string",
          "minLength": 1,
          "description": "As-written header string from the directive (e.g., \"e.h\" or <vector>)"
        },
        "resolved_path": {
          "type": "string",
          "minLength": 1,
          "description": "Legacy include-path spelling for this include edge. This field is retained for backward compatibility only. Consumers should prefer opened_path for physical identity proof and entered_file_spelling for __FILE__ / __FILE_NAME__ observer proof when those fields are present."
        },
        "opened_path": {
          "type": "string",
          "minLength": 1,
          "description": "Producer-owned physical/FileManager path for the file opened by this include edge. This is the preferred input to physical identity proof; consumers should compare replayed candidates against it with path-equivalence logic rather than using filename-observer spelling."
        },
        "entered_file_spelling": {
          "type": "string",
          "minLength": 1,
          "description": "Exact file spelling Clang exposed through __FILE__ while preprocessing this include instance. This is the preferred proof source for filename observers and intentionally preserves relative, symlink, search-directory, or absolute spelling."
        },
        "entered_file_name": {
          "type": "string",
          "minLength": 1,
          "description": "Exact unescaped file-name spelling Clang would expose through __FILE_NAME__ at the start of this include instance, before later #line / linemarker changes. Optional audit metadata; consumers may compute this from entered_file_spelling when absent."
        },
        "lookup": {
          "$ref": "#/$defs/IncludeLookupProvenance",
          "description": "Producer-owned lookup provenance for this include edge. When present, consumers should use it to prove ordinary include replay and #include_next selected-target equivalence."
        },
        "include_next": {
          "$ref": "#/$defs/IncludeNextProvenance",
          "description": "Additional non-redundant resume provenance for #include_next directives. Omitted for ordinary #include and optional for backward compatibility with older maps."
        },
        "angled": {
          "type": "boolean",
          "description": "True if angle brackets (<...>) were used."
        },
        "parent": {
          "type": "integer",
          "minimum": 0,
          "description": "Optional item id of a logical parent include."
        },
        "site_b": {
          "type": [
            "integer",
            "null"
          ],
          "minimum": 0,
          "description": "Begin byte offset of the include directive within 'site_path'"
        },
        "site_e": {
          "type": [
            "integer",
            "null"
          ],
          "minimum": 0,
          "description": "End byte offset (exclusive) of the include directive within 'site_path'"
        },
        "spans": {
          "type": "array",
          "items": {
            "$ref": "#/$defs/PPSpan"
          },
          "description": "Token spans in A corresponding to the expansion of the included file."
        },
        "decls": {
          "type": "array",
          "minItems": 1,
          "items": {
            "$ref": "#/$defs/HeaderDecl"
          },
          "description": "Logical header-level declarations for this include instance, in source order."
        }
      },
      "dependentSchemas": {
        "decls": {
          "anyOf": [
            {
              "required": [
                "opened_path"
              ]
            },
            {
              "required": [
                "resolved_path"
              ]
            }
          ],
          "description": "Header declarations need a header-file identity source: opened_path in the new schema or legacy resolved_path."
        }
      },
      "allOf": [
        {
          "if": {
            "properties": {
              "subkind": {
                "const": "#include"
              }
            },
            "required": [
              "subkind"
            ]
          },
          "then": {
            "not": {
              "required": [
                "include_next"
              ]
            }
          }
        }
      ]
    },
    "DirectiveMacroItem": {
      "type": "object",
      "required": [
        "id",
        "kind",
        "subkind",
        "name",
        "text",
        "spans",
        "site_path",
        "site_b",
        "site_e"
      ],
      "additionalProperties": false,
      "properties": {
        "id": {
          "type": "integer",
          "minimum": 0,
          "description": "Unique item id."
        },
        "kind": {
          "const": "directive",
          "description": "Item kind discriminator (always 'directive')."
        },
        "subkind": {
          "type": "string",
          "enum": [
            "#define",
            "#undef"
          ],
          "description": "Directive kind (#define or #undef)."
        },
        "name": {
          "type": "string",
          "minLength": 1,
          "description": "Macro identifier whose state is changed by this #define/#undef directive. This is producer-owned proof data so consumers do not need to parse directive text for macro-state lookup."
        },
        "text": {
          "type": "string",
          "minLength": 1,
          "description": "Exact directive text (e.g., '#define FOO ...')."
        },
        "function_like": {
          "type": "boolean",
          "description": "For #define directives, true iff the active MacroInfo was function-like. This producer-owned shape bit lets consumers distinguish object-like from function-like macro-state observations without reparsing directive text. Omitted for #undef."
        },
        "spans": {
          "type": "array",
          "items": {
            "$ref": "#/$defs/PPSpan"
          },
          "description": "Token spans in A that this directive's expansion contributed to. May be empty."
        },
        "site_path": {
          "type": "string",
          "minLength": 1,
          "description": "File containing this #define/#undef directive"
        },
        "site_b": {
          "type": [
            "integer",
            "null"
          ],
          "minimum": 0,
          "description": "Begin byte offset of the directive within 'site_path'"
        },
        "site_e": {
          "type": [
            "integer",
            "null"
          ],
          "minimum": 0,
          "description": "End byte offset (exclusive) of the directive within 'site_path'"
        },
        "owner_include_id": {
          "type": "integer",
          "minimum": 0,
          "description": "Include item id that opened this file instance (disambiguates repeated includes)"
        },
        "def_params": {
          "type": "array",
          "items": {
            "$ref": "#/$defs/MacroParam"
          },
          "description": "Formal parameter list from this #define directive when it is function-like. Omitted for object-like macro definitions."
        },
        "replacement_tokens": {
          "type": "array",
          "items": {
            "$ref": "#/$defs/MacroReplacementToken"
          },
          "description": "Producer-owned replay tape for the macro replacement list, in definition order. Parameter references point at def_params by param_index; other tokens are fixed literal spellings."
        }
      },
      "allOf": [
        {
          "if": {
            "properties": {
              "subkind": {
                "const": "#define"
              }
            },
            "required": [
              "subkind"
            ]
          },
          "then": {
            "required": [
              "function_like"
            ]
          },
          "else": {
            "not": {
              "required": [
                "function_like"
              ]
            }
          }
        }
      ],
      "description": "Represents #define/#undef lines (definitions), not invocation sites."
    },
    "DirectivePragmaItem": {
      "type": "object",
      "required": [
        "id",
        "kind",
        "subkind",
        "text",
        "site_path",
        "site_b",
        "site_e"
      ],
      "additionalProperties": false,
      "properties": {
        "id": {
          "type": "integer",
          "minimum": 0,
          "description": "Unique item id."
        },
        "kind": {
          "const": "directive",
          "description": "Item kind discriminator (always 'directive')."
        },
        "subkind": {
          "const": "#pragma",
          "description": "Directive kind (always '#pragma')."
        },
        "text": {
          "type": "string",
          "minLength": 1,
          "description": "Exact directive text as written (e.g., '#pragma once')."
        },
        "site_path": {
          "type": "string",
          "minLength": 1,
          "description": "Path of the FILE containing this #pragma."
        },
        "site_b": {
          "type": [
            "integer",
            "null"
          ],
          "minimum": 0,
          "description": "Begin byte offset of the #pragma line in 'site_path'."
        },
        "site_e": {
          "type": [
            "integer",
            "null"
          ],
          "minimum": 0,
          "description": "End byte offset (exclusive) of the #pragma line in 'site_path'."
        },
        "owner_include_id": {
          "type": [
            "integer",
            "null"
          ],
          "minimum": 0,
          "description": "Include item id that opened site_path for this pragma occurrence, when known. Used to disambiguate repeated header instances."
        },
        "via_pragma_operator": {
          "type": "boolean",
          "description": "True when this pragma was spelled with the _Pragma(\"...\") operator rather than a #pragma directive line. Omitted (defaults false) for directive pragmas and older maps; lets the consumer fold a pragma content edit back into the operator form."
        },
        "operator_b": {
          "type": "integer",
          "minimum": 0,
          "description": "For a _Pragma operator, begin byte offset of the _Pragma(\"...\") expression within 'site_path'. May be narrower than [site_b, site_e) when the operator is mid-line."
        },
        "operator_e": {
          "type": "integer",
          "minimum": 0,
          "description": "For a _Pragma operator, end byte offset (exclusive) of the _Pragma(\"...\") expression within 'site_path'."
        }
      }
    },
    "FileItem": {
      "type": "object",
      "required": [
        "id",
        "kind",
        "subkind",
        "path",
        "spans"
      ],
      "additionalProperties": false,
      "properties": {
        "id": {
          "type": "integer",
          "minimum": 0,
          "description": "Unique item id."
        },
        "kind": {
          "const": "file",
          "description": "Item kind discriminator (always 'file')."
        },
        "subkind": {
          "const": "file",
          "description": "Subkind discriminator (always 'file')."
        },
        "path": {
          "type": "string",
          "minLength": 1,
          "description": "Optional file path this item refers to."
        },
        "spans": {
          "type": "array",
          "items": {
            "$ref": "#/$defs/PPSpan"
          },
          "description": "A-token spans emitted while the current TU (top-level) was active."
        }
      }
    },
    "LineControlEvent": {
      "type": "object",
      "required": [
        "id",
        "physical_file",
        "site_b",
        "site_e",
        "active",
        "producer_proven",
        "logical_line_after",
        "logical_file_after"
      ],
      "additionalProperties": false,
      "properties": {
        "id": {
          "type": "integer",
          "minimum": 0,
          "description": "Stable line-control event id."
        },
        "physical_file": {
          "type": "string",
          "minLength": 1,
          "description": "Physical source file containing the directive site."
        },
        "site_b": {
          "type": [
            "integer",
            "null"
          ],
          "minimum": 0,
          "description": "Begin byte offset of the physical directive line, or null when unavailable."
        },
        "site_e": {
          "type": [
            "integer",
            "null"
          ],
          "minimum": 0,
          "description": "End byte offset of the physical directive line, or null when unavailable."
        },
        "active": {
          "type": "boolean",
          "description": "True iff Clang executed this line-control directive for this preprocessing run."
        },
        "producer_proven": {
          "type": "boolean",
          "description": "True iff the logical effect was recorded after Clang evaluated directive operands and conditional activity."
        },
        "logical_line_after": {
          "type": "integer",
          "minimum": 0,
          "description": "Logical line number established for the next physical line after the directive."
        },
        "logical_file_after": {
          "type": "string",
          "description": "Logical file spelling established after the directive."
        },
        "owner_include_id": {
          "type": "integer",
          "minimum": 0,
          "description": "Include item id that opened the physical file instance containing the directive."
        },
        "text": {
          "type": "string",
          "description": "Exact source text for the physical directive line when available."
        }
      }
    },
    "Item": {
      "oneOf": [
        {
          "$ref": "#/$defs/MacroItem"
        },
        {
          "$ref": "#/$defs/DirectiveIncludeItem"
        },
        {
          "$ref": "#/$defs/DirectiveMacroItem"
        },
        {
          "$ref": "#/$defs/DirectivePragmaItem"
        },
        {
          "$ref": "#/$defs/FileItem"
        }
      ]
    },
    "OptByteRange": {
      "type": "object",
      "description": "A byte range where endpoints may be unknown (null).",
      "additionalProperties": false,
      "required": [
        "b",
        "e"
      ],
      "properties": {
        "b": {
          "anyOf": [
            {
              "type": "integer",
              "minimum": 0
            },
            {
              "type": "null"
            }
          ],
          "description": "Inclusive byte offset from the start of the file, or null if unknown."
        },
        "e": {
          "anyOf": [
            {
              "type": "integer",
              "minimum": 0
            },
            {
              "type": "null"
            }
          ],
          "description": "Exclusive byte offset from the start of the file, or null if unknown."
        }
      }
    },
    "MacroParam": {
      "type": "object",
      "additionalProperties": false,
      "required": [
        "name",
        "variadic"
      ],
      "properties": {
        "name": {
          "type": "string",
          "minLength": 1,
          "description": "Formal parameter name as written in the macro definition."
        },
        "variadic": {
          "type": "boolean",
          "description": "True iff this parameter is the variadic parameter (C99 '...' or GNU named varargs)."
        }
      },
      "description": "A single formal macro parameter from the macro definition corresponding to a recorded invocation."
    },
    "PastePart": {
      "type": "object",
      "required": [
        "byte_begin",
        "byte_end"
      ],
      "additionalProperties": false,
      "properties": {
        "arg_index": {
          "type": "integer",
          "minimum": 0,
          "description": "Macro parameter index that contributed this contiguous substring of the pasted token spelling. Omitted for literal body fragments participating in the paste."
        },
        "byte_begin": {
          "type": "integer",
          "minimum": 0,
          "description": "Inclusive byte offset of this part within the final pasted token spelling."
        },
        "byte_end": {
          "type": "integer",
          "minimum": 0,
          "description": "Exclusive byte offset of this part within the final pasted token spelling."
        },
        "kind": {
          "type": "string",
          "enum": [
            "arg",
            "literal"
          ],
          "description": "Part discriminator. arg means the bytes came from a macro argument token; literal means the bytes are fixed replacement-list text participating in the paste."
        },
        "spelling": {
          "type": "string",
          "description": "Exact bytes contributed by this part to the final pasted token spelling."
        },
        "arg_byte_begin": {
          "type": "integer",
          "minimum": 0,
          "description": "For arg parts, begin byte offset within the invocation argument spelling that supplied this part. Omitted when the producer cannot prove a direct argument-source slice."
        },
        "arg_byte_end": {
          "type": "integer",
          "minimum": 0,
          "description": "For arg parts, end byte offset within the invocation argument spelling that supplied this part. Omitted when the producer cannot prove a direct argument-source slice."
        }
      },
      "dependentRequired": {
        "arg_byte_begin": [
          "arg_byte_end"
        ],
        "arg_byte_end": [
          "arg_byte_begin"
        ]
      },
      "description": "One ordered segment of an exact pasted-token replay. Arg segments identify the formal argument and optional source slice; literal segments identify fixed replacement-list bytes such as delimiter tokens."
    },
    "PasteToken": {
      "type": "object",
      "required": [
        "spelling",
        "parts"
      ],
      "additionalProperties": false,
      "properties": {
        "spelling": {
          "type": "string",
          "description": "Exact spelled token synthesized by one `##` projection for this invocation."
        },
        "parts": {
          "type": "array",
          "description": "Ordered exact replay decomposition of the pasted token spelling. The segments tile the final token byte range from left to right and include both argument-derived slices and fixed literal/delimiter spans.",
          "items": {
            "$ref": "#/$defs/PastePart"
          }
        }
      }
    },
    "TupleArgRef": {
      "type": "object",
      "required": [
        "caller_param_index",
        "caller_byte_begin",
        "caller_byte_end"
      ],
      "additionalProperties": false,
      "properties": {
        "caller_param_index": {
          "type": "integer",
          "minimum": 0,
          "description": "Zero-based formal index in caller_macro_id whose trimmed actual-argument spelling owns this tuple/signature slice."
        },
        "caller_byte_begin": {
          "type": "integer",
          "minimum": 0,
          "description": "Begin byte offset (inclusive) of the forwarded slice within the trimmed actual-argument spelling for caller_param_index. The outer tuple delimiters remain in this coordinate space."
        },
        "caller_byte_end": {
          "type": "integer",
          "minimum": 0,
          "description": "End byte offset (exclusive) of the forwarded slice within the trimmed actual-argument spelling for caller_param_index. Together with caller_byte_begin this forms a half-open byte interval."
        }
      }
    },
    "CalleeOrigin": {
      "type": "object",
      "required": [
        "kind"
      ],
      "additionalProperties": false,
      "properties": {
        "kind": {
          "type": "string",
          "enum": [
            "literal_macro_name",
            "caller_param",
            "paste",
            "opaque"
          ],
          "description": "Provenance class for the macro callee token at this invocation site. literal_macro_name means the callee identifier came from a literal macro name spelled in the replacement list/body. caller_param means the callee token came from a caller formal parameter (higher-order/x-macro style). paste means the callee token was synthesized by token pasting. opaque means the producer could not prove a more specific origin."
        },
        "caller_param_indices": {
          "type": "array",
          "items": {
            "type": "integer",
            "minimum": 0
          },
          "uniqueItems": true,
          "description": "When kind is caller_param, the zero-based formal indices in caller_macro_id that directly contributed the complete callee token spelling at this invocation site. The indices are immediate-edge provenance, not transitive root indices. A uniquely forwarded caller-supplied callee is represented by exactly one index; multiple indices remain explicit and require separate consumer proof. Empty or omitted for literal_macro_name, paste, or opaque."
        },
        "spelling": {
          "type": "string",
          "description": "For paste-derived or otherwise segmented callees, the exact final callee token spelling."
        },
        "parts": {
          "type": "array",
          "items": {
            "$ref": "#/$defs/CalleeOriginPart"
          },
          "description": "Ordered producer-proven decomposition of the callee token spelling. For paste origins, these parts tile spelling and identify the root selector argument slices that contributed to the pasted callee name."
        }
      },
      "description": "Producer-proven structural provenance for the complete invoked macro name at one caller edge. literal_macro_name is fixed replacement-list text; caller_param identifies formal(s) of caller_macro_id that supplied the callee token; paste records synthesized spelling; opaque withholds a stronger claim. Consumers may admit higher-order replay only by composing these explicit edge-local facts and must fail closed when required provenance is missing or non-unique.",
      "allOf": [
        {
          "if": {
            "properties": {
              "kind": {
                "const": "paste"
              }
            },
            "required": [
              "kind"
            ]
          },
          "then": {
            "required": [
              "spelling",
              "parts"
            ]
          }
        }
      ]
    },
    "MacroReplacementToken": {
      "type": "object",
      "required": [
        "kind",
        "spelling"
      ],
      "additionalProperties": false,
      "properties": {
        "kind": {
          "type": "string",
          "enum": [
            "literal",
            "param_ref"
          ],
          "description": "Replacement-list token kind. literal is fixed replacement-list text; param_ref is a reference to a macro formal parameter."
        },
        "spelling": {
          "type": "string",
          "description": "Exact token spelling recorded by the producer for this replacement-list token."
        },
        "param_index": {
          "type": "integer",
          "minimum": 0,
          "description": "For param_ref tokens, the zero-based formal parameter index referenced by this replacement-list token."
        }
      },
      "allOf": [
        {
          "if": {
            "properties": {
              "kind": {
                "const": "param_ref"
              }
            },
            "required": [
              "kind"
            ]
          },
          "then": {
            "required": [
              "param_index"
            ]
          }
        }
      ],
      "description": "Producer-owned replay token for a macro definition replacement list. This lets the consumer replay simple macro definitions without reparsing #define text."
    },
    "CalleeOriginPart": {
      "type": "object",
      "required": [
        "kind",
        "spelling"
      ],
      "additionalProperties": false,
      "properties": {
        "kind": {
          "type": "string",
          "enum": [
            "literal",
            "caller_arg_slice"
          ],
          "description": "Segment kind for a macro callee token. literal is fixed replacement-list text; caller_arg_slice is a slice of a root invocation argument that participated in the callee spelling."
        },
        "spelling": {
          "type": "string",
          "description": "Exact bytes contributed by this segment to the callee token spelling."
        },
        "root_macro_id": {
          "type": "integer",
          "minimum": 0,
          "description": "For caller_arg_slice, the root macro invocation item id whose argument supplied this callee segment."
        },
        "root_param_index": {
          "type": "integer",
          "minimum": 0,
          "description": "For caller_arg_slice, the zero-based root invocation formal/argument index containing this selector slice."
        },
        "byte_begin": {
          "type": "integer",
          "minimum": 0,
          "description": "For caller_arg_slice, begin byte offset within the trimmed root argument spelling."
        },
        "byte_end": {
          "type": "integer",
          "minimum": 0,
          "description": "For caller_arg_slice, end byte offset within the trimmed root argument spelling."
        }
      },
      "allOf": [
        {
          "if": {
            "properties": {
              "kind": {
                "const": "caller_arg_slice"
              }
            },
            "required": [
              "kind"
            ]
          },
          "then": {
            "required": [
              "root_macro_id",
              "root_param_index",
              "byte_begin",
              "byte_end"
            ]
          }
        }
      ],
      "description": "One producer-proven segment of a macro callee token spelling, used especially for paste-derived callee selector substitution."
    }
  }
}
)json";

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSCHEMA_H
