# applications_user/python_detector/CMakeLists.txt

add_application(
    python_detector_app
    c
    tetra_detector_app.c
)

# No extra include paths needed—everything you’re using comes from the SDK
