#!/bin/bash
find include src tests -type f \( -name "*.c" -o -name "*.h" \) | xargs clang-format -i
