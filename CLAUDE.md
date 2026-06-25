
**Before starting any work on this project, read `DESIGN.md`.**

## Guidelines

### 1. General guidelines

- **Sprite checkpoints**: Never run `sprite-env checkpoints` automatically.

- **Markdown formatting** When writing or editing any `.md` file, wrap
  prose paragraphs so that no line exceeds 65 characters. This applies
  to body text only.

	- do not reformat code blocks, tables, or headings.

- **Fix one issue at a time:** Complete the full plan-act-validate
  cycle for a single issue before moving to the next.

- **Verify starting state, surface tradeoffs before
  implementing:** Before writing code, include a step that
  confirms the starting state, and present design tradeoffs
  for review rather than committing to one approach (e.g.,
  dead nodes in redirects, branch-vs-direct commit).

- **Verification:** Always run full test suite before committing.

- **Inspect tests via a saved log:** When running tests for
  inspection, save output to a file rather than repeatedly piping to
  grep. 

- **Git Merges:** Always merge branches using `git merge --squash`.

- **Code Formatting:** Always run `make format` before committing.

### 2. Think Before Coding

**Don't assume. Don't hide confusion. Surface tradeoffs.**

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them - don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

### 3. Simplicity First

**Minimum code that solves the problem. Nothing speculative.**

- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask yourself: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

### 4. Surgical Changes

**Touch only what you must. Clean up only your own mess.**

When editing existing code:
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it - don't delete it.

When your changes create orphans:
- Remove imports/variables/functions that YOUR changes made unused.
- Don't remove pre-existing dead code unless asked.

The test: Every changed line should trace directly to the user's request.

### 5. Goal-Driven Execution

**Define success criteria. Loop until verified.**

Transform tasks into verifiable goals:
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan:
```
1. [Step] → verify: [check]
2. [Step] → verify: [check]
3. [Step] → verify: [check]
```

Strong success criteria let you loop independently. Weak criteria
("make it work") require constant clarification.


