# Build defaults for work inside this repository. Not installed with the
# package - consumers keep their own toolchain settings.
#
# The adapter is C++, so Nim needs a C++ compiler. Prefer whatever Nim is
# already configured for, and fall back to Clang only on machines with no g++.
if findExe("g++").len == 0 and findExe("clang++").len > 0:
  switch("cc", "clang")
