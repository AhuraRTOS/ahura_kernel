cmake_minimum_required(VERSION 3.22)

# Ahura kernel self-test suite: a small static library so any application
# that already links ahura_kernel can add it. It supplies the strong
# override of the weak os_test() (declared in ahura.h, empty default in
# os_kernel.c) - once linked in, os_init() automatically creates and starts
# a task that runs it, when OS_CONFIG_TEST_ENABLE is 1. No header here: the
# only symbol this library exports (os_test) is already declared in
# ahura.h. Independent from the top-level project: link this anywhere
# ahura_kernel is available to run the suite on that target/device.

add_library(os_test STATIC
	os_test.c
)

target_link_libraries(os_test PUBLIC
	ahura_kernel
)
