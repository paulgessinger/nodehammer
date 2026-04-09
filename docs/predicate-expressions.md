# Predicate Expressions

Predicate expressions are a concise syntax for specifying node matching criteria in nodehammer configuration files. They can be used in `keep_if`/`drop_if` fields of `[[selection_rules]]` and in the `match` field of `[[rules]]`.

## Quick example

```toml
# Table form (verbose):
[[selection_rules]]
[selection_rules.keep_if]
type = "and"
operands = [
  { type = "tag", key = "sensitive", value = "true" },
  { type = "or", operands = [
    { type = "path_glob", pattern = "**/Pixels/**" },
    { type = "path_glob", pattern = "**/ShortStrips/**" },
  ] },
]

# Expression form (concise):
[[selection_rules]]
keep_if = 'tag.sensitive == "true" && any(path ~= "**/Pixels/**", path ~= "**/ShortStrips/**")'
```

Both forms produce the same predicate AST. The table form remains valid and can be mixed with string expressions in the same config file.

## Atoms

| Expression | Description |
|---|---|
| `true` | Always matches |
| `false` | Never matches |
| `is_leaf` | Matches nodes with no children |
| `name ~= "pattern"` | Glob match against the node's name |
| `path ~= "pattern"` | Glob match against the node's original path |
| `tag.KEY` | Matches if the tag `KEY` exists (any value) |
| `tag.KEY == "value"` | Matches if tag `KEY` equals `"value"` |

### Glob patterns

The `~=` operator performs glob matching with the following rules:

- `*` matches any sequence of characters **within** a single path segment (does not cross `/`)
- `**` matches any sequence of characters **including** `/` (crosses segment boundaries)
- All other characters match literally (case-sensitive)

Examples:
- `name ~= "sensor*"` matches `sensor0`, `sensor_barrel`, etc.
- `path ~= "**/Pixels/**"` matches any node under a `Pixels` ancestor
- `path ~= "**/Pixels"` matches a node named `Pixels` at any depth

### Tag predicates

Tags are key-value string pairs attached to nodes by importers. Tag keys in expressions use dot notation after `tag`:

- `tag.sensitive` -- true if the node has a tag with key `sensitive` (regardless of value)
- `tag.sensitive == "true"` -- true if the tag value is exactly `"true"`
- `tag.sub_detector == "barrel"` -- underscore in key names is supported

Note: tag values are always strings. Use quoted string literals for comparison values.

## Operators

### Logical operators (binary, infix)

| Operator | Description | Precedence |
|---|---|---|
| `!` | Logical NOT (unary prefix) | Highest |
| `&&` | Logical AND | Middle |
| `\|\|` | Logical OR | Lowest |

Precedence means `a \|\| b && c` is parsed as `a \|\| (b && c)`, and `!a && b` is parsed as `(!a) && b`.

Use parentheses to override precedence:

```
(path ~= "**/A/**" || path ~= "**/B/**") && tag.sensitive == "true"
```

### Variadic functions

| Function | Description |
|---|---|
| `any(a, b, c, ...)` | True if **any** argument matches (variadic OR) |
| `all(a, b, c, ...)` | True if **all** arguments match (variadic AND) |

These are syntactic sugar for `\|\|` and `&&` with multiple operands. They are particularly useful when combining many alternatives:

```
any(
  path ~= "**/Pixels/**",
  path ~= "**/ShortStrips/**",
  path ~= "**/LongStrips/**"
)
```

Arguments can be any expression, including nested function calls:

```
all(tag.sensitive == "true", any(path ~= "**/A/**", path ~= "**/B/**"))
```

## Usage in configuration

### Selection rules

```toml
[[selection_rules]]
drop_if = 'name ~= "*"'

[[selection_rules]]
keep_if = 'tag.sensitive == "true" && path ~= "**/Tracker/**"'
```

Multi-line expressions are supported using TOML multi-line literal strings (`'''`):

```toml
[[selection_rules]]
keep_if = '''
any(
  path ~= "**/Pixels/*Barrel",
  path ~= "**/Pixels/*EndcapN",
  path ~= "**/Pixels/*EndcapP"
)
'''
```

### Rules match field

```toml
[[rules]]
scope = "**/Tracker/**"
material = "silicon"
match = 'tag.sensitive == "true"'
```

## Grammar

For reference, the formal grammar in PEG notation:

```
expr       <- or_expr
or_expr    <- and_expr ('||' and_expr)*
and_expr   <- unary_expr ('&&' unary_expr)*
unary_expr <- '!' unary_expr / primary
primary    <- func_call / atom / '(' expr ')'

func_call  <- ('any' / 'all') '(' expr (',' expr)* ','? ')'

atom       <- 'true' / 'false' / 'is_leaf' / tag_expr / path_expr / name_expr
tag_expr   <- 'tag.' IDENT ('==' STRING)?
path_expr  <- 'path' '~=' STRING
name_expr  <- 'name' '~=' STRING

STRING     <- '"' [^"]* '"'
IDENT      <- [a-zA-Z_][a-zA-Z0-9_]*
```

## Error messages

The parser provides position-aware error messages for common mistakes:

- `unexpected '&' at position 5; did you mean '&&'?` -- single `&` instead of `&&`
- `unexpected '|' at position 5; did you mean '||'?` -- single `|` instead of `||`
- `unterminated string starting at position 10` -- missing closing quote
- `unknown identifier 'foo' at position 0` -- unrecognized keyword
- `expected ')' at position 15` -- unclosed parenthesis
