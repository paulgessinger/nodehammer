#!/bin/bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}"   )" &> /dev/null && pwd   )


source $HOME/spack/share/spack/setup-env.sh

spack env activate nodehammer-dev

odd_root=$HOME/dev/OpenDataDetector
source $odd_root/install/bin/this_odd.sh

odd_xml=$odd_root/xml/OpenDataDetector.xml

config_file=$SCRIPT_DIR/fixtures/configs/odd_tracker.toml

pushd $SCRIPT_DIR/build

$SCRIPT_DIR/build/nodehammer dump-semantic -i $odd_xml -c $config_file -o odd.json
$SCRIPT_DIR/build/nodehammer convert -i $odd_xml -c $config_file -o odd.glb
