UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
  # k.h symbols (ktn, krr, ...) resolve from the host q at load time
  LDSHARED := -bundle -undefined dynamic_lookup
else
  LDSHARED := -shared
endif

CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter
BASE   := -std=c11 -fPIC -D_DEFAULT_SOURCE

all: qgeos.so qgdal.so

qgeos.so: qgeos.c k.h
	$(CC) $(BASE) $(CFLAGS) $(shell geos-config --cflags) $(LDSHARED) -o $@ qgeos.c $(shell geos-config --clibs)

qgdal.so: qgdal.c k.h
	$(CC) $(BASE) $(CFLAGS) $(shell gdal-config --cflags) $(LDSHARED) -o $@ qgdal.c $(shell gdal-config --libs) -lm

test: all
	$(Q) test.q
	$(Q) test_gdal.q

clean:
	rm -f qgeos.so qgdal.so

.PHONY: all test clean
