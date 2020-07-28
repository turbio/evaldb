import setuptools

with open("README.md", "r") as fh:
    long_description = fh.read()

setuptools.setup(
    name="evaldb",
    version="0.0.2",
    author="Mason Clayton",
    author_email="mason@turb.io",
    description="Python client for evaldb (evaldb.turb.io).",
    long_description=long_description,
    long_description_content_type="text/markdown",
    url="https://github.com/turbio/evaldb",
    packages=setuptools.find_packages(),
    classifiers=[
        "Programming Language :: Python :: 3",
        "License :: OSI Approved :: MIT License",
        "Operating System :: OS Independent",
    ],
    python_requires='>=3.6',
)
