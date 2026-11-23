if(DEFINED ESP_PLATFORM)
    idf_component_register(
        SRCS
            "src/protocol.cpp"
            "src/packetizer.cpp"
        INCLUDE_DIRS
            "include"
        REQUIRES
            codec
    )
else()
    add_library(ecl_transport STATIC
        src/protocol.cpp
        src/packetizer.cpp
        src/net_impairment.cpp
        src/udp_transport.cpp
    )
    target_include_directories(ecl_transport PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(ecl_transport PUBLIC ecl_codec)
    target_compile_features(ecl_transport PUBLIC cxx_std_17)
endif()
