cmake_minimum_required(VERSION 3.22)

project(ahura_kernel C)

# Allow manual override: -DOS_ARCH_VARIANT=cortex_m4 (for example)
set(OS_ARCH_VARIANT "" CACHE STRING "ARM Cortex arch folder name under ahura_kernel/arch/arm")

if(OS_ARCH_VARIANT STREQUAL "")
	set(_os_arch_candidates "")

	string(REGEX MATCH "-mcpu=([^ ]+)" _os_mcpu_match "${CMAKE_C_FLAGS}")
	if(CMAKE_MATCH_1)
		string(TOLOWER "${CMAKE_MATCH_1}" _os_mcpu)
		# Drop feature suffixes such as +nofp or +nodsp before the folder lookup.
		string(REGEX REPLACE "\\+.*" "" _os_mcpu "${_os_mcpu}")
		string(REPLACE "-" "_" _os_mcpu "${_os_mcpu}")
		list(APPEND _os_arch_candidates "${_os_mcpu}")
	endif()

	if(DEFINED CMAKE_SYSTEM_PROCESSOR AND NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "")
		string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _os_proc)
		string(REPLACE "-" "_" _os_proc "${_os_proc}")
		list(APPEND _os_arch_candidates "${_os_proc}")
	endif()

	list(APPEND _os_arch_candidates "cortex_m33")

	foreach(_os_candidate IN LISTS _os_arch_candidates)
		if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/arch/arm/${_os_candidate}/os_arch_port.c")
			set(OS_ARCH_VARIANT "${_os_candidate}")
			break()
		endif()
	endforeach()
endif()

set(OS_ARCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/arch/arm/${OS_ARCH_VARIANT}")

if(NOT EXISTS "${OS_ARCH_DIR}/os_arch_port.c")
	message(FATAL_ERROR "Unsupported OS_ARCH_VARIANT='${OS_ARCH_VARIANT}'. Missing ${OS_ARCH_DIR}/os_arch_port.c")
endif()

message(STATUS "Ahura kernel arch: ${OS_ARCH_VARIANT}")

add_library(ahura_kernel STATIC
	arch/arm/${OS_ARCH_VARIANT}/os_arch_port.c
	core/critical.c
	core/delay.c
	core/event.c
	core/kernel.c
	core/list.c
	core/memory_pool.c
	core/mutex.c
	core/queue.c
	core/semaphore.c
	core/task.c
	core/tick.c
	core/timer.c
	core/work.c
)

target_include_directories(ahura_kernel PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${OS_ARCH_DIR}
)
