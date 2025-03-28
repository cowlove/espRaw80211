BOARD=esp32
#VERBOSE=1::
CHIP=esp32
OTA_ADDR=192.168.68.118

ifeq ($(BOARD),esp32s3)
	CDC_ON_BOOT = 1
	UPLOAD_PORT ?= /dev/ttyACM0
else 
	BUILD_EXTRA_FLAGS += -DI2S
endif

#GIT_VERSION := "$(shell git describe --abbrev=4 --dirty --always --tags)"
BUILD_EXTRA_FLAGS += -DGIT_VERSION=\"$(GIT_VERSION)\"
BUILD_EXTRA_FLAGS += -DESP32CORE_V2
include ${HOME}/Arduino/libraries/makeEspArduino/makeEspArduino.mk


.PHONY: ${MAIN_NAME}_csim

csim: ${MAIN_NAME}_csim
	cp $< $@

${MAIN_NAME}_csim:  
	g++ -x c++ -g ${MAIN_NAME}.ino -o $@ -DESP32 -DCSIM -DUBUNTU \
	-I./ -I${HOME}/Arduino/lib -I ${HOME}/Arduino/libraries/esp32jimlib/src/ 


csim:	${MAIN_NAME}_csim

fixtty:
	stty -F ${UPLOAD_PORT} -hupcl -crtscts -echo raw 115200

cat:    fixtty
	cat ${UPLOAD_PORT}


socat:  
	socat udp-recvfrom:9000,fork - 
mocat:
	mosquitto_sub -h rp1.local -t "${MAIN_NAME}/#" -F "%I %t %p"   

uc:
	make upload && make cat


backtrace:
	tr ' ' '\n' | /home/jim/.arduino15/packages/esp32/tools/xtensa-esp32-elf-gcc/*/bin/xtensa-esp32-elf-addr2line -f -i -e /tmp/mkESP/winglevlr_ttgo-t1/*.elf
        


