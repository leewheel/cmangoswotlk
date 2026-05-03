#
# This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# disable warnings for dependencies
DisableWarnings()


# overwrite target creation macro to add dependencies folder property
set(SET_DEPENDENCY_PROP TRUE)
macro(add_library _target)
    _add_library(${_target} ${ARGN})
    if (SET_DEPENDENCY_PROP)
        get_target_property(originalTarget ${_target} ALIASED_TARGET)
        if (${originalTarget} STREQUAL "originalTarget-NOTFOUND")
          if(${CMAKE_VERSION} VERSION_GREATER_EQUAL "3.19.0")
            set_target_properties(${_target} PROPERTIES FOLDER "Dependencies")
          else()
            get_target_property(link_type ${_target} TYPE)
            if (NOT link_type STREQUAL "INTERFACE_LIBRARY")
              set_target_properties(${_target} PROPERTIES FOLDER "Dependencies")
            endif()
          endif()
        endif()
    endif()
endmacro()
macro(add_executable _target)
    _add_executable(${_target} ${ARGN})
    if (SET_DEPENDENCY_PROP)
        get_target_property(originalTarget ${_target} ALIASED_TARGET)
        if (${originalTarget} STREQUAL "originalTarget-NOTFOUND")
          if(${CMAKE_VERSION} VERSION_GREATER_EQUAL "3.19.0")
            set_target_properties(${_target} PROPERTIES FOLDER "Dependencies")
          else()
            get_target_property(link_type ${_target} TYPE)
            if (NOT link_type STREQUAL "INTERFACE_LIBRARY")
              set_target_properties(${_target} PROPERTIES FOLDER "Dependencies")
            endif()
          endif()
        endif()
    endif()
endmacro()
macro(add_custom_target _target)
    _add_custom_target(${_target} ${ARGN})
    if (SET_DEPENDENCY_PROP)
        get_target_property(originalTarget ${_target} ALIASED_TARGET)
        if (${originalTarget} STREQUAL "originalTarget-NOTFOUND")
          if(${CMAKE_VERSION} VERSION_GREATER_EQUAL "3.19.0")
            set_target_properties(${_target} PROPERTIES FOLDER "Dependencies")
          else()
            get_target_property(link_type ${_target} TYPE)
            if (NOT link_type STREQUAL "INTERFACE_LIBRARY")
              set_target_properties(${_target} PROPERTIES FOLDER "Dependencies")
            endif()
          endif()
        endif()
    endif()
endmacro()
add_subdirectory(recastnavigation)
add_subdirectory(src)
add_subdirectory(lualib)

if(BUILD_CHATLLM)
  # llama.cpp - LLM inference library (auto-detect Vulkan SDK for GPU)
  set(LLAMA_BUILD_COMMON ON CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

  find_package(Vulkan QUIET)
  if(Vulkan_FOUND)
    set(GGML_VULKAN ON CACHE BOOL "Vulkan GPU backend" FORCE)
    message(STATUS "Vulkan SDK found, enabling GPU acceleration")
  else()
    set(GGML_VULKAN OFF CACHE BOOL "Vulkan GPU backend" FORCE)
    message(STATUS "Vulkan SDK not found, building CPU-only")
  endif()

  message(STATUS "Building llama.cpp as static library")
  add_subdirectory(llama.cpp)

  # Fix MSVC C++20 char8_t conflict (llama needs /Zc:char8_t- but llama-common/json needs char8_t)
  if(MSVC)
    target_compile_options(llama        PRIVATE /Zc:char8_t-)
    target_compile_options(ggml         PRIVATE /Zc:char8_t-)
    target_compile_definitions(llama    PRIVATE NOMINMAX)
    target_compile_definitions(ggml     PRIVATE NOMINMAX)
  endif()
endif()
if (BUILD_EXTRACTORS)
  add_subdirectory(libmpq)
endif()

if (BUILD_GAME_SERVER OR BUILD_LOGIN_SERVER OR BUILD_EXTRACTORS)
  add_subdirectory(g3dlite)
endif()

set(SET_DEPENDENCY_PROP FALSE)