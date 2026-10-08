include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/RunExamplePipelineChain.cmake")
get_filename_component(runtime_dir "${NXRUNNER}" DIRECTORY)
set(OUTPUT_CATEGORY "T12_Import_Examples")
set(pipeline_stems
  "(01) T12 Typed CSV Import"
  "(02) T12 Selective DREAM3D Import"
)
foreach(stem IN LISTS pipeline_stems)
  validate_companion("${EXAMPLE_DIR}/${stem}.d3dpipeline")
endforeach()
run_example_prerequisite("T12_Reporting_Examples" "${CMAKE_CURRENT_LIST_DIR}/T12ReportingExamplePipelineTest.cmake"
  "${CMAKE_CURRENT_LIST_DIR}/../pipelines/T12_Reporting_Examples"
  )
run_example_pipeline_chain()
