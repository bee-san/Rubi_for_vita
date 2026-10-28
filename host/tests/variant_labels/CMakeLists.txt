# The labels host/tests/variant_free.py derives (the root CMakeLists.txt, "variant_free"). ctest reads this
# directory's tests file after the root's, where every test is defined, so the file it includes from here can set
# their labels; the root's own include files are read before its tests.
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES ${VJO_VARIANT_LABELS_INCLUDE})
