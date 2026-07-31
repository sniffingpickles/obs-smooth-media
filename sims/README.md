# Exploratory simulations

The scripts in this directory are historical investigation tools. They model
specific earlier revisions of the timing and recovery logic and are useful for
reproducing the failure modes that motivated the reliability work. They are not
acceptance tests for the current implementation.

The maintained executable timing specification is
`tests/test-timing-model.py`. The compiled core and decoder tests are under
`tests/`; see `docs/testing.md` for the supported test and sanitizer commands.
