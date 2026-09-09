# FreeInkBook SDK Development Guide

Project: Low-level book rendering SDK (EPUB, CSS, text layout, glyph caching)
Repo: https://github.com/Belphemur/freeink-sdk — fork of https://github.com/Free-Ink/freeink-sdk

## Scope

This work is part of the CrossPoint X reader TTF font support effort (see
https://github.com/Belphemur/XPoint PR #96). The three features to implement
are:

1. **Strikethrough** — `StyleStrikethrough` bit + `line-through` CSS parsing
2. **Synthetic-hyphen flag** — `LayoutHyphenated` on `PageTextRun`
3. **Drawn horizontal rule** — `PageRule` element + render arm

## Layout

- `libs/book/FreeInkBook/include/` — public headers (BookFont.h, Layout.h, Css.h)
- `libs/book/FreeInkBook/src/` — engine source (ChapterLayout.cpp, PageRenderer.cpp, Css.cpp)
- `libs/book/FreeInkBook/test/host/` — host tests (test_freeinkbook.cpp)

## Build/Style

- Header guards: `#pragma once`
- Classes: PascalCase, methods: camelCase, constants: UPPER_SNAKE_CASE
- Run host tests: `cd libs/book/FreeInkBook/test/host && ./run.sh`
