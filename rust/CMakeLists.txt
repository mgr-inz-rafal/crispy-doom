if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CARGO_CMD cargo build)
    set(TARGET_DIR "debug")
else ()
    set(CARGO_CMD cargo build --release)
    set(TARGET_DIR "release")
endif ()

set(RUST_DOOM_SO "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_DIR}/librust.so")

add_custom_target(rustdoom ALL
    COMMENT "Compiling Rusty part of Crispy-Doom"
    COMMAND CARGO_TARGET_DIR=${CMAKE_CURRENT_BINARY_DIR} ${CARGO_CMD} 
    COMMAND cp ${RUST_DOOM_SO} ${CMAKE_CURRENT_BINARY_DIR}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(rustdoom PROPERTIES LOCATION ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME rustdoom_test 
    COMMAND cargo test
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
