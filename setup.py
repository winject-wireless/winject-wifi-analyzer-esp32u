from setuptools import Extension, setup

setup(ext_modules=[Extension(
    'winject_wifi_analyzer._native', ['native/viterbi.cpp'],
    language='c++', extra_compile_args=['-O3', '-std=c++17'],
)])
