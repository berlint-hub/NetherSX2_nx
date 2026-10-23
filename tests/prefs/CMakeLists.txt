cmake_minimum_required(VERSION 3.22)
project(NetherSX2PrefsValidation LANGUAGES C)

add_executable(prefs_validation
  validation.c
  ../../source/prefs.c)
target_compile_features(prefs_validation PRIVATE c_std_11)
target_include_directories(prefs_validation PRIVATE ../../source)

add_executable(saltysd_fps_policy_validation
  ../saltysd_fps_policy.c)
target_compile_features(saltysd_fps_policy_validation PRIVATE c_std_11)

enable_testing()
add_test(NAME prefs_validation COMMAND prefs_validation)
add_test(NAME saltysd_fps_policy_validation COMMAND saltysd_fps_policy_validation)
