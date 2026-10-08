include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/RunExamplePipelineChain.cmake")
get_filename_component(runtime_dir "${NXRUNNER}" DIRECTORY)
set(OUTPUT_CATEGORY "Small_IN100_Geometry_Examples")
set(pipeline_stems
  "(01) Small IN100 Surface Mesh"
  "(02) Small IN100 Surface Smoothing"
  "(03) Small IN100 Surface Transform and Export"
)
foreach(stem IN LISTS pipeline_stems)
  validate_companion("${EXAMPLE_DIR}/${stem}.d3dpipeline")
endforeach()
run_example_prerequisite("Small_IN100_Examples" "${CMAKE_CURRENT_LIST_DIR}/SmallIN100ExamplePipelineTest.cmake"
  "${CMAKE_CURRENT_LIST_DIR}/../pipelines/Small_IN100_Examples"
  "-DRAW_DATA_DIR=${runtime_dir}/Data/Small_IN100"
  "-DARCHIVE_PIPELINE=${CMAKE_CURRENT_LIST_DIR}/../pipelines/Small_IN100_Processing/(01) Small IN100 Archive.d3dpipeline")
run_example_pipeline_chain()
