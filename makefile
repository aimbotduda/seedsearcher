#CC      = gcc
#AR      = ar
ARFLAGS = cr
override LDFLAGS = -lm
override CFLAGS += -Wall -Wextra -fwrapv

ifeq ($(OS),Windows_NT)
	override CFLAGS += -D_WIN32
	CC = gcc
	RM = del
else
	override LDFLAGS += -pthread
	#RM = rm
endif

.PHONY : all debug release native libcubiomes clean

all: release

debug: CFLAGS += -DDEBUG -O0 -ggdb3
debug: libcubiomes
ifeq ($(OS),Windows_NT)
release: CFLAGS += -O3
release: libcubiomes
native: CFLAGS += -O3 -march=native -ffast-math
native: libcubiomes
else
release: CFLAGS += -O3 -flto -fPIC
release: LDFLAGS += -flto
release: libcubiomes
native: CFLAGS += -O3 -march=native -ffast-math -flto
native: LDFLAGS += -flto
native: libcubiomes
#debug: CFLAGS += -fsanitize=undefined
endif


libcubiomes: noise.o biomes.o layers.o biomenoise.o generator.o finders.o util.o quadbase.o
	$(AR) $(ARFLAGS) libcubiomes.a $^

finders.o: finders.c finders.h
	$(CC) -c $(CFLAGS) $<

generator.o: generator.c generator.h
	$(CC) -c $(CFLAGS) $<

biomenoise.o: biomenoise.c
	$(CC) -c $(CFLAGS) $<

biometree.o: biometree.c
	$(CC) -c $(CFLAGS) $<

layers.o: layers.c layers.h
	$(CC) -c $(CFLAGS) $<

biomes.o: biomes.c biomes.h
	$(CC) -c $(CFLAGS) $<

noise.o: noise.c noise.h
	$(CC) -c $(CFLAGS) $<

util.o: util.c util.h
	$(CC) -c $(CFLAGS) $<

quadbase.o: quadbase.c quadbase.h
	$(CC) -c $(CFLAGS) $<

clean:
	$(RM) *.o *.a

# Build the hutfinder executable against the static library
.PHONY: hutfinder
hutfinder: release libcubiomes hutfinder.c
	$(CC) $(CFLAGS) -o hutfinder hutfinder.c libcubiomes.a $(LDFLAGS)

# Build the structure_finder executable against the static library
.PHONY: structure_finder
structure_finder: release libcubiomes structure_finder.c
	$(CC) $(CFLAGS) -o structure_finder structure_finder.c libcubiomes.a $(LDFLAGS)

# Build the groupfinder executable (standalone, doesn't need cubiomes)
.PHONY: groupfinder
groupfinder:
	$(MAKE) -C findgroups
