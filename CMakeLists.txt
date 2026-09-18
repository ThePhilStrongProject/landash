# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

#
# Enlarge the lwIP ARP cache, project-wide.
#
# The scanner reads MAC addresses out of this table, and lwIP's default of 10
# entries is far too small for a subnet sweep. Worse, when the table is full
# lwIP recycles the oldest *stable* entry before any pending one, so every
# probe to an address that does not answer can evict a host the scanner has
# already resolved but not yet read. At the default size a /24 sweep found
# only a handful of the live hosts.
#
# ARP_TABLE_SIZE is a plain lwIP option guarded by "#if !defined", so setting
# it here overrides opt.h. 64 entries costs roughly 1.5 KB of RAM.
#
idf_build_set_property(COMPILE_DEFINITIONS "ARP_TABLE_SIZE=64" APPEND)

project(netdash)
