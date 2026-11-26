#!/usr/bin/env bash
# Version: 3.1
# Date: 2024-04-17
# This bash script generates a CMSIS Software Pack:
#

set -o pipefail

# Keep the source-file guard independent of the gen-pack library working directory.
WIO_PACK_SOURCE_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)" || exit 1
readonly WIO_PACK_SOURCE_DIR

# Set version of gen pack library
# For available versions see https://github.com/Open-CMSIS-Pack/gen-pack/tags.
# Use the tag name without the prefix "v", e.g., 0.7.0
REQUIRED_GEN_PACK_LIB="0.14.0"

# Set default command line arguments
DEFAULT_ARGS=()

# Pack warehouse directory - destination
# Default: ./output
#
# PACK_OUTPUT=./output

# Temporary pack build directory,
# Default: ./build
#
# PACK_BUILD=./build

# Specify directory names to be added to pack base directory
# An empty list defaults to all folders next to this script.
# Default: empty (all folders)
#
PACK_DIRS="
  Board
  Documents
  Examples
  Flash
  Middleware
  Tests
  .knowledge
"

# Specify file names to be added to pack base directory
# Default: empty
#
PACK_BASE_FILES="
  LICENSE
"

# Specify file names to be deleted from pack build directory
# Default: empty
#
PACK_DELETE_FILES="
  Examples/Blinky/out
  Examples/Blinky/tmp
  Examples/Blinky/.cmsis
  Examples/Blinky/.vscode
  Examples/Blinky/.clangd
  Examples/Blinky/*.cbuild-idx.yml
  Examples/Blinky/*.cbuild-pack.yml
  Examples/Blinky/*.cbuild-set.yml
  Examples/Blinky/*.cbuild-run.yml
  Examples/Blinky/*.cbuild.yml
  Examples/Blinky/RTE/AC6/_*
  Examples/Blinky/RTE/GCC/_*
  Examples/HardwareTest/out
  Examples/HardwareTest/tmp
  Examples/HardwareTest/.cmsis
  Examples/HardwareTest/.vscode
  Examples/HardwareTest/.clangd
  Examples/HardwareTest/*.cbuild-idx.yml
  Examples/HardwareTest/*.cbuild-pack.yml
  Examples/HardwareTest/*.cbuild-set.yml
  Examples/HardwareTest/*.cbuild-run.yml
  Examples/HardwareTest/*.cbuild.yml
  Examples/HardwareTest/RTE/AC6/_*
  Examples/HardwareTest/RTE/GCC/_*
"

# Specify patches to be applied
# Default: empty
#
# PACK_PATCH_FILES="
#     <list patches here>
# "

# Specify addition argument to packchk
# Default: empty
#
PACKCHK_ARGS=()

# Specify additional dependencies for packchk
# Default: empty
#
PACKCHK_DEPS="
  Microchip.SAMD51_DFP.pdsc
  ARM.CMSIS.pdsc
"

# Optional: restrict fallback modes for changelog generation
# Default: full
# Values:
# - full      Tag annotations, release descriptions, or commit messages (in order)
# - release   Tag annotations, or release descriptions (in order)
# - tag       Tag annotations only
#
# PACK_CHANGELOG_MODE="<full|release|tag>"

# Specify file patterns to be excluded from the checksum file
# Default: <empty>
# Values:
# - empty          All files packaged are included in the checksum file
# - glob pattern   One glob pattern per line. Files matching a given pattern are excluded
#                  from the checksum file
# - "*"            The * (match all pattern) can be used to skip checksum file creating completely.
#
# PACK_CHECKSUM_EXCLUDE="
#   <list file patterns here>
# "

#
# custom pre-processing steps
#
# usage: preprocess <build>
#   <build>  The build folder
#
function preprocess() {
  # add custom steps here to be executed
  # before populating the pack build folder

  # Knowledge for the CMSIS Developer Assistant: .knowledge/units/*.md -> .knowledge/knowledge.db
  # (pip install cmsis-developer-assistant-tools). gen-pack ignores the return value, so stop here on failure.
  # The pack folder comes first: --fetch takes an optional index URL and would swallow a following '.'
  cmsis-knowledge build . --strict --fetch || exit 1
  return 0
}

#
# custom post-processing steps
#
# usage: postprocess <build>
#   <build>  The build folder
#
function portable_hardware_test_solution() {
  # Filter stdin to stdout so this transformation can be checked without a
  # pack build. Match the BSP entry, not unrelated packs or arbitrary paths.
  awk '
    /^[[:space:]]*# Build this checkout, including its local drivers and safe flash algorithm\.[[:space:]]*$/ { next }
    {
      if ($0 ~ /^[[:space:]]*-[[:space:]]+pack:/) {
        bsp = $0 ~ /^[[:space:]]*-[[:space:]]+pack:[[:space:]]*SeeedStudio::WioTerminal_BSP(@[^[:space:]#]+)?[[:space:]]*(#.*)?$/
        if (bsp) {
          entries++
          indent = match($0, /[^[:space:]]/) - 1
        }
      } else if (bsp && $0 !~ /^[[:space:]]*($|#)/ &&
                 match($0, /[^[:space:]]/) - 1 <= indent) {
        bsp = 0
      }
      if (bsp && $0 ~ /^[[:space:]]*path:/) {
        if ($0 !~ /^[[:space:]]*path:[[:space:]]*\.\.\/\.\.[[:space:]]*(#.*)?$/) {
          print "Unexpected HardwareTest BSP path; refusing to package" > "/dev/stderr"
          failed = 1
          exit 1
        }
        removed++
        next
      }
      print
    }
    END {
      if (!failed && (entries != 1 || removed != 1)) {
        print "Expected exactly one HardwareTest BSP local-path override" > "/dev/stderr"
        exit 1
      }
    }
  '
}

function postprocess() {
  # Source-tree builds use the local BSP. A copied pack example must instead
  # resolve the installed BSP, independently of its destination directory.
  local solution="${1:?Missing pack build folder}/Examples/HardwareTest/HardwareTest.csolution.yml"
  local source_solution="${WIO_PACK_SOURCE_DIR}/Examples/HardwareTest/HardwareTest.csolution.yml"
  local temporary_solution

  if [[ ! -f "${source_solution}" || ! -f "${solution}" || "${solution}" -ef "${source_solution}" ]]; then
    printf '%s\n' "Refusing to transform a missing or source HardwareTest solution" >&2
    exit 1
  fi
  temporary_solution="$(mktemp "${solution}.XXXXXX")" || exit 1
  if ! portable_hardware_test_solution < "${solution}" > "${temporary_solution}"; then
    rm -f -- "${temporary_solution}"
    exit 1
  fi
  chmod 0644 "${temporary_solution}" || exit 1
  mv -- "${temporary_solution}" "${solution}" || exit 1
  return 0
}

############ DO NOT EDIT BELOW ###########

# Set GEN_PACK_LIB_PATH to use a specific gen-pack library root
# ... instead of bootstrap based on REQUIRED_GEN_PACK_LIB
if [[ -n "${GEN_PACK_LIB_PATH}" ]] && [[ -f "${GEN_PACK_LIB_PATH}/gen-pack" ]]; then
  . "${GEN_PACK_LIB_PATH}/gen-pack"
else
  . <(curl -sL "https://raw.githubusercontent.com/Open-CMSIS-Pack/gen-pack/main/bootstrap")
fi

gen_pack "${DEFAULT_ARGS[@]}" "$@"

exit 0
