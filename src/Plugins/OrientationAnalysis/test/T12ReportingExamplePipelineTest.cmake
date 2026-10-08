include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/RunExamplePipelineChain.cmake")
get_filename_component(runtime_dir "${NXRUNNER}" DIRECTORY)
set(OUTPUT_CATEGORY "T12_Reporting_Examples")
set(pipeline_stems
  "(01) T12 Derived Reporting Fields"
  "(02) T12 Feature to Cell Mapping"
  "(03) T12 Compact Report"
)
foreach(stem IN LISTS pipeline_stems)
  validate_companion("${EXAMPLE_DIR}/${stem}.d3dpipeline")
endforeach()
run_example_prerequisite("T12_Orientation_Examples" "${CMAKE_CURRENT_LIST_DIR}/T12OrientationExamplePipelineTest.cmake"
  "${CMAKE_CURRENT_LIST_DIR}/../pipelines/T12_Orientation_Examples"
  "-DRAW_DATA_FILE=${runtime_dir}/Data/T12-MAI-2010/fw-ar-IF1-aptr12-corr.ctf")
run_example_pipeline_chain()
