# - Try to find the LDAP client libraries
# Once done this will define
#
#  LDAP_FOUND - system has libldap
#  LDAP_INCLUDE_DIRS - the ldap include directory
#  LDAP_LIBRARIES - libldap + liblber (if found) library
#  LBER_LIBRARIES - liblber library

if(LDAP_INCLUDE_DIRS AND LDAP_LIBRARIES)
  # Already in cache, be silent
  set(Ldap_FIND_QUIETLY TRUE)
endif(LDAP_INCLUDE_DIRS AND LDAP_LIBRARIES)

# Support preference of static libs by adjusting CMAKE_FIND_LIBRARY_SUFFIXES
if(BUILD_STATIC)
  set(_openldap_ORIG_CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES})
  set(CMAKE_FIND_LIBRARY_SUFFIXES .a )
endif()


if (NOT APPLE)
    FIND_PATH(LDAP_INCLUDE_DIRS ldap.h)
    FIND_LIBRARY(LDAP_LIBRARIES NAMES ldap)
    FIND_LIBRARY(LBER_LIBRARIES NAMES lber)
    FIND_LIBRARY(RESOLV_LIBRARIES NAMES resolv)
else ()
    FIND_PATH(LDAP_INCLUDE_DIRS ldap.h PATHS
        /usr/include
        /opt/local/include
        /usr/local/include
        /usr/local/opt/openldap/include
        NO_CMAKE_SYSTEM_PATH)
    FIND_LIBRARY(LDAP_LIBRARIES NAMES ldap PATHS
        /usr/local/opt/openldap/lib
        /usr/local/lib
        NO_CMAKE_SYSTEM_PATH)
    FIND_LIBRARY(LBER_LIBRARIES NAMES lber PATHS
        /usr/local/opt/openldap/lib
        /usr/local/lib
        NO_CMAKE_SYSTEM_PATH)
FIND_LIBRARY(RESOLV_LIBRARIES NAMES resolv PATHS
        /usr/local/opt/openldap/lib
        /usr/local/lib
        NO_CMAKE_SYSTEM_PATH)
endif ()

if(LDAP_INCLUDE_DIRS AND LDAP_LIBRARIES)
  set(LDAP_FOUND TRUE)
  if(LBER_LIBRARIES)
    set(LDAP_LIBRARIES ${LDAP_LIBRARIES} ${LBER_LIBRARIES} ${RESOLV_LIBRARIES})
  endif(LBER_LIBRARIES)
endif(LDAP_INCLUDE_DIRS AND LDAP_LIBRARIES)

if(LDAP_FOUND)
  if(NOT Ldap_FIND_QUIETLY)
    message(STATUS "LDAP include dir: ${LDAP_INCLUDE_DIRS}")
    message(STATUS "LDAP libraries: ${LDAP_LIBRARIES}")
  endif(NOT Ldap_FIND_QUIETLY)
else(LDAP_FOUND)
  if (Ldap_FIND_REQUIRED)
    message(FATAL_ERROR "Could NOT find ldap")
  endif (Ldap_FIND_REQUIRED)
endif(LDAP_FOUND)

MARK_AS_ADVANCED(LDAP_INCLUDE_DIRS LDAP_LIBRARIES LBER_LIBRARIES RESOLV_LIBRARIES)

# Restore the original find library ordering
if(BUILD_STATIC)
  set(CMAKE_FIND_LIBRARY_SUFFIXES ${_openldap_ORIG_CMAKE_FIND_LIBRARY_SUFFIXES})
endif()
