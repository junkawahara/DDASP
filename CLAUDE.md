# DDASP — ASP to Decision Diagram compiler

## Project goal

- This project uses **ASP (Answer Set Programming)** as a description language.
  The user writes a problem as an ASP program, and this project converts it into a
  **ZDD** (Zero-suppressed Decision Diagram) or another kind of decision diagram
  (BDD, MTBDD, etc.) that represents the set of answer sets (or a related structure).
- The decision diagram construction is done with the **KyotoDD** library.

## KyotoDD

- `KyotoDD` in this directory is a **symbolic link** to the KyotoDD library
  (`../KyotoDD`). Do not copy the library into this repository; always use it via the
  symlink.
- KyotoDD is a C++ BDD/ZDD library with Python bindings (`import kyotodd`).
  Read `KyotoDD/CLAUDE.md`, `KyotoDD/README.md`, and the headers under
  `KyotoDD/include/` for API details. Do not duplicate the KyotoDD API in this file.
- Do not modify files under `KyotoDD/` from this project. If a change to KyotoDD is
  needed, ask the user first.

## Rules

- **Commit to git after every code change.** Each logical change gets its own commit
  with a clear message.
- Implement incrementally, step by step, so the user can follow every change.
- Never decide unclear specifications on your own — ask the user first.
- Write tests for every implemented feature.
