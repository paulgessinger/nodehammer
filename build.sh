#!/usr/bin/env bash

source $HOME/spack/share/spack/setup-env.sh

spack env activate nodehammer-dev

$@
