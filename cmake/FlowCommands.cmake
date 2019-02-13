macro(actor_set varname srcs)
  set(${varname})
  foreach(src ${srcs})
    set(tmp "${src}")
    if(${src} MATCHES ".*\\.h")
      continue()
    elseif(${src} MATCHES ".*\\.actor\\.cpp")
      string(REPLACE ".actor.cpp" ".actor.g.cpp" tmp ${src})
      set(tmp "${CMAKE_CURRENT_BINARY_DIR}/${tmp}")
    endif()
    set(${varname} "${${varname}};${tmp}")
  endforeach()
endmacro()

set(ACTOR_TARGET_COUNTER "0")
macro(actor_compile target srcs)
  set(options DISABLE_ACTOR_WITHOUT_WAIT_WARNING)
  set(oneValueArg)
  set(multiValueArgs)
  cmake_parse_arguments(ACTOR_COMPILE "${options}" "${oneValueArgs}" "${multiValueArgs}" "${ARGN}")
  set(_tmp_out "")
  foreach(src ${srcs})
    set(tmp "")
    if(${src} MATCHES ".*\\.actor\\.h")
      string(REPLACE ".actor.h" ".actor.g.h" tmp ${src})
    elseif(${src} MATCHES ".*\\.actor\\.cpp")
      string(REPLACE ".actor.cpp" ".actor.g.cpp" tmp ${src})
    endif()
    set(actor_compiler_flags "")
    if(ACTOR_COMPILE_DISABLE_ACTOR_WITHOUT_WAIT_WARNING)
      set(actor_compiler_flags "--disable-actor-without-wait-error")
    endif()
    if(tmp)
      if(WIN32)
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${tmp}"
          COMMAND $<TARGET_FILE:actorcompiler> "${CMAKE_CURRENT_SOURCE_DIR}/${src}" "${CMAKE_CURRENT_BINARY_DIR}/${tmp}" ${actor_compiler_flags}
          DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${src}" actorcompiler ${actor_exe}
          COMMENT "Compile actor: ${src}")
      else()
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${tmp}"
          COMMAND ${MONO_EXECUTABLE} ${actor_exe} "${CMAKE_CURRENT_SOURCE_DIR}/${src}" "${CMAKE_CURRENT_BINARY_DIR}/${tmp}" ${actor_compiler_flags} > /dev/null
          DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${src}" actorcompiler ${actor_exe}
          COMMENT "Compile actor: ${src}")
      endif()
      set(_tmp_out "${_tmp_out};${CMAKE_CURRENT_BINARY_DIR}/${tmp}")
    endif()
  endforeach()
  MATH(EXPR ACTOR_TARGET_COUNTER "${ACTOR_TARGET_COUNTER}+1")
  add_custom_target(${target}_actors_${ACTOR_TARGET_COUNTER} DEPENDS ${_tmp_out})
  add_dependencies(${target} ${target}_actors_${ACTOR_TARGET_COUNTER})
  target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
endmacro()
