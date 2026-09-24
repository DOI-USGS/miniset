# Contributing to Miniset

Thank you for your interest in contributing. Miniset is developed by the
U.S. Geological Survey (USGS) Astrogeology Science Center, and we welcome
contributions from both inside and outside the USGS.

This document explains how to report problems, propose changes, and get a
contribution merged.

## Code of conduct

Be respectful and constructive. Discussion should stay focused on the technical
merits of the work. Harassment or discriminatory behavior of any kind is not
tolerated, and maintainers may remove comments or contributions that violate
this expectation.

## Reporting bugs

Open an issue at <https://github.com/DOI-USGS/miniset/issues> and include:

- What you expected to happen, and what actually happened.
- The exact command or code needed to reproduce the problem.
- Your platform and versions: OS, compiler, CMake, GDAL, PROJ, and (for
  WebAssembly builds) Emscripten.
- Any relevant error output or stack trace, as text rather than a screenshot.
- A minimal input file, or a description of one, if the problem is data-specific.

Search the existing issues first — the problem may already be filed or fixed
on `main`.

## Requesting features

Open an issue describing the problem you are trying to solve rather than only
the solution you have in mind. Explain the scientific or operational use case;
that context helps us judge scope and design a fix that serves other users too.

## Reporting security issues

Do not open a public issue for a suspected security vulnerability. Email the
maintainer (see [Contact](#contact)) with the details and allow time for a fix
before disclosing publicly.

## Development setup

Miniset builds natively with CMake and, optionally, to WebAssembly with
Emscripten.

```bash
# Native build
mamba env create -f environment.yaml -n miniset
mamba activate miniset

git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

See the [README](README.md) for the WebAssembly build and for usage examples,
and the [documentation site](https://DOI-USGS.github.io/miniset/) for the full
API reference.

## Submitting changes

1. **Open an issue first** for anything beyond a small fix. This avoids
   duplicated effort and lets us agree on the approach before you write code.
2. **Fork the repository** and create a branch off `main`. Use a short,
   descriptive branch name.
3. **Make your change**, including tests (see below).
4. **Run the test suite** and confirm it passes.
5. **Open a pull request** against `main`. Describe what changed and why, and
   link the issue it addresses.
6. **Respond to review.** A maintainer will review your pull request; changes to
   CI workflows additionally require review by a code owner.

Keep pull requests focused on a single concern. Unrelated changes bundled
together are slower to review and harder to revert.

### Commit messages

Write a short imperative subject line (for example, "Fix DEM radius lookup at
the poles"), and use the body to explain *why* the change is needed when that is
not obvious from the diff.

If you used generative AI tools to author or modify code, note that in the pull
request description so it can be recorded per USGS disclosure requirements. See
the Generative AI Disclosure section of the [README](README.md).

## Coding standards

- **C++17.** Match the style of the surrounding code: 4-space indentation, and
  the naming and comment conventions already used in the file you are editing.
- **Public headers** live in `include/`, implementations in `src/`. Keep
  third-party headers out of public headers where a forward declaration will do.
- **Portability.** Much of the library compiles to both native and WebAssembly.
  Guard native-only dependencies rather than assuming they are present.
- **No commented-out code** or backup files in commits.

## Tests

New behavior needs a test, and bug fixes should come with a test that fails
before the fix and passes after it.

- C++ tests use GoogleTest and live in [tests/](tests/); register new test files
  in [tests/CMakeLists.txt](tests/CMakeLists.txt).
- WebAssembly tests live in [tests/wasm/](tests/wasm/).

Run the native suite with `ctest --test-dir build --output-on-failure` before
opening a pull request.

## Documentation

Update the documentation when you change public behavior. Prose and API
reference live in [docs/](docs/) and are published with MkDocs; the README
covers the quick-start path.

## Licensing and attribution

Miniset is released into the public domain under [CC0 1.0](LICENSE.md). By
contributing, you agree that your contribution is released under the same terms
and that you have the right to make that dedication.

Work produced by USGS employees in the course of their duties is a work of the
U.S. Government and is not subject to copyright protection in the United States.

## Contact

Kelvin Rodriguez — <krodriguez@usgs.gov>
U.S. Geological Survey, Astrogeology Science Center
