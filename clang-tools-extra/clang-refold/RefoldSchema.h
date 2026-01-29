//===- RefoldSchema.h - JSON schema for clang-refold map --------*- C++ -*-===//
//
// This header embeds (as a string literal) the JSON Schema that describes the
// on-disk “refold map” produced by the modified Clang preprocessor and consumed
// by the clang-refold tool. The map records how the unmodified preprocessed
// token stream A (pp-tokens) was derived from source constructs so that edits
// made to a retransformed/edited preprocessed stream B can be deterministically
// projected back into partially expanded C/C++ source.
//
// High-level model
// ----------------
//   * A: the original, unedited preprocessed token stream (indices 0..N-1).
//   * B: a later, edited preprocessed token stream (not stored in the map).
//   * The schema ties A’s tokens back to source files/bytes and PP constructs:
//       - macro invocations and their expansion covers
//       - include directives and the byte spans they expanded into
//       - explicit separation of macro-expansion tokens by origin:
//         * arg_spans  : tokens sourced from actual macro arguments
//         * body_spans : tokens sourced from the macro body (non-arguments)
//         This enables deterministic handling of non-argument edits by
//         replacing the invocation bytes with the edited expansion text
//         whenever any edit falls in body_spans.
//       - macro definition/undef directives and pragmas
//       - conditional groups (#if/#elif/#else/#endif) with selected arms
//       - token→file byte mapping for A (absolute/canonical paths)
//       - explicit “slots” (stable insertion anchors in original source)
//
// Top-level object
// ----------------
// {
//   "version": "<opaque format version string>",
//   "source":  "<absolute path of the TU we refold back into>",
//   "tokens":  { "count": <N> },       // # of pp-tokens in A
//   "items":   [ Item, ... ],          // macros/includes/defs/pragmas/files
//   "tokmap":  [ TokMapEntry, ... ],   // A-token → (file,[b,e)) mapping
//   "slots":   [ Slot, ... ],          // explicit insertion anchors (optional)
//   "conds":   [ Cond, ... ]           // conditional groups (optional)
// }
//
// Core definitions (selected)
// ---------------------------
// * PPSpan
//     Half-open A-token interval [begin, end). Every Item that contributes
//     preprocessed tokens reports one or more spans; “pp_cover” is the minimal
//     interval covering all of an item’s spans.
//
// * MacroItem
//     A macro expansion in A, with:
//       - name, kind (func/obj), spans[], pp_cover
//       - invocation site bytes: inv_file, [inv_b, inv_e)  (BYTES in that file)
//       - owner_include_id: include instance that opened inv_file (nullable)
//
// * DirectiveIncludeItem
//     A single #include/#include_next instance, with:
//       - site_path, [site_b, site_e)  (BYTES of the directive line)
//       - target (as written), resolved_path, angled, parent (logical parent
//       id)
//       - spans[] (A-token spans produced by this include), pp_cover
//
// * DirectiveMacroItem
//     A #define/#undef directive line (definition), with optional pp_cover and
//     owner_include_id (to disambiguate repeated header instances).
//
// * DirectivePragmaItem
//     A #pragma line with exact bytes and location (site_path, [site_b,
//     site_e)).
//
// * FileItem
//     Represents tokens emitted while the main TU (top-level) was active;
//     spans[] and optional pp_cover.
//
// * TokMapEntry
//     A-token → source mapping: file (absolute/canonical), [b,e) BYTES in that
//     file. The array is ordered so that tokmap[i] corresponds to A-token i.
//     (An optional “pp” field, when present, MUST equal that index.)
//
// * Slot
//     Explicit insertion anchors in original source bytes. Kinds include
//     { file_begin, file_end, after_last_include, before_include,
//     after_include, arm_begin, arm_end }. For include/arm slots, “ref” points
//     at the owning include/arm id. Slots may also carry a stabilizing “pp”
//     token index when multiple slots share identical byte offsets.
//
// * Cond / Arm
//     Conditional groups with absolute byte bounds [group_b, group_e) in a
//     file, plus ordered arms. Each arm records its kind
//     (if/ifdef/ifndef/elif/else), body byte range [body_b, body_e), optional
//     textual condition, and a boolean “selected” indicating that the arm
//     contributed tokens in A.
//
// Invariants & conventions
// ------------------------
// * All token intervals are half-open A-token ranges [begin, end) with end ≥
// begin.
// * All byte intervals are half-open file byte ranges [b, e) with e ≥ b.
// * Paths are absolute/canonical wherever applicable; include “target”
// preserves
//   the as-written form (e.g., "e.h" or <vector>).
// * Items’ ids are unique within the file. Relationships (e.g.,
// owner_include_id)
//   use those ids to bind expansions and include instances deterministically.
// * “additionalProperties” is disabled across the schema to keep the format
//   closed and fully specified. Producers should populate all required fields;
//   consumers should treat missing required data as a hard error.
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
// The map is intentionally sufficient for a deterministic refolder: it avoids
// heuristic source recovery by recording the exact A-token covers, explicit
// byte locations for directives and invocation sites, and the
// conditional/include structure that was in effect when each token in A was
// produced.
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
    "items",
    "tokmap"
  ],
  "additionalProperties": false,
  "properties": {
    "version": {
      "type": "string",
      "minLength": 1,
      "$comment": "File format version."
    },
    "pp_ctx": {
      "type": "object",
      "description": "Preprocessor invocation context used to produce the original preprocessed token stream (A). This is used by clang-refold --check to re-run preprocessing of the refolded source under the same flags and verify token alignment against the edited preprocessed output.",
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
          "description": "Argument vector tokens that materially affect preprocessing and must be replayed for deterministic checking (e.g. -D/-U/-I/-isystem/-include/--sysroot/-isysroot/-resource-dir/-triple/-target-cpu/-std/-x). Stored exactly as tokens, in original order.",
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
            "minimum": -1
          },
          "description": "Per-token begin byte offsets in the preprocessed output (A stream). Index i corresponds to A token i."
        },
        "pp_byte_end": {
          "type": "array",
          "items": {
            "type": "integer",
            "minimum": -1
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
    "items": {
      "type": "array",
      "items": {
        "$ref": "#/$defs/Item"
      }
    },
    "tokmap": {
      "type": "array",
      "items": {
        "$ref": "#/$defs/TokMapEntry"
      }
    },
    "slots": {
      "type": "array",
      "description": "Explicit insertion anchors in the ORIGINAL source (bytes) that the refolder can attach new B-only code to without heuristics.",
      "items": {
        "$ref": "#/$defs/Slot"
      }
    },
    "conds": {
      "type": "array",
      "description": "Conditional groups (#if/#elif/#else/#endif) discovered in source files; used to deterministically place edits into the correct arm.",
      "items": {
        "$ref": "#/$defs/Cond"
      }
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
          "minimum": 0
        },
        "end": {
          "type": "integer",
          "minimum": 0
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
          "minimum": 0
        },
        "end": {
          "type": "integer",
          "minimum": 0
        },
        "arg_index": {
          "type": "integer",
          "minimum": 0
        },
        "byte_begin": {
          "type": "integer",
          "minimum": -1
        },
        "byte_end": {
          "type": "integer",
          "minimum": -1
        },
        "pp_byte_begin": {
          "type": "integer",
          "minimum": -1,
          "description": "Byte offset in the preprocessed output (A stream) where this span begins. A value of -1 indicates the producer could not compute the byte range."
        },
        "pp_byte_end": {
          "type": "integer",
          "minimum": -1,
          "description": "Byte offset in the preprocessed output (A stream) where this span ends (exclusive). A value of -1 indicates the producer could not compute the byte range."
        }
      },
      "description": "Inherits logic from PPSpan but manually flattened for validator compatibility.",
      "dependentRequired": {
        "pp_byte_begin": [
          "pp_byte_end"
        ],
        "pp_byte_end": [
          "pp_byte_begin"
        ]
      }
    },
    "MacroItem": {
      "type": "object",
      "required": [
        "id",
        "kind",
        "subkind",
        "name",
        "spans"
      ],
      "additionalProperties": false,
      "properties": {
        "id": {
          "type": "integer",
          "minimum": 0
        },
        "kind": {
          "const": "macro"
        },
        "subkind": {
          "enum": [
            "func",
            "obj"
          ]
        },
        "name": {
          "type": "string",
          "minLength": 1,
          "description": "Macro identifier."
        },
        "spans": {
          "type": "array",
          "items": {
            "$ref": "#/$defs/PPSpan"
          },
          "description": "Token spans in the preprocessed stream A that together represent this macro's expansion."
        },
        "arg_spans": {
          "type": "array",
          "items": {
            "$ref": "#/$defs/PPArgSpan"
          },
          "description": "A-token spans within pp_cover that originate from any actual macro arguments."
        },
        "stringify_spans": {
          "type": "array",
          "items": {
            "$ref": "#/$defs/PPArgSpan"
          },
          "description": "A-token spans within pp_cover that originate from macro-body stringification of an argument (e.g. '#X')."
        },
        "paste_spans": {
          "type": "array",
          "items": {
            "$ref": "#/$defs/PPArgSpan"
          },
          "description": "A-token spans within pp_cover that originate from macro-body token-paste involving an argument (e.g. 'X##Y'). Multiple spans may overlap when a single pasted token depends on multiple arguments."
        },
        "body_spans": {
          "type": "array",
          "items": {
            "$ref": "#/$defs/PPSpan"
          },
          "description": "A-token spans within pp_cover that originate from the macro body (non-argument tokens)."
        },
        "inv_text": {
          "type": "string",
          "description": "Exact bytes at the macro call site in the source (e.g., 'FOO(1, 2)')."
        },
        "inv_file": {
          "type": "string",
          "description": "File containing the macro invocation"
        },
        "inv_b": {
          "type": "integer",
          "minimum": 0,
          "description": "Begin byte offset of the invocation within 'inv_file'"
        },
        "inv_e": {
          "type": "integer",
          "minimum": 0,
          "description": "End byte offset (exclusive) of the invocation within 'inv_file'"
        },
        "pp_cover": {
          "type": "object",
          "required": [
            "begin",
            "end"
          ],
          "properties": {
            "begin": {
              "type": "integer",
              "minimum": 0
            },
            "end": {
              "type": "integer",
              "minimum": 0
            }
          },
          "description": "Minimal [begin,end) A-token interval covering all spans for this item."
        },
        "owner_include_id": {
          "type": [
            "integer",
            "null"
          ],
          "description": "Include item id that opened inv_file (when it's an included header instance)"
        },
        "inv_pp_byte_begin": {
          "type": "integer",
          "minimum": -1,
          "description": "Byte offset in the preprocessed output (A stream) for the start of the expansion associated with this invocation, or -1 if unknown/unavailable."
        },
        "inv_pp_byte_end": {
          "type": "integer",
          "minimum": -1,
          "description": "Byte offset in the preprocessed output (A stream) for the end (exclusive) of the expansion associated with this invocation, or -1 if unknown/unavailable."
        }
      },
      "dependentRequired": {
        "inv_b": [
          "inv_e"
        ],
        "inv_e": [
          "inv_b"
        ],
        "inv_pp_byte_begin": [
          "inv_pp_byte_end"
        ],
        "inv_pp_byte_end": [
          "inv_pp_byte_begin"
        ]
      },
      "$comment": "inv_b/inv_e are BYTES in the main source, not token indices."
    },
    "DirectiveIncludeItem": {
      "type": "object",
      "required": [
        "id",
        "kind",
        "subkind",
        "target",
        "site_path",
        "site_b",
        "site_e"
      ],
      "additionalProperties": false,
      "properties": {
        "id": {
          "type": "integer",
          "minimum": 0
        },
        "kind": {
          "const": "directive"
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
          "default": false,
          "description": "True if angle brackets (<...>) were used."
        },
        "parent": {
          "type": "integer",
          "description": "Optional item id of a logical parent include."
        },
        "site_b": {
          "type": "integer",
          "minimum": 0,
          "description": "Begin byte offset of the include directive within 'site_path'"
        },
        "site_e": {
          "type": "integer",
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
        "pp_cover": {
          "type": "object",
          "required": [
            "begin",
            "end"
          ],
          "properties": {
            "begin": {
              "type": "integer",
              "minimum": 0
            },
            "end": {
              "type": "integer",
              "minimum": 0
            }
          },
          "description": "Minimal [begin,end) A-token interval covering all spans for this include."
        },
        "decls": {
          "type": "array",
          "items": {
            "$ref": "#/$defs/HeaderDecl"
          },
          "description": "Logical header-level declarations for this include instance, in source order."
        }
      },
      "dependentRequired": {
        "site_b": [
          "site_e"
        ],
        "site_e": [
          "site_b"
        ]
      }
    },
    "DirectiveMacroItem": {
      "type": "object",
      "required": [
        "id",
        "kind",
        "subkind",
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
          "minimum": 0
        },
        "kind": {
          "const": "directive"
        },
        "subkind": {
          "enum": [
            "#define",
            "#undef"
          ]
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
          "description": "File containing this #define/#undef directive"
        },
        "site_b": {
          "type": "integer",
          "minimum": 0,
          "description": "Begin byte offset of the directive within 'site_path'"
        },
        "site_e": {
          "type": "integer",
          "minimum": 0,
          "description": "End byte offset (exclusive) of the directive within 'site_path'"
        },
        "pp_cover": {
          "type": "object",
          "required": [
            "begin",
            "end"
          ],
          "properties": {
            "begin": {
              "type": "integer",
              "minimum": 0
            },
            "end": {
              "type": "integer",
              "minimum": 0
            }
          },
          "description": "Minimal [begin,end) A-token interval covering all spans (optional)."
        },
        "owner_include_id": {
          "type": [
            "integer",
            "null"
          ],
          "description": "Include item id that opened this file instance (disambiguates repeated includes)"
        }
      },
      "$comment": "Represents #define/#undef lines (definitions), not invocation sites."
    },
    "DirectivePragmaItem": {
      "type": "object",
      "required": [
        "id",
        "kind",
        "subkind",
        "site_path",
        "text",
        "site_b",
        "site_e"
      ],
      "additionalProperties": false,
      "properties": {
        "id": {
          "type": "integer",
          "minimum": 0
        },
        "kind": {
          "const": "directive"
        },
        "subkind": {
          "const": "#pragma"
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
          "type": "integer",
          "minimum": 0,
          "description": "Begin byte offset of the #pragma line in site_path."
        },
        "site_e": {
          "type": "integer",
          "minimum": 0,
          "description": "End byte offset (exclusive) of the #pragma line in site_path."
        }
      }
    },
    "FileItem": {
      "type": "object",
      "required": [
        "id",
        "kind",
        "subkind",
        "spans"
      ],
      "additionalProperties": false,
      "properties": {
        "id": {
          "type": "integer",
          "minimum": 0
        },
        "kind": {
          "const": "file"
        },
        "subkind": {
          "const": "file"
        },
        "path": {
          "type": "string",
          "description": "Optional file path this item refers to."
        },
        "spans": {
          "type": "array",
          "items": {
            "$ref": "#/$defs/PPSpan"
          },
          "description": "A-token spans emitted while the current TU (top-level) was active."
        },
        "pp_cover": {
          "type": "object",
          "required": [
            "begin",
            "end"
          ],
          "properties": {
            "begin": {
              "type": "integer",
              "minimum": 0
            },
            "end": {
              "type": "integer",
              "minimum": 0
            }
          },
          "description": "Minimal [begin,end) A-token interval covering all spans (optional)."
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
    "TokMapEntry": {
      "type": "object",
      "required": [
        "file",
        "b",
        "e"
      ],
      "additionalProperties": false,
      "properties": {
        "file": {
          "type": "string",
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
        "file",
        "kind",
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
        "file": {
          "type": "string",
          "description": "Path of the file whose offsets b/e apply to"
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
        "ref": {
          "type": [
            "integer",
            "null"
          ],
          "description": "For include/arm slots: include item id or cond-arm id"
        },
        "b": {
          "type": "integer",
          "minimum": -1,
          "description": "Start byte offset in 'file'"
        },
        "e": {
          "type": "integer",
          "minimum": -1,
          "description": "End byte offset in 'file' (b == e for point slots)"
        },
        "owner_include_id": {
          "type": [
            "integer",
            "null"
          ],
          "description": "If this slot belongs to a specific included-file instance, the include item id that opened it"
        },
        "pp": {
          "type": "integer",
          "minimum": 0,
          "description": "Optional A-token index for stable ordering when b/e are identical"
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
        "body_e"
      ],
      "properties": {
        "id": {
          "type": "integer",
          "description": "Unique arm id"
        },
        "kind": {
          "enum": [
            "if",
            "ifdef",
            "ifndef",
            "elif",
            "else"
          ]
        },
        "cond": {
          "type": "string",
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
      }
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
      "properties": {
        "id": {
          "type": "integer",
          "description": "Unique per map id"
        },
        "file": {
          "type": "string",
          "description": "Path of the source file"
        },
        "parent_arm_id": {
          "type": [
            "integer",
            "null"
          ],
          "description": "Enclosing conditional arm id if nested (null for top-level)"
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
        "parent_include_id": {
          "type": [
            "integer",
            "null"
          ],
          "minimum": 0,
          "description": "Include item id that opened this file instance; null in the main TU"
        },
        "arms": {
          "type": "array",
          "items": {
            "$ref": "#/$defs/Arm"
          }
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
      "properties": {
        "kind": {
          "type": "string",
          "description": "Coarse decl kind: function, variable, typedef, etc.",
          "enum": [
            "function",
            "variable",
            "typedef",
            "enum",
            "struct",
            "union",
            "unknown"
          ]
        },
        "name": {
          "type": "string",
          "description": "Primary identifier for this declaration (e.g. function name)."
        },
        "header_span": {
          "description": "Byte span [b,e) in the *header file* containing this decl.",
          "type": "object",
          "required": [
            "file",
            "b",
            "e"
          ],
          "properties": {
            "file": {
              "type": "string"
            },
            "b": {
              "type": "integer",
              "minimum": 0
            },
            "e": {
              "type": "integer",
              "minimum": 0
            }
          }
        },
        "pp_span": {
          "description": "A-token span [begin,end) in the preprocessed stream for this decl.",
          "$ref": "#/$defs/PPSpan"
        }
      }
    }
  }
}
)json";

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSCHEMA_H
