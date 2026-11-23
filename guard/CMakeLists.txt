add_executable(CinemaxGuard WIN32 src/GuardMain.cpp)
target_link_libraries(CinemaxGuard PRIVATE cinemax_win)
set_target_properties(CinemaxGuard PROPERTIES FOLDER "CINEMAX")
