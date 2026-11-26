add_executable(test_sercom0 test_sercom0.c ../../Board/Drivers/wio_sercom0.c)
target_include_directories(test_sercom0 PRIVATE stubs ../../Board/Include)
target_compile_options(test_sercom0 PRIVATE -Wall -Wextra -Werror)
add_test(NAME sercom0_ownership COMMAND test_sercom0)
