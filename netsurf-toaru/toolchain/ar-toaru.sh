#!/bin/sh
# Archive tool for the ToaruOS cross build; the host ar understands the
# static ELF objects produced by gcc-toaru.sh.
exec ar "$@"
