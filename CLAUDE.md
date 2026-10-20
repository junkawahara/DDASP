# DDASP — ASP to Decision Diagram compiler

## Project goal

- This project uses **ASP (Answer Set Programming)** as a description language.
  The user writes a problem as an ASP program, and this project converts it into a
  **ZDD** (Zero-suppressed Decision Diagram) or another kind of decision diagram
  (BDD, MTBDD, etc.) that represents the set of answer sets (or a related structure).
- The decision diagram construction is done with the **KyotoDD** library.

## KyotoDD

- KyotoDD is a **git submodule** at `third_party/KyotoDD`
  (https://github.com/graphillion/kyotodd). Fetch it with
  `git submodule update --init --recursive` (it has a nested TdZdd submodule). Do not
  copy the library into this repository; always use it via the submodule.
- KyotoDD is a C++ BDD/ZDD library with Python bindings (`import kyotodd`).
  Read `third_party/KyotoDD/CLAUDE.md`, `third_party/KyotoDD/README.md`, and the
  headers under `third_party/KyotoDD/include/` for API details. Do not duplicate the
  KyotoDD API in this file.
- Do not modify files under `third_party/KyotoDD/` from this project. If a change to
  KyotoDD is needed, ask the user first.

## Layout and workflow

- C++ core: `include/ddasp/` (headers), `src/` (sources), `app/ddasp/main.cpp` (CLI),
  `test/` (Google Test). Python package: `python/` (pybind11 `ddasp._core`,
  `ddasp` wrapper, pytest suite in `python/tests/`). Design notes: `docs/DESIGN.md`.
- Build and test: `cmake -S . -B build && cmake --build build -j && ctest --test-dir build`,
  then `.venv/bin/pytest` (needs the venv described in `README.md`; after changing the
  C++ core, reinstall the package with `.venv/bin/pip install --no-build-isolation -e python`).
- The grounder in tests is `python -m clingo --mode=gringo` from the venv.
- C++11, each source file within 2000 lines, snake_case for new functions.

## Rules

- **Commit to git after every code change.** Each logical change gets its own commit
  with a clear message.
- Implement incrementally, step by step, so the user can follow every change.
- Never decide unclear specifications on your own — ask the user first.
- Write tests for every implemented feature.
