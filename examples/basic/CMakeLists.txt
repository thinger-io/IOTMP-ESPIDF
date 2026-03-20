cmake_minimum_required(VERSION 3.16)

# EXTRA_COMPONENT_DIRS can be passed via command line:
#   idf.py build -DEXTRA_COMPONENT_DIRS="/path/to/iotmp-espidf;/path/to/iotmp-embedded"

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(thinger_iotmp_example)
