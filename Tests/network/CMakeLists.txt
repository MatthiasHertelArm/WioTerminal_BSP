add_executable(wio_network_protocol_test
  test_network.c
  ../../Board/SDK/wio_network.c)
target_include_directories(wio_network_protocol_test PRIVATE ../../Board/Include)
target_compile_options(wio_network_protocol_test PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME network.protocol COMMAND wio_network_protocol_test)

add_executable(wio_network_uart_test
  test_network_uart.c
  ../../Board/SDK/wio_network_uart.c)
target_include_directories(wio_network_uart_test PRIVATE
  stubs ../../Board/Include "${CMSIS_DRIVER_INCLUDE_DIR}")
target_compile_options(wio_network_uart_test PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME network.uart COMMAND wio_network_uart_test)
