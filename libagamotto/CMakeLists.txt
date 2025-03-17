project(libagamotto)

file(GLOB agamotto_SRC
  "src/*.c"
)

add_library(agamotto
  SHARED
  ${agamotto_SRC}
)

set(COMMON_FLAGS "-D_GNU_SOURCE")
set(COMMON_FLAGS "${COMMON_FLAGS} -Wall -fPIC -fno-strict-aliasing")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COMMON_FLAGS}")

target_link_libraries(agamotto PUBLIC ${CMAKE_DL_LIBS} pthread)

message(STATUS ${CMAKE_CURRENT_SOURCE_DIR})

target_include_directories(agamotto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)