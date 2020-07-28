#!/usr/bin/env bash

python3 -m pip install --user --upgrade setuptools wheel
rm -rf build dist evaldb.egg-info
python3 setup.py sdist bdist_wheel
python3 -m twine upload --repository pypi dist/*
