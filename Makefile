PROGNAME=readsb
READSB_VERSION := $(shell cat version) wiedehopf git: $(shell git describe --abbrev --dirty --always || echo nogit)

RTLSDR ?= no
BLADERF ?= no
HACKRF ?= no
PLUTOSDR ?= no
SOAPYSDR ?= no
AGGRESSIVE ?= no
HAVE_BIASTEE ?= no
TRACKS_UUID ?= no
PRINT_UUIDS ?= no
ENABLE_MQTT ?= auto

DIALECT = -std=c11
CFLAGS = $(DIALECT) -W -Wall -Werror -fno-common -O2
CFLAGS += -DMODES_READSB_VERSION=\"3.16.2\"
CFLAGS += -DREADSB_SHORT_VERSION=\"$(shell cat version)\"
CFLAGS += -DREADSB_SHORT_COMMIT=\"$(shell git describe --abbrev --dirty --always || echo nogit)\"
CFLAGS += -Wdate-time -fstack-protector-strong -Wformat -Werror=format-security

# Platform-specific settings
UNAME := $(shell uname)
ifeq ($(UNAME), Linux)
    CFLAGS += -D_GNU_SOURCE -D_DEFAULT_SOURCE
    CFLAGS += -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2
    LIBS = -pthread -lpthread -lm -lrt -lzstd
else ifeq ($(UNAME), Darwin)
    CFLAGS += -D_DARWIN_C_SOURCE
    CFLAGS += -I./compat/apple  # For epoll_shim.h
    CFLAGS += -I/opt/homebrew/include  # For zstd.h
    LDFLAGS += -L/opt/homebrew/lib
    LIBS = -pthread -lpthread -lm -lzstd
    COMPAT += compat/apple/epoll_shim.o
else
    LIBS = -pthread -lpthread -lm -lrt -lzstd
endif

ifeq ($(ZLIB_STATIC), yes)
	LIBS += -l:libz.a
else
    LIBS += -lz
endif

ifeq ($(shell $(CC) -c feature_test.c -o feature_test.o -Wno-format-truncation -Werror >/dev/null 2>&1 && echo 1 || echo 0), 1)
    CFLAGS += -Wno-format-truncation
endif

ifeq ($(shell uname -m | grep -qs -e arm -e aarch64 >/dev/null 2>&1 && echo 1 || echo 0), 1)
    CFLAGS += -DSC16Q11_TABLE_BITS=8
endif

ifeq ($(DISABLE_INTERACTIVE), yes)
    CFLAGS += -DDISABLE_INTERACTIVE
else
    ifeq ($(UNAME), Darwin)
        LIBS += $(shell pkg-config --libs ncurses 2>/dev/null || echo -lncurses)
    else
        LIBS += $(shell pkg-config --libs ncurses)
    endif
endif

# only disable workaround if zerocopy is disabled in librtlsdr, otherwise expect significantly increased CPU use
ifeq ($(DISABLE_RTLSDR_ZEROCOPY_WORKAROUND), yes)
    CFLAGS += -DDISABLE_RTLSDR_ZEROCOPY_WORKAROUND
endif

ifeq ($(LTO), yes)
	CFLAGS += -flto
	LDFLAGS += -flto
endif

ifeq ($(HISTORY), yes)
    CFLAGS += -DALL_JSON=1
endif

ifneq ($(PREAMBLE_THRESHOLD_DEFAULT),)
    CFLAGS += -DPREAMBLE_THRESHOLD_DEFAULT=$(PREAMBLE_THRESHOLD_DEFAULT)
endif

ifneq ($(TRACE_THREADS),)
    CFLAGS += -DTRACE_THREADS=$(TRACE_THREADS)
endif

ifneq ($(TRACE_RECENT_POINTS),)
    CFLAGS += -DTRACE_RECENT_POINTS=$(TRACE_RECENT_POINTS)
endif

ifneq ($(AIRCRAFT_HASH_BITS),)
    CFLAGS += -DAIRCRAFT_HASH_BITS=$(AIRCRAFT_HASH_BITS)
endif

ifeq ($(STATS_PHASE),yes)
    CFLAGS += -DSTATS_PHASE
endif

ifeq ($(TRACKS_UUID), yes)
    CFLAGS += -DTRACKS_UUID
endif

ifeq ($(PRINT_UUIDS), yes)
    CFLAGS += -DPRINT_UUIDS
endif

ifneq ($(RECENT_RECEIVER_IDS),)
    CFLAGS += -DRECENT_RECEIVER_IDS=$(RECENT_RECEIVER_IDS)
endif

ifeq ($(RTLSDR), yes)
    SDR_OBJ += sdr_rtlsdr.o
    CFLAGS += -DENABLE_RTLSDR

    ifeq ($(HAVE_BIASTEE), yes)
        CFLAGS += -DENABLE_RTLSDR_BIASTEE
    endif

    ifdef RTLSDR_PREFIX
        CFLAGS += -I$(RTLSDR_PREFIX)/include
        LDFLAGS += -L$(RTLSDR_PREFIX)/lib
    else
        CFLAGS += $(shell pkg-config --cflags librtlsdr)
        LDFLAGS += $(shell pkg-config --libs-only-L librtlsdr)
    endif

	# static linking not well supported, use at own risk
    ifeq ($(STATIC), yes)
		LIBS_SDR += -Wl,-Bstatic -lrtlsdr -Wl,-Bdynamic -lusb-1.0
    else
        LIBS_SDR += -lrtlsdr -lusb-1.0
    endif
endif

ifeq ($(BLADERF), yes)
    SDR_OBJ += sdr_bladerf.o sdr_ubladerf.o
    CFLAGS += $(shell pkg-config --cflags libbladeRF) -DENABLE_BLADERF
    LIBS_SDR += $(shell pkg-config --libs libbladeRF)
endif

ifeq ($(HACKRF), yes)
    SDR_OBJ += sdr_hackrf.o
    CFLAGS += $(shell pkg-config --cflags libhackrf) -DENABLE_HACKRF
    LIBS_SDR += $(shell pkg-config --libs libhackrf)
endif

ifeq ($(PLUTOSDR), yes)
    SDR_OBJ += sdr_plutosdr.o
    CFLAGS += $(shell pkg-config --cflags libiio libad9361) -DENABLE_PLUTOSDR
    LIBS_SDR += $(shell pkg-config --libs libiio libad9361)
endif

ifeq ($(SOAPYSDR), yes)
    SDR_OBJ += sdr_soapy.o
    CFLAGS += $(shell pkg-config --cflags SoapySDR) -DENABLE_SOAPYSDR
    LIBS_SDR += $(shell pkg-config --libs SoapySDR)
endif

# MQTT support detection and configuration
ifeq ($(ENABLE_MQTT), auto)
    # Auto-detect libmosquitto availability
    MQTT_AVAILABLE := $(shell pkg-config --exists libmosquitto 2>/dev/null && echo yes || echo no)
    ifeq ($(MQTT_AVAILABLE), yes)
        ENABLE_MQTT := yes
        $(info MQTT support: auto-detected libmosquitto, enabling MQTT features)
    else
        ENABLE_MQTT := no
        $(info MQTT support: libmosquitto not found, compiling without MQTT support)
    endif
else ifeq ($(ENABLE_MQTT), yes)
    # Explicitly enabled - require libmosquitto
    MQTT_AVAILABLE := $(shell pkg-config --exists libmosquitto 2>/dev/null && echo yes || echo no)
    ifneq ($(MQTT_AVAILABLE), yes)
        $(error MQTT support explicitly enabled but libmosquitto not found. Install libmosquitto-dev or set ENABLE_MQTT=no)
    endif
    $(info MQTT support: explicitly enabled)
else
    # Explicitly disabled
    $(info MQTT support: explicitly disabled)
endif

# Watchlist support (can be enabled independently of MQTT)
ifeq ($(ENABLE_WATCHLIST), yes)
    CFLAGS += -DENABLE_WATCHLIST
    WATCHLIST_OBJ = watchlist.o alert_publisher.o
    $(info Watchlist support: enabled)
else
    WATCHLIST_OBJ =
endif

# MQTT support (requires watchlist)
ifeq ($(ENABLE_MQTT), yes)
    ifeq ($(ENABLE_WATCHLIST), yes)
        CFLAGS += -DENABLE_MQTT
        CFLAGS += $(shell pkg-config --cflags libmosquitto)
        LIBS += $(shell pkg-config --libs libmosquitto)
        MQTT_OBJ = mqtt_client.o
        $(info MQTT support: enabled with watchlist)
    else
        $(error MQTT support requires ENABLE_WATCHLIST=yes)
    endif
else
    MQTT_OBJ =
endif

# Combined object files
WATCHLIST_MQTT_OBJ = $(WATCHLIST_OBJ) $(MQTT_OBJ)

# add custom overrides if user defines them
CFLAGS += -g $(OPTIMIZE)

all: readsb viewadsb

ifneq ($(shell cat .version 2>/dev/null),prefix $(READSB_VERSION))
.PHONY: .version
.version:
	@(echo 'prefix $(READSB_VERSION)' >.version &)
endif

readsb.o: readsb.c *.h .version
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c *.h
	$(CC) $(CFLAGS) -c $< -o $@

readsb: readsb.o argp.o anet.o interactive.o mode_ac.o mode_s.o comm_b.o json_out.o net_io.o crc.o demod_2400.o \
	uat2esnt/uat2esnt.o uat2esnt/uat_decode.o \
	stats.o cpr.o icao_filter.o track.o util.o fasthash.o convert.o sdr_ifile.o sdr_beast.o sdr.o ais_charset.o \
	globe_index.o geomag.o receiver.o aircraft.o api.o threadpool.o \
	$(SDR_OBJ) $(WATCHLIST_MQTT_OBJ) $(COMPAT)
	$(CC) -o $@ $^ $(LDFLAGS) $(LIBS) $(LIBS_SDR) $(OPTIMIZE)

viewadsb: readsb
	rm -f viewadsb
	cp readsb viewadsb

clean:
	rm -f *.o uat2esnt/*.o compat/clock_gettime/*.o compat/clock_nanosleep/*.o compat/apple/*.o readsb viewadsb cprtests crctests convert_benchmark

test: cprtest crctest

cprtest: cprtests
	./cprtests

crctest: cprtests
	./cprtests

cprtests: cpr.o cprtests.o
	$(CC) $(CFLAGS) -o $@ $^ -lm

crctests: crc.c crc.h
	$(CC) $(CFLAGS) -DCRCDEBUG -o $@ $<

benchmarks: oneoff/convert_benchmark
	./convert_benchmark

oneoff/convert_benchmark: oneoff/convert_benchmark.o convert.o util.o
	$(CC) $(CFLAGS) -o $@ $^ -lm

oneoff/decode_comm_b: oneoff/decode_comm_b.o comm_b.o ais_charset.o
	$(CC) $(CFLAGS) -o $@ $^ -lm
