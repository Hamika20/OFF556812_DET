cmake_minimum_required(VERSION 3.13)
project(python_detector_app C)
find_package(FlipperZero REQUIRED)
add_flipperzero_app(python_detector_app python_detector_app.c)