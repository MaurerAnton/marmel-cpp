cmake_minimum_required(VERSION 3.20)
project(smallagent
  DESCRIPTION "C++ translations of marmel commits, one folder per upstream commit"
  LANGUAGES CXX)

# Each commits/NN-* folder is a self-contained CMake project mirroring one
# upstream commit. Add new translations here as they land.
add_subdirectory(commits/01-marmel-v0.1)
