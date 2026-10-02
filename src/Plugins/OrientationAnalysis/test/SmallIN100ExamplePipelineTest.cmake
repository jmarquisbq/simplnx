# Smoke-test the public Small IN100 archive and the two pilot example pipelines.
# This checks execution and output presence, not scientific correctness.

foreach(required_var NXRUNNER RAW_DATA_DIR ARCHIVE_PIPELINE EXAMPLE_DIR WORK_DIR TEST_BINARY_ROOT)
  if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
    message(FATAL_ERROR "Missing ${required_var}")
  endif()
endforeach()

foreach(required_path "${NXRUNNER}"
                      "${ARCHIVE_PIPELINE}"
                      "${EXAMPLE_DIR}/(01) Small IN100 Feature Preparation.d3dpipeline"
                      "${EXAMPLE_DIR}/(02) Small IN100 Feature Measurements.d3dpipeline")
  if(NOT EXISTS "${required_path}")
    message(FATAL_ERROR "Required executable or pipeline is missing: ${required_path}")
  endif()
endforeach()

# Keep each human/AI companion aligned with its executable pipeline. The YAML
# files use JSON syntax, so CMake can inspect them without a Python dependency.
function(validate_companion pipeline_path)
  get_filename_component(pipeline_stem "${pipeline_path}" NAME_WE)
  get_filename_component(pipeline_dir "${pipeline_path}" DIRECTORY)
  set(sidecar_path "${pipeline_dir}/${pipeline_stem}.yaml")
  if(NOT EXISTS "${sidecar_path}")
    message(FATAL_ERROR "Missing sidecar for ${pipeline_path}: ${sidecar_path}")
  endif()
  file(READ "${pipeline_path}" pipeline_json)
  file(READ "${sidecar_path}" sidecar_json)
  string(JSON step_count ERROR_VARIABLE pipeline_error LENGTH "${pipeline_json}" pipeline)
  string(JSON sidecar_step_count ERROR_VARIABLE sidecar_error LENGTH "${sidecar_json}" steps)
  if(NOT pipeline_error STREQUAL "NOTFOUND" OR NOT sidecar_error STREQUAL "NOTFOUND")
    message(FATAL_ERROR "Invalid pipeline or sidecar JSON: ${pipeline_path}, ${sidecar_path}")
  endif()
  if(NOT step_count EQUAL sidecar_step_count)
    message(FATAL_ERROR "Step count differs: ${pipeline_path} has ${step_count}, ${sidecar_path} has ${sidecar_step_count}")
  endif()

  math(EXPR last_step "${step_count} - 1")
  foreach(step RANGE 0 ${last_step})
    string(JSON sidecar_index GET "${sidecar_json}" steps ${step} index)
    string(JSON pipeline_uuid GET "${pipeline_json}" pipeline ${step} filter uuid)
    string(JSON sidecar_uuid GET "${sidecar_json}" steps ${step} uuid)
    string(JSON full_filter_name GET "${pipeline_json}" pipeline ${step} filter name)
    string(REGEX REPLACE "^.*::" "" filter_class "${full_filter_name}")
    string(JSON sidecar_filter GET "${sidecar_json}" steps ${step} filter)
    if(NOT sidecar_index EQUAL step OR NOT pipeline_uuid STREQUAL sidecar_uuid OR NOT filter_class STREQUAL sidecar_filter)
      message(FATAL_ERROR "Step ${step} index/filter/UUID mismatch in ${pipeline_path} and ${sidecar_path}")
    endif()

    string(JSON pipeline_param_version GET "${pipeline_json}" pipeline ${step} args parameters_version)
    string(JSON sidecar_param_version GET "${sidecar_json}" steps ${step} parameters_version)
    if(NOT pipeline_param_version STREQUAL sidecar_param_version)
      message(FATAL_ERROR "Step ${step} parameters_version mismatch in ${pipeline_path} and ${sidecar_path}")
    endif()

    string(JSON arg_count LENGTH "${pipeline_json}" pipeline ${step} args)
    string(JSON key_count LENGTH "${sidecar_json}" steps ${step} key_parameters)
    math(EXPR expected_key_count "${arg_count} - 1")
    if(NOT key_count EQUAL expected_key_count)
      message(FATAL_ERROR "Step ${step} parameter-key count mismatch in ${pipeline_path} and ${sidecar_path}")
    endif()
    math(EXPR last_arg "${arg_count} - 1")
    foreach(arg RANGE 0 ${last_arg})
      string(JSON key MEMBER "${pipeline_json}" pipeline ${step} args ${arg})
      if(key STREQUAL "parameters_version")
        continue()
      endif()
      string(JSON source_type TYPE "${pipeline_json}" pipeline ${step} args "${key}" value)
      string(JSON sidecar_type ERROR_VARIABLE key_error TYPE "${sidecar_json}" steps ${step} key_parameters "${key}")
      if(NOT key_error STREQUAL "NOTFOUND" OR NOT source_type STREQUAL sidecar_type)
        message(FATAL_ERROR "Step ${step} parameter ${key} type/missing-key mismatch in ${pipeline_path} and ${sidecar_path}")
      endif()
      string(JSON source_value GET "${pipeline_json}" pipeline ${step} args "${key}" value)
      string(JSON sidecar_value GET "${sidecar_json}" steps ${step} key_parameters "${key}")
      if(source_type STREQUAL "OBJECT" OR source_type STREQUAL "ARRAY")
        string(JSON values_equal EQUAL "${source_value}" "${sidecar_value}")
      elseif("${source_value}" STREQUAL "${sidecar_value}")
        set(values_equal TRUE)
      else()
        set(values_equal FALSE)
      endif()
      if(NOT values_equal)
        message(FATAL_ERROR "Step ${step} parameter ${key} value mismatch in ${pipeline_path} and ${sidecar_path}")
      endif()
    endforeach()
  endforeach()
endfunction()

validate_companion("${EXAMPLE_DIR}/(01) Small IN100 Feature Preparation.d3dpipeline")
validate_companion("${EXAMPLE_DIR}/(02) Small IN100 Feature Measurements.d3dpipeline")

# Check every raw section before staging or running a pipeline.
foreach(slice RANGE 1 117)
  if(NOT EXISTS "${RAW_DATA_DIR}/Slice_${slice}.ang" OR IS_DIRECTORY "${RAW_DATA_DIR}/Slice_${slice}.ang")
    message(FATAL_ERROR "Required Small IN100 section is missing: ${RAW_DATA_DIR}/Slice_${slice}.ang")
  endif()
endforeach()

file(MAKE_DIRECTORY "${WORK_DIR}")
file(REAL_PATH "${WORK_DIR}" work_root)
file(REAL_PATH "${TEST_BINARY_ROOT}" test_binary_root)
cmake_path(IS_PREFIX test_binary_root "${work_root}" NORMALIZE inside_test_tree)
if(NOT inside_test_tree)
  message(FATAL_ERROR "Test work directory is outside the test binary tree: ${work_root}")
endif()

file(REMOVE
  "${work_root}/archive-preflight.log" "${work_root}/archive-execute.log"
  "${work_root}/preparation-preflight.log" "${work_root}/preparation-execute.log"
  "${work_root}/measurements-preflight.log" "${work_root}/measurements-execute.log")

set(raw_stage "${work_root}/Data/Small_IN100")
file(MAKE_DIRECTORY "${raw_stage}")
file(REAL_PATH "${raw_stage}" raw_stage_real)
cmake_path(IS_PREFIX work_root "${raw_stage_real}" NORMALIZE raw_stage_inside)
if(NOT raw_stage_inside)
  message(FATAL_ERROR "Raw-data staging directory is outside the test work directory: ${raw_stage_real}")
endif()

foreach(slice RANGE 1 117)
  file(COPY_FILE "${RAW_DATA_DIR}/Slice_${slice}.ang" "${raw_stage_real}/Slice_${slice}.ang" ONLY_IF_DIFFERENT)
endforeach()

# Remove only the known products after checking the resolved parent directories.
# Each checked output must therefore come from this test invocation.
set(known_outputs
    "Data/Output/Reconstruction/Small_IN100.h5ebsd"
    "Data/Output/Small_IN100_Examples/Preparation/SmallIN100_Features.dream3d"
    "Data/Output/Small_IN100_Examples/Preparation/SmallIN100_Features.xdmf"
    "Data/Output/Small_IN100_Examples/Measurements/SmallIN100_FeatureMeasurements.csv"
    "Data/Output/Small_IN100_Examples/Measurements/SmallIN100_FeatureMeasurements.dream3d")
foreach(relative_path IN LISTS known_outputs)
  set(output_path "${work_root}/${relative_path}")
  get_filename_component(output_parent "${output_path}" DIRECTORY)
  file(MAKE_DIRECTORY "${output_parent}")
  file(REAL_PATH "${output_parent}" output_parent_real)
  cmake_path(IS_PREFIX work_root "${output_parent_real}" NORMALIZE output_inside)
  if(NOT output_inside)
    message(FATAL_ERROR "Generated output is outside the test work directory: ${output_path}")
  endif()
  file(REMOVE "${output_path}")
  if(EXISTS "${output_path}")
    message(FATAL_ERROR "Could not clear stale generated output: ${output_path}")
  endif()
endforeach()

function(run_pipeline stage pipeline)
  foreach(mode --preflight --execute)
    string(SUBSTRING "${mode}" 2 -1 mode_name)
    set(log_file "${work_root}/${stage}-${mode_name}.log")
    execute_process(
      COMMAND "${NXRUNNER}" "${mode}" "${pipeline}"
      WORKING_DIRECTORY "${work_root}"
      RESULT_VARIABLE result
      OUTPUT_VARIABLE stdout
      ERROR_VARIABLE stderr)
    file(WRITE "${log_file}" "Command: ${NXRUNNER} ${mode} ${pipeline}\nResult: ${result}\n\n${stdout}\n${stderr}\n")
    if(NOT "${result}" STREQUAL "0")
      message(FATAL_ERROR "${stage} ${mode} failed (${result}); see ${log_file}\n${stdout}\n${stderr}")
    endif()
  endforeach()
endfunction()

function(require_output relative_path)
  set(output_path "${work_root}/${relative_path}")
  if(NOT EXISTS "${output_path}")
    message(FATAL_ERROR "Current execution did not create ${output_path}")
  endif()
  file(SIZE "${output_path}" output_size)
  if(output_size EQUAL 0)
    message(FATAL_ERROR "Current execution created an empty output: ${output_path}")
  endif()
endfunction()

run_pipeline("archive" "${ARCHIVE_PIPELINE}")
require_output("Data/Output/Reconstruction/Small_IN100.h5ebsd")

run_pipeline("preparation" "${EXAMPLE_DIR}/(01) Small IN100 Feature Preparation.d3dpipeline")
require_output("Data/Output/Small_IN100_Examples/Preparation/SmallIN100_Features.dream3d")

run_pipeline("measurements" "${EXAMPLE_DIR}/(02) Small IN100 Feature Measurements.d3dpipeline")
require_output("Data/Output/Small_IN100_Examples/Measurements/SmallIN100_FeatureMeasurements.csv")
require_output("Data/Output/Small_IN100_Examples/Measurements/SmallIN100_FeatureMeasurements.dream3d")

set(csv_path "${work_root}/Data/Output/Small_IN100_Examples/Measurements/SmallIN100_FeatureMeasurements.csv")
file(STRINGS "${csv_path}" first_two_lines LIMIT_COUNT 2)
list(LENGTH first_two_lines line_count)
if(line_count LESS 2)
  message(FATAL_ERROR "Feature CSV has no data rows: ${csv_path}")
endif()
list(GET first_two_lines 0 header)
string(REPLACE "," ";" columns "${header}")
list(GET columns 0 first_column)
list(FIND columns "NumElements" num_elements_index)
if(NOT first_column STREQUAL "Feature_ID" OR num_elements_index LESS 0)
  message(FATAL_ERROR "Feature CSV lacks Feature_ID or NumElements header: ${csv_path}")
endif()

message(STATUS "Small IN100 example smoke test passed; logs and outputs: ${work_root}")
