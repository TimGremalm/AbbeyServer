# make
# make flash
# make test
# make test ESPPORT=/dev/ttyUSB0
PROGRAM=abbeyserver
EXTRA_COMPONENTS = extras/paho_mqtt_c extras/pwm
#include ../../common.mk
include $(espopenrtos)

