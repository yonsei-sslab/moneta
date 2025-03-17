FIXUP=../../scripts/fixup.py

DIR=$(realpath $PWD/../..)

MODES=moneta

HOST?=localhost
PORT?=56841
WORKDIR?=workdir
VMCNT?=1

CONFIGS=amdgpu mali nvidia-full nvidia-nofd nvidia-nommio nvidia-norecall nvidia-nosnapshot

files := $(foreach mode,$(MODES),$(foreach config,$(CONFIGS),generated/$(mode)-$(config).cfg))

all: $(files)

generated/%.cfg: %.cfg |generated
	$(FIXUP) -i $^ -o $@ -c $(VMCNT) -t $(HOST) -p $(PORT) -w $(WORKDIR)

generated:
	mkdir -p generated

clean:
	rm -rf generated
