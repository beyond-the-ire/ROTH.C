# sdk/Makefile — regenerate + verify the ROTH.C modding SDK foundation.
#
# Self-contained: touches nothing outside sdk/ and tools/sdk_*.py. The generators
# resolve all paths from their own location, so these targets are cwd-independent.
#
#   make -C sdk check     run all offline CI gates (drift, id-invariants, abi-diff,
#                         exposure-order, compile, abi-probe, override-validate) —
#                         the gate the parent CI should call
#   make -C sdk gen       regenerate functions.json / roth_sdk.h / descriptor.json /
#                         host_adapters.c from the schemas
#   make -C sdk seed-ids  (maintenance) re-seed function_ids.json from corpus+engine.h;
#                         appends new rename aliases — review the diff before committing
#   make -C sdk release   copy the current descriptor + id table into a new
#                         sdk/abi/released/<VER>/ baseline (VER=x.y). Do this when you
#                         intentionally bump the ABI version.
#   make -C sdk all       gen + check

PY      := python3
TOOLS   := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))..)/tools
SIG     := $(TOOLS)/sdk_extract_signatures.py
MANIFEST:= $(TOOLS)/sdk_override_manifest.py
GEN     := $(TOOLS)/sdk_gen.py
IDS     := $(TOOLS)/sdk_ids.py
CHECK   := $(TOOLS)/sdk_check.py

.PHONY: all check gen seed-ids release

check:
	$(PY) $(CHECK)

gen:
	$(PY) $(SIG) --write
	$(PY) $(MANIFEST) --write
	$(PY) $(GEN) --write

seed-ids:
	$(PY) $(IDS) --seed

all: gen check

# make -C sdk release VER=0.2
release:
	@test -n "$(VER)" || (echo "usage: make -C sdk release VER=x.y"; exit 2)
	mkdir -p abi/released/$(VER)
	cp abi/descriptor.json      abi/released/$(VER)/descriptor.json
	cp schema/function_ids.json abi/released/$(VER)/function_ids.json
	@echo "wrote abi/released/$(VER)/ — remember to bump ROTH_ABI_* in sdk_api.json"
