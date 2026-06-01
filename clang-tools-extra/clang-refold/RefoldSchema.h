//===- RefoldSchema.h - JSON schema for clang-refold map --------*- C++ -*-===//
//
// This header embeds (as a string literal) the JSON Schema that describes the
// on-disk “refold map” produced by the modified Clang preprocessor and consumed
// by the clang-refold tool. The map records how the unmodified preprocessed
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
//     "argv": ["<clang arg token>", ...],   // replayed for deterministic --check
//     "lang": "<high-level language>"       // e.g. "c", "c++", "objc"
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
//   "items":  [ Item, ... ]                 // macros/includes/defs/pragmas/files
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
//         * byte_begin/byte_end      : bytes in the original source (when known)
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
//       - optional #define replay proof data: def_params and replacement_tokens
//
// * DirectivePragmaItem
//     A #pragma line with exact text and location (site_path, [site_b, site_e))
//     (nullable bounds when unavailable).
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
//     Consumers should key by pp; producers typically emit one entry per A token.
//
// * Slot
//     Explicit insertion anchors in original source bytes. Kinds include:
//     { file_begin, file_end, after_last_include, before_include, after_include,
//       arm_begin, arm_end }.
//       - file, [b,e): byte range in file (point slots have b == e)
//       - ref: required for include/arm kinds; identifies include item id or
//         conditional arm id (per-kind)
//       - pp: optional stabilizer for ordering when multiple slots share b/e
//       - owner_include_id: include instance that opened file (when nested)
//
// * Cond / Arm
//     Conditional groups with absolute byte bounds [group_b, group_e) in a file,
//     plus ordered arms. Each Arm records:
//       - kind: if/ifdef/ifndef/elif/else
//       - cond: textual condition / macro name (required except for else)
//       - body byte range [body_b, body_e)
//       - selected: true iff this arm contributed tokens to A
//       - pp_span: present iff selected is true
//
// Invariants & conventions
// ------------------------
// * All token intervals are half-open A-token ranges [begin, end) with end >= begin.
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
// A-token spans for PP constructs, byte locations for directive/invocation sites
// when available, and the conditional/include structure governing token
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
      "description": "Preprocessor invocation context used to produce stream A; used by clang-refold --check to re-run preprocessing and validate alignment against edited stream B.",
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
              "description": "Header file path containing this declaration. If omitted, it is implied by the enclosing include's resolved_path."
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
          "description": "Exact bytes at the macro call site in the source (e.g., 'FOO(1, 2)'). Null means the producer could not prove an exact raw invocation spelling and intentionally withheld raw-invocation proof material."
        },
        "normalized_inv_text": {
          "type": "string",
          "minLength": 1,
          "description": "Canonicalized function-like invocation text synthesized from actual callee arguments when the source spelling is not a normal NAME(...) form (for example, higher-order tuple forwarding such as '(G z)')."
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
          "description": "Per-formal-parameter byte ranges within normalized_inv_text. Entry i corresponds to formal parameter index i. These ranges are relative to the synthesized normalized_inv_text string rather than inv_file source bytes.",
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
          "description": "If this macro invocation occurred while expanding another macro, this is the item id of the immediately enclosing (caller) macro invocation. This encodes the structural nesting DAG only; argument flow is described separately by arg_deps/arg_refs and callee_origin."
        },
        "callee_origin": {
          "$ref": "#/$defs/CalleeOrigin",
          "description": "Origin of the callee token for this invocation. A literal callee name is safe for generalized nested args-only refolding; caller-param, paste, or opaque origins must be conservatively expanded."
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
          "description": "Per-callee-parameter mapping back to slices of caller formal arguments. For parameter i, arg_refs[i] is an ordered sequence of (caller_param_index, byte_begin, byte_end) triples describing which caller argument slices were substituted into this callee parameter's raw argument text. This refines arg_deps for argument-text lifting only and does not encode callee-token provenance."
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
          "description": "Per-callee-parameter structural forwarding from a caller formal tuple/signature. For parameter i, arg_tuple_refs[i] is an ordered sequence of slices within the trimmed caller-formal text that produce this callee argument. This is used for higher-order forms such as '(G z)' where callee arguments are unpacked from a single caller formal rather than referenced by name."
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
          "description": "Filesystem path actually opened for this include as resolved by the preprocessor."
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
      "dependentRequired": {
        "decls": [
          "resolved_path"
        ]
      }
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
          "description": "Zero-based index of the caller formal that owns the tuple/signature text."
        },
        "caller_byte_begin": {
          "type": "integer",
          "minimum": 0,
          "description": "Begin byte offset (inclusive) of the forwarded slice within the trimmed caller-formal text."
        },
        "caller_byte_end": {
          "type": "integer",
          "minimum": 0,
          "description": "End byte offset (exclusive) of the forwarded slice within the trimmed caller-formal text."
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
          "description": "When kind is caller_param, the caller formal parameter indices that directly contributed the callee token spelling at this invocation site. Empty or omitted for literal_macro_name, paste, or opaque."
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
      "description": "Structural provenance for the invoked macro name itself. The consumer uses this to conservatively disable args-only and DAG-lift refolding for higher-order or otherwise non-literal callee origins.",
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
