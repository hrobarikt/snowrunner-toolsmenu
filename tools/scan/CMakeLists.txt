add_executable(srtm-scan main.cpp)
target_link_libraries(srtm-scan PRIVATE srtm_scanner)
