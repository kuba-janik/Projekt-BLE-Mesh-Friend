cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lpn_off)

target_sources(app PRIVATE
  src/main.c
  src/model_handler.c
  src/retained.c
)

zephyr_library_include_directories(.)
# net.h stosu mesh - stad bt_mesh.seq, ktore odtwarzamy po System OFF
zephyr_library_include_directories(${ZEPHYR_BASE}/subsys/bluetooth/mesh)
