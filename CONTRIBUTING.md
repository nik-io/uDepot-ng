# Contributing to uDepot-ng

Thank you for your interest in contributing to uDepot-ng.

## Getting Started

1. Fork the repository
2. Create a feature branch from `main`
3. Make your changes
4. Run the test suite: `ctest --test-dir build`
5. Submit a pull request

## Development Setup

```bash
git clone https://github.com/nik-io/uDepot-ng.git
cd uDepot-ng
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build
```

## Code Style

Follow the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
with the conventions documented in `CLAUDE.md`. In short:

- C++23, compiled with `-Wall -Wextra -Werror`
- `snake_case` for functions/variables, `PascalCase` for types
- Member variables use `_` suffix
- `#pragma once` for headers
- No exceptions on the I/O hot path

## Design Constraints

Read `docs/architecture.md` before making changes. The four design principles
(zero copy, no global locking, minimal amplification, enterprise-grade crash
recovery) are non-negotiable. If a change conflicts with one of them, open an
issue to discuss before submitting a PR.

## Testing

- Every new public API must have corresponding tests
- Test both success and error paths
- A failing test must fail the build
- Non-SPDK tests must always pass

## Reporting Issues

Open an issue on GitHub. Include:
- What you expected to happen
- What actually happened
- Steps to reproduce
- Environment (OS, compiler, backend)

## Code of Conduct

This project follows the [Contributor Covenant Code of Conduct](CODE_OF_CONDUCT.md).
