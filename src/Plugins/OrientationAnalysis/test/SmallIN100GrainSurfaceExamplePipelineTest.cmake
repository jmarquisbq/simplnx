include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/RunExamplePipelineChain.cmake")
set(OUTPUT_CATEGORY "Small_IN100_Grain_Surface_Examples")
set(pipeline_stems
  "(01) Small IN100 Complete Grain Surface"
  "(02) Small IN100 Grain Surface Measurements"
  "(03) Small IN100 Grain Point Cloud Registration"
  "(04) Small IN100 Point Cloud Interchange"
)
foreach(stem IN LISTS pipeline_stems)
  validate_companion("${EXAMPLE_DIR}/${stem}.d3dpipeline")
endforeach()
run_example_prerequisite("Small_IN100_Geometry_Examples" "${CMAKE_CURRENT_LIST_DIR}/SmallIN100GeometryExamplePipelineTest.cmake"
  "${CMAKE_CURRENT_LIST_DIR}/../pipelines/Small_IN100_Geometry_Examples")
run_example_pipeline_chain()
