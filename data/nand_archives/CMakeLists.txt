# This directory contains pre-built NCCH archives that provide freely licensed
# alternatives to system archives not included in game update partitions.
#
# The build scripts are located at https://github.com/mikage-emu/conan-3ds/tree/mikage/packages/citra_system_archives,
# which is based on https://github.com/B3n30/citra_system_archives.

set(TITLES
    "0004009b/00010202"
    "0004009b/00010402"
    "0004009b/00014002"
    "000400db/00010302"
    "000400db/00016102")

foreach(TITLE ${TITLES})
    configure_file(${TITLE}/content/00000000.app ${CMAKE_BINARY_DIR}/data/${TITLE}/content/00000000.cxi COPYONLY)
endforeach()
