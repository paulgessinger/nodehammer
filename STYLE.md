# Code style — nodehammer

Project conventions beyond what the compiler and CMake enforce.

## Indexing containers

Avoid bare `operator[]` on vectors (and similar indexable containers) when it is not necessary. Prefer:

- **Range-based `for`** when visiting every element
- **Iterators** when you need position or partial traversal
- **`at(i)`** when you want a bounds-checked access, or the index is not obviously valid

Use `operator[]` only when the index is known valid and the unchecked access is intentional (for example, a tight inner loop where bounds are already established).

## Braced control flow

Always use `{}` braces for the body of `if`, `else`, `for`, `while`, and `do-while`, even when the body is a single statement.

Do:

```cpp
if (ready) {
    run();
}

for (auto &x : items) {
    use(x);
}
```

Do not:

```cpp
if (ready)
    run();

for (auto &x : items)
    use(x);
```

`else if` chains keep one statement per branch; each branch still uses braces around its body.
