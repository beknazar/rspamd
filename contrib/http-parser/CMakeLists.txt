SET(HTTPSRC            http_parser.c)

if ("${CMAKE_C_COMPILER_ID}" STREQUAL "Clang" OR "${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3")
endif ()
ADD_LIBRARY(rspamd-http-parser ${LINK_TYPE} ${HTTPSRC})
SET_TARGET_PROPERTIES(rspamd-http-parser PROPERTIES VERSION ${RSPAMD_VERSION})
SET_TARGET_PROPERTIES(rspamd-http-parser PROPERTIES COMPILE_FLAGS "-DRSPAMD_LIB")

IF(NO_SHARED MATCHES "OFF")
    INSTALL(TARGETS rspamd-http-parser
        LIBRARY DESTINATION ${LIBDIR}
        PUBLIC_HEADER DESTINATION ${INCLUDEDIR})
ENDIF(NO_SHARED MATCHES "OFF")  