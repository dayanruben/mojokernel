from setuptools import setup
from setuptools.dist import Distribution
from pathlib import Path

class BinaryDistribution(Distribution):
    """Force platform wheel when binary is present."""
    def has_ext_modules(self):
        return (Path(__file__).parent / "mojokernel" / "bin" / "mojo-repl-server").exists()

setup(distclass=BinaryDistribution)
