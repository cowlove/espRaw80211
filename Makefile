BOARD ?= esp32
# The generated empirical CSIM model owns the default simulated fleet size.
CONTEXT_COUNT ?= $(shell sed -n 's/.*boardCount = \([0-9][0-9]*\).*/\1/p' csimPairwiseData.h)

ifneq ($(filter esp32 esp32s3,$(BOARD)),)
CHIP=esp32
BUILD_MEMORY_TYPE=qio_qspi
BUILD_EXTRA_FLAGS += -DI2S
ALIBS=${HOME}/Arduino/libraries
EXCLUDE_DIRS=${ALIBS}/lvgl|${ALIBS}/LovyanGFX|${ALIBS}/U8g2|${ALIBS}/esp32csim|${ALIBS}/PubSubClient/tests
GIT_VERSION := "$(shell git describe --abbrev=4 --dirty --always --tags)"
BUILD_EXTRA_FLAGS += -DGIT_VERSION=\"$(GIT_VERSION)\"
BUILD_EXTRA_FLAGS += -DESP32CORE_V2

ifeq ($(BOARD),esp32)
OTA_ADDR=192.168.68.118
else ifeq ($(BOARD),esp32s3)
# ESP32-S3 boards use native USB CDC and the compact partition layout.
UPLOAD_PORT ?= /dev/ttyACM0
PART_FILE=${ESP_ROOT}/tools/partitions/min_spiffs.csv
CDC_ON_BOOT=1
endif

include ${HOME}/Arduino/libraries/makeEspArduino/makeEspArduino.mk

.PHONY: hardware-upload upload-only fixtty cat uc

hardware-upload: upload

# The deployment helper builds once before launching concurrent board uploads.
# This target only reads the completed artifacts, avoiding parallel make jobs
# racing while producing the shared build outputs.
upload-only:
	$(CHECK_PORT)
	$(UPLOAD_COM)

fixtty:
	stty -F ${UPLOAD_PORT} -hupcl -crtscts -echo raw 115200

cat: fixtty
	cat ${UPLOAD_PORT}

uc: hardware-upload cat

else

ALIBS=${HOME}/Arduino/libraries
CSIM_LIBS=Arduino_CRC32 ArduinoJson Adafruit_HX711 esp32jimlib esp32csim
CSIM_SRC_DIRS=$(foreach L,$(CSIM_LIBS),${ALIBS}/${L}/src)
CSIM_SRC_DIRS+=$(foreach L,$(CSIM_LIBS),${ALIBS}/${L})
CSIM_SRC_DIRS+=$(foreach L,$(CSIM_LIBS),${ALIBS}/${L}/src/csim_include)
CSIM_SRCS=$(foreach DIR,$(CSIM_SRC_DIRS),$(wildcard $(DIR)/*.cpp))
CSIM_BUILD_DIR=./build/csim
CSIM_OBJS=$(foreach S,$(notdir $(CSIM_SRCS)),$(CSIM_BUILD_DIR)/$(S:.cpp=.o))
CSIM_INC=$(foreach DIR,$(CSIM_SRC_DIRS),-I${DIR})
CSIM_CFLAGS=-g -O2 -MMD -fpermissive -DESP32 -DCSIM -DUBUNTU \
	-DCONTEXT_COUNT=$(CONTEXT_COUNT)
VPATH=$(sort $(dir $(CSIM_SRCS)))

espRaw80211_csim: ${CSIM_OBJS} ${CSIM_BUILD_DIR}/espRaw80211.o | ${CSIM_BUILD_DIR}
	g++ -g ${CSIM_CFLAGS} ${CSIM_OBJS} ${CSIM_BUILD_DIR}/espRaw80211.o -o $@

${CSIM_BUILD_DIR}/%.o: %.cpp | ${CSIM_BUILD_DIR}
	g++ ${CSIM_CFLAGS} -x c++ -c ${CSIM_INC} $< -o $@

${CSIM_BUILD_DIR}/%.o: %.ino | ${CSIM_BUILD_DIR}
	g++ ${CSIM_CFLAGS} -x c++ -c ${CSIM_INC} $< -o $@

${CSIM_BUILD_DIR}:
	mkdir -p $@

.PHONY: clean clear-state depend

clear-state:
	rm -rf csim-fs csim_rtc.bin csim_context_sleep.txt

depend: ${CSIM_OBJS} ${CSIM_BUILD_DIR}/espRaw80211.o

clean:
	rm -rf ${CSIM_BUILD_DIR} espRaw80211_csim

-include ${CSIM_BUILD_DIR}/*.d

endif
