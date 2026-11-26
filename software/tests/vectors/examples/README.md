# free-pdk examples

The `*.ihx` files are copied unchanged from `easy-pdk-programmer-software/Examples/`
(Copyright (C) 2019-2020 freepdk, https://free-pdk.github.io, GPL-3.0-or-later).
The copy is needed so that the tests do not depend on the submodule, which in this project is only a reference.

`gen_packages.py` builds `ex_*` packages from them (one IHRC stub in each, except `adctest_pfs173`). These
packages are used by the firmware tests (no false stub matches) and the client tests (a package
bit-identical to the one built from the HEX).
