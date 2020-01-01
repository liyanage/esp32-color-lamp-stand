#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#

#used for memmem
CFLAGS := -D__GNU_VISIBLE

PROJECT_NAME := esp32-color-lamp-stand

include $(IDF_PATH)/make/project.mk

