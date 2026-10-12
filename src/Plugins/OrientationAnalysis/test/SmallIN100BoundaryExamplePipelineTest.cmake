include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/RunExamplePipelineChain.cmake")
get_filename_component(runtime_dir "${NXRUNNER}" DIRECTORY)
set(OUTPUT_CATEGORY "Small_IN100_Boundary_Examples")
set(pipeline_stems "(01) Small IN100 Boundary and Section Measurements")
validate_companion("${EXAMPLE_DIR}/${pipeline_stems}.d3dpipeline")
run_example_prerequisite("Small_IN100_Examples" "${CMAKE_CURRENT_LIST_DIR}/SmallIN100ExamplePipelineTest.cmake"
  "${CMAKE_CURRENT_LIST_DIR}/../pipelines/Small_IN100_Examples"
  "-DRAW_DATA_DIR=${runtime_dir}/Data/Small_IN100"
  "-DARCHIVE_PIPELINE=${CMAKE_CURRENT_LIST_DIR}/../pipelines/Small_IN100_Processing/(01) Small IN100 Archive.d3dpipeline")
run_example_pipeline_chain()
