# Flow
# Copyright 2023 Akamai Technologies, Inc.
#
# Licensed under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in
# compliance with the License.  You may obtain a copy
# of the License at
#
#   https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in
# writing, software distributed under the License is
# distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
# CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing
# permissions and limitations under the License.

# Make a couple binaries in very similar ways; call this for each.
function(handle_binary name_sh long_name) # name_sh is "cli"/"client" or "srv"/"server"; as in echo/cli or echo/srv.
  set(name "net_flow_echo_${name_sh}.exec")
  add_executable(${name} "${name_sh}/echo_${long_name}.cpp")

  # Do stuff we've resolved to do on all our targets.
  common_set_target_properties(${name})

  # Link good ol' libflow.
  target_link_libraries(${name} PRIVATE ${PROJ})

  # Might as well export if they `make install` or equivalent.
  install(TARGETS ${name}
          RUNTIME DESTINATION bin)
endfunction()

handle_binary(cli client)
handle_binary(srv server)
