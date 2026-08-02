# Formatter

`chaaya fmt` ([`src/fmt.c`](../../src/fmt.c)) is a comment-preserving CST
formatter inspired by Kaappi's `fmt.zig`.

## Usage

```bash
chaaya fmt [--check] [files...]
```

## Behavior

- Lexes every lexeme including line/block/`#;` comments and blank-line structure
- Layout: 2-space indent, LF endings, 80-column reflow
- Before writing, re-reads original and formatted text with the real reader and
  compares datum sequences with `equal?`; refuses to write on mismatch

`--check` exits nonzero if a file would be reformatted (CI-safe).
