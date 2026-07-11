# kestrel/bench/Makefile
CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
# Pin the bundled libzstd 1.5.7 if present; else system zstd. Override on the box:
#   make WITH_QAT=1 WITH_IAA=1 ZSTD_PREFIX=/path/to/zstd
ZSTD_PREFIX ?=
LDLIBS := -lzstd -ldeflate
INCS :=
ifneq ($(ZSTD_PREFIX),)
  INCS += -I$(ZSTD_PREFIX)/include
  LDLIBS := -L$(ZSTD_PREFIX)/lib $(LDLIBS)
endif
SRC := codec_sw.c corpus.c metrics.c
ifdef WITH_QAT
  CFLAGS += -DWITH_QAT
  SRC += codec_qat.c
  LDLIBS += -lqatseqprod -lqatzip -lqat -lusdm
endif
ifdef WITH_IAA
  CFLAGS += -DWITH_IAA
  SRC += codec_iaa.c
  LDLIBS += -lqpl -laccel-config
endif

bench: main.c $(SRC)
	$(CC) $(CFLAGS) $(INCS) -o $@ $^ $(LDLIBS)
selftest: selftest.c codec_sw.c $(if $(WITH_QAT),codec_qat.c) $(if $(WITH_IAA),codec_iaa.c)
	$(CC) $(CFLAGS) $(INCS) -o $@ $^ $(LDLIBS)
clean:
	rm -f bench selftest
.PHONY: clean
