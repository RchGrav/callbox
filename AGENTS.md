# Product language

These rules apply to first-party help, interface labels, menus, CLI output,
errors, documentation, examples, tests, scripts, comments, and docstrings.

1. **Verify the behavior.** Trace each material claim to the parser, handler,
   data model, or an applicable test before writing it. Existing prose and
   comments are evidence to check, not proof. Record unresolved questions.
2. **Use the glossary.** Follow [docs/glossary.md](docs/glossary.md) for shared
   terms. Preserve command names, identifiers, protocol fields, environment
   variables, and required syntax. Update the glossary with related wording
   when an implemented concept changes.
3. **Write conventional technical English.** Name the operation and object:
   “Pane Navigation,” “Focus adjacent pane,” “Select recipient.” Use concise
   labels and direct descriptions. Remove conversational filler, jokes,
   metaphors, vague directions, and unsupported absolutes.
4. **Organize help by task.** Start with functional categories, then operations,
   usage, and details. Keep command topics and aliases discoverable. Use compact
   quick references for keyboard controls and mode-specific help; put workflows,
   examples, constraints, and related commands in the linked reference.
5. **Describe the actual effect.** Distinguish the program, process record,
   profile, pane, window, console, and inbox. Include prerequisites, scope,
   state changes, and side effects when they affect the user's decision.
   Distinguish an observed failure from a suspected cause.
6. **Edit connected surfaces together.** Check help pages, command usage,
   interface hints, documentation, examples, and text asserted by tests for the
   same operation. Use consistent spelling, capitalization, key notation,
   argument notation, and punctuation within each surface.
7. **Keep comments useful.** Correct stale behavior claims. Explain interfaces,
   constraints, and non-obvious rationale; remove narration, duplication, and
   unsupported commentary. Add comments only when they clarify the code.
8. **Keep language changes in scope.** Preserve runtime behavior and established
   syntax. Report behavioral defects separately; implement a fix only when the
   requested work clearly includes it. Make the smallest coherent wording and
   help-presentation changes.
9. **Verify and report.** Recheck changed claims against implementation, inspect
   the diff for unintended behavior changes, and run relevant available checks.
   Inspect rendered help at the terminal sizes being supported. Report actual
   files/surfaces reviewed, exclusions, results, and remaining gaps; do not claim
   broader coverage than the review establishes.

The help-source organization is documented in [docs/help.md](docs/help.md).

# File size

**No file over 500 lines.** Every first-party source, header, script and
test file stays at or under 500 lines. Why: a file a person or an agent can
read whole is one they can change safely; past that, a file holds more than
one responsibility and its parts hide each other. Split by responsibility
(one concern per file, in its layer; `make lint` still checks the layer
direction), never mechanically at line 500.

- Not first-party, so not counted: generated files marked "Do not edit"
  (`src/screen/font_data.h`, `gen/`) and vendored code (`third_party/`).
- Files over the limit when the rule began are listed in
  `scripts/lint_lines.allow` with their size then as a ceiling. They may
  shrink, never grow, and leave the list once split. Nothing new joins it.
- `scripts/lint_lines.sh` checks it, in `make lint` and `scripts/ci.sh`.
