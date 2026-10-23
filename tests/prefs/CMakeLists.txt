cmake_minimum_required(VERSION 3.22)
project(NetherSX2PrefsValidation LANGUAGES C)

add_executable(prefs_validation
  validation.c
  ../../source/prefs.c)
target_compile_features(prefs_validation PRIVATE c_std_11)
target_include_directories(prefs_validation PRIVATE ../../source)

enable_testing()
add_test(NAME prefs_validation COMMAND prefs_validation)
