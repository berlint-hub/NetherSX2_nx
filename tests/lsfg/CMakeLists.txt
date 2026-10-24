cmake_minimum_required(VERSION 3.22)
project(NetherSX2LsfgValidation LANGUAGES C)

add_executable(lsfg_schedule_validation schedule_validation.c)
target_compile_features(lsfg_schedule_validation PRIVATE c_std_11)
target_include_directories(lsfg_schedule_validation PRIVATE ../../source/lsfg)

enable_testing()
add_test(NAME lsfg_schedule_validation COMMAND lsfg_schedule_validation)
