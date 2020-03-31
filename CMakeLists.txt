add_library(time_sync SHARED f_time_sync.cpp ${CHANS})

target_include_directories(time_sync PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_include_directories(time_sync PUBLIC ${PROJECT_SOURCE_DIR}/channels)
install(TARGETS time_sync DESTINATION lib)
