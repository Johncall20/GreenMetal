# libnvrm387 - nvrm_xnu.h sobre o RM r387 (NVDAUser)
#
#   make test   testes de traducao com transporte falso (roda neste Mac, sem GPU)
#   make lib    build/libnvrm387.a para x86_64 (o hackintosh)
#   make hw     build/test_hw x86_64: teste de integracao na GPU (rodar com sudo no hackintosh)

OGKM   := ../ref/ogkm
INC    := -Iinclude -I.. -I$(OGKM)/src/common/sdk/nvidia/inc -I$(OGKM)/src/nvidia/arch/nvalloc/unix/include \
          -I$(OGKM)/src/common/inc
CWARN  := -Wall -Wextra -Wno-unused-parameter
X86    := -arch x86_64 -mmacosx-version-min=26.0
BUILD  := build

.PHONY: all test lib hw clean
all: test lib hw

$(BUILD):
	mkdir -p $(BUILD)

# --- testes de traducao (nativo, transporte falso) ---
$(BUILD)/test_xlate: src/nvrm387.c tests/test_xlate.c include/*.h ../nvrm_r387.h | $(BUILD)
	clang -std=c11 -O1 -g $(CWARN) -DNVRM387_MOCK $(INC) -o $@ src/nvrm387.c tests/test_xlate.c

test: $(BUILD)/test_xlate
	./$(BUILD)/test_xlate

# --- biblioteca x86_64 ---
$(BUILD)/nvrm387.o: src/nvrm387.c include/*.h ../nvrm_r387.h | $(BUILD)
	clang -std=c11 -O2 $(CWARN) $(X86) $(INC) -c -o $@ src/nvrm387.c

$(BUILD)/libnvrm387.a: $(BUILD)/nvrm387.o
	libtool -static -o $@ $^

lib: $(BUILD)/libnvrm387.a

# --- teste de integracao na GPU ---
$(BUILD)/test_hw: tests/test_hw.c $(BUILD)/libnvrm387.a | $(BUILD)
	clang -std=c11 -O1 $(CWARN) $(X86) $(INC) -o $@ tests/test_hw.c $(BUILD)/libnvrm387.a \
	      -framework IOKit -framework CoreFoundation
	codesign -s - -f $@

hw: $(BUILD)/test_hw

clean:
	rm -rf $(BUILD)
