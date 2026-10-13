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

# Keep the retained-region measurements visible in the existing CSV export.
set(cleanup_csv "${WORK_DIR}/Data/Output/Ti64_Segmentation_Examples/Cleanup/Ti64_MinimumSizeCleanup.csv")
file(STRINGS "${cleanup_csv}" cleanup_rows)
list(POP_FRONT cleanup_rows cleanup_header)
string(REPLACE "," ";" cleanup_columns "${cleanup_header}")
foreach(column Feature_ID FeatureHasData MaximumIntensity MeanIntensity MinimumIntensity PixelCount SelectionClass Omega1 Omega2
    FeatureRect_0 FeatureRect_1 FeatureRect_2 FeatureRect_3 FeatureRect_4 FeatureRect_5
    CentralMoments_0 CentralMoments_1 CentralMoments_2 CentralMoments_3 CentralMoments_4 CentralMoments_5 CentralMoments_6 CentralMoments_7 CentralMoments_8)
  list(FIND cleanup_columns "${column}" column_index)
  if(column_index LESS 0)
    message(FATAL_ERROR "Retained-region CSV is missing ${column}: ${cleanup_csv}")
  endif()
endforeach()
file(READ "${EXAMPLE_DIR}/(03) Ti64 Minimum Size Cleanup.yaml" cleanup_sidecar)
string(JSON expected_regions GET "${cleanup_sidecar}" reference_results retained_regions)
list(LENGTH cleanup_rows exported_regions)
if(NOT exported_regions EQUAL expected_regions)
  message(FATAL_ERROR "Retained-region CSV has ${exported_regions} rows; expected ${expected_regions}: ${cleanup_csv}")
endif()
