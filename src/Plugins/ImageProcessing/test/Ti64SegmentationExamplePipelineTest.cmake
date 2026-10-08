include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/RunExamplePipelineChain.cmake")
get_filename_component(runtime_dir "${NXRUNNER}" DIRECTORY)
set(OUTPUT_CATEGORY "Ti64_Segmentation_Examples")
set(pipeline_stems
  "(01) Ti64 Connected Bright Regions"
  "(02) Ti64 Connectivity Comparison"
  "(03) Ti64 Minimum Size Cleanup"
)
foreach(stem IN LISTS pipeline_stems)
  validate_companion("${EXAMPLE_DIR}/${stem}.d3dpipeline")
endforeach()
run_example_prerequisite("Ti64_Image_Cleanup_Examples" "${CMAKE_CURRENT_LIST_DIR}/Ti64ImageCleanupExamplePipelineTest.cmake"
  "${CMAKE_CURRENT_LIST_DIR}/../pipelines/Ti64_Image_Cleanup_Examples"
  "-DRAW_DATA_FILE=${runtime_dir}/Data/ImageProcessing_Examples/materials/microstructure_grayscale.png")
run_example_pipeline_chain()
