add_executable(arcadeduck-regtest
  regtest_host.cpp
)

target_link_libraries(arcadeduck-regtest PRIVATE core common scmversion)

add_core_resources(arcadeduck-regtest)