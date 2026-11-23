if(DEFINED ESP_PLATFORM)
    idf_component_register(
        SRCS
            "src/jpeg_parser.cpp"
            "src/jpeg_writer.cpp"
            "src/temporal_codec.cpp"
        INCLUDE_DIRS
            "include"
    )
else()
    add_library(ecl_codec STATIC
        src/jpeg_parser.cpp
        src/jpeg_writer.cpp
        src/temporal_codec.cpp
    )
    target_include_directories(ecl_codec PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_features(ecl_codec PUBLIC cxx_std_17)
    if(ECL_ENABLE_ZLIB AND ZLIB_FOUND)
        target_compile_definitions(ecl_codec PUBLIC ECL_ENABLE_ZLIB=1)
        target_link_libraries(ecl_codec PUBLIC ZLIB::ZLIB)
    endif()
endif()
