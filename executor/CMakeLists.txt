file(GLOB executor_C_SRC
    "*.c"
)

foreach(executor_C ${executor_C_SRC})
    get_filename_component(executor_C ${executor_C} NAME)
    string(REPLACE "executor" "syz-executor.debug" syz_executor_C ${executor_C})
    string(REPLACE ".c" "" syz_executor_NOEXT ${syz_executor_C})
    add_executable(${syz_executor_NOEXT} ${executor_C})
    message(${CMAKE_CURRENT_SOURCE_DIR})
    target_include_directories(
        ${syz_executor_NOEXT}
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../../
    )
    target_link_libraries(${syz_executor_NOEXT} rt)
endforeach()