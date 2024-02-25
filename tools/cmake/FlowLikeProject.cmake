# See FlowLikeCodeGenerate.cmake: we promised to determine $PROJ_VERSION.
# Subtlety: In comments below there's some talk of ipc and ipc_* (from the Flow-IPC meta-project); formally speaking
# those are mere examples; the same would work for any "Flow-like" meta-project.  As of this writing Flow-IPC is the
# first and only such project is all.

# Parse a VERSION file.
function(parse_version_file version_file_path result_var)
  if(EXISTS "${version_file_path}")
    # Strip off any leading or trailing whitespace including newlines; then just ensure that's what left
    # is not multi-line (no newlines) nor has any other spaces inside for good measure.
    file(READ "${version_file_path}" version_content)
    string(STRIP "${version_content}" version_content)
    if(version_content MATCHES ".*[ \t\r\n].*")
      message(FATAL_ERROR "VERSION file [${version_file_path}] contains invalid characters (required: exactly a single "
                          "line with real content, and it should have no whitespace, after leading and trailing "
                          "whitespace is thrown out).")
    else()
      set(${result_var} "${version_content}" PARENT_SCOPE)
    endif()
  else()
    set(${result_var} "NOTFOUND" PARENT_SCOPE)
  endif()
endfunction()

# Try to parse VERSION file in this project's top dir (ours).
set(version_file "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
parse_version_file("${version_file}" PROJ_VERSION)

if(PROJ_VERSION STREQUAL "NOTFOUND")
  # If the VERSION file was not found in the current source directory, check FLOW_LIKE_META_ROOT, the meta-project
  # root that might have the VERSION for all sub-projects that don't define their own version.
  # As of this writing, for example, `ipc/flow/VERSION` exists, and `ipc/VERSION` exists, but `ipc_*/VERSION` do not.
  if(NOT FLOW_LIKE_META_ROOT)
    # This is not fatal error: perhaps they're (e.g.) building an ipc_* by itself, with dependencies installed
    # separately.  That's not too convenient (it's what the meta-project mechanism is for), but it is allowed.
    # TODO: At some point focus more on that use case.  As of this writing it's something we allow
    # as a forward-thinking thing, but we don't focus on it.  Ideally that shouldn't result in a WARNING and a
    # "fake" version #.
    set(PROJ_VERSION "0.0.1")
    message(WARNING "There's no ${version_file}, and we are not part of a meta-project, so nowhere to find "
                      "VERSION.  Assuming a default version number:")
    message(STATUS "Version: [${PROJ_VERSION}] (default value; no VERSION file found).")
  else()
    set(version_file "${FLOW_LIKE_META_ROOT}/VERSION")
    parse_version_file("${version_file}" PROJ_VERSION)

    if(PROJ_VERSION STREQUAL "NOTFOUND")
      message(FATAL_ERROR "VERSION file does not exist in meta-project root ${FLOW_LIKE_META_ROOT}.")
    else()
      message(STATUS "Version: [${PROJ_VERSION}] (from containing meta-project ./VERSION file).")
    endif()
  endif()
else()
  message(STATUS "Version: [${PROJ_VERSION}] (from our own ./VERSION file).")
endif()
