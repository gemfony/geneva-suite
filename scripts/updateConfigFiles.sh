#!/bin/bash

################################################################################
#
# This file is part of the Geneva library collection. The following license
# applies to this file:
#
# ------------------------------------------------------------------------------
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ------------------------------------------------------------------------------
#
# Note that other files in the Geneva library collection may use a different
# license. Please see the licensing information in each file.
#
################################################################################
#
# See the NOTICE file in the top-level directory of the Geneva library
# collection for a list of contributors and copyright information.
#
################################################################################
# This script may be used to generate or update the configuration
# files of all tests and examples in the Geneva tree. This script
# requires a fully built Geneva tree, and takes the location of
# the source tree and the location of the fully built binary tree
# as parameters.
# NOTE: running any Geneva test or example in an empty folder will
# automatically generate all the necessary configuration files
# where all the parameters have default values, if an empty
# './config' folder is present. This script only automates the
# calling of all test and example executables and the copying of
# the new files into the source tree.
####################################################################

# Check the parameters
if [ ! $# -eq 2 ]; then

	echo -e "\nUsage: `basename $0` <GENEVA_SOURCE_DIR> <GENEVA_BUILD_DIR>\n"
	exit 1

fi

DIR_GENEVA_SRC="$1"
DIR_GENEVA_BUILD="$2"

if [ ! -d "$DIR_GENEVA_SRC" ]; then
	echo -e "\nError: Folder '$DIR_GENEVA_SRC' does not exist.\nAborting...\n"
	exit 2
fi
if [ ! -d "$DIR_GENEVA_BUILD" ]; then
	echo -e "\nError: Folder '$DIR_GENEVA_BUILD' does not exist.\nAborting...\n"
	exit 2
fi

if [ ! -e "$DIR_GENEVA_SRC/CMakeLists.txt" ] \
		|| [ ! -d "$DIR_GENEVA_SRC/libs/geneva/examples" ] \
		|| [ ! -d "$DIR_GENEVA_SRC/libs/geneva/tests" ]; then
	echo -e "\nError: Folder '$DIR_GENEVA_SRC' does not seem to contain a Geneva source tree.\nAborting...\n"
	exit 2
fi

if [ ! -e "${DIR_GENEVA_BUILD}/CMakeCache.txt" ] \
		|| [ ! -x "${DIR_GENEVA_BUILD}/libs/geneva/examples/01_GSimpleOptimizer/GSimpleOptimizer" ] \
		|| [ ! -x "${DIR_GENEVA_BUILD}/libs/geneva/tests/UnitTests/GenevaStandardTests" ]; then
	echo -e "\nError: Folder '$DIR_GENEVA_BUILD' does not seem to contain a built Geneva build tree.\nAborting...\n"
	exit 2
fi

####################################################################
# Warn the user and ask for confirmation

echo -e "\nWARNING: This script will delete all 'config/*.json' files in the given" \
	"Geneva source tree, and replace them with newly generated files."
read -p "Are you sure you want to continue? (y/N) " ANS

if [ "x$ANS" != "xy" ] && [ "x$ANS" != "xY" ]; then
	echo -e "\nOk, no files were changed...\n"
	exit 0
fi

####################################################################
# Collect the configuration folders and files to update

pushd "$DIR_GENEVA_SRC" > /dev/null

# Since the per-library layout migration the config-generating programs live in per-library
# subtrees (libs/geneva/examples, libs/geneva/tests, libs/geneva/benchmarks, libs/common/examples, ...), not in a
# top-level examples/ and tests/. Discover every 'config' directory under the library subtrees;
# there are no filenames/paths with spaces in the Geneva sources.
GENEVA_LIBS="common hap courtier geneva"
CONFIG_DIRS_EX=`find $GENEVA_LIBS -type d -path '*/examples/*' -name config 2>/dev/null | sort`
CONFIG_DIRS_TST=`find $GENEVA_LIBS -type d \( -path '*/tests/*' -o -path '*/benchmarks/*' \) -name config 2>/dev/null | sort`

####################################################################
# Cleanup the old configuration files and generate new ones

for d in $CONFIG_DIRS_EX $CONFIG_DIRS_TST ; do
	rm -f "$d"/*.json
done

popd > /dev/null
pushd "$DIR_GENEVA_BUILD" > /dev/null

# The configuration files in the build tree are copied from the
# source tree by CMake at build time, remove them also
for d in $CONFIG_DIRS_EX $CONFIG_DIRS_TST ; do
	rm -f "$d"/*.json
done

echo -e "\n======================================================================================\n"

ALGS="ea,gd,ps,sa,swarm"
OPT_ALGS="--optimizationAlgorithms"

# Run each program from its own subfolder
for d in $CONFIG_DIRS_EX $CONFIG_DIRS_TST ; do

	PRGM_DIR=`dirname $d`
	pushd $PRGM_DIR > /dev/null
	PRGM=`find -maxdepth 1 -type f -executable`

	echo -e "Running  '$PRGM'  to generate default configuration files..."

	# Check if different optimization algotithms are possible...
	# ... Go2.json config files are generated here already, redirect stderr!
	if `"$PRGM" --help 2> /dev/null | grep -q ".$OPT_ALGS"`; then
		# Configuration files are created all together at the beginning
		PARAMS="$OPT_ALGS $ALGS"
	else
		PARAMS=""
	fi
	timeout 5s "$PRGM" $PARAMS > /dev/null 2>&1
	popd > /dev/null

done

popd > /dev/null

echo -e "\n======================================================================================\n"

####################################################################
# Copy the newly generated configuration files to the source tree

echo -en "Copying newly generated files to the source tree... "

for d in $CONFIG_DIRS_EX $CONFIG_DIRS_TST ; do
	cp "${DIR_GENEVA_BUILD}"/$d/*.json "${DIR_GENEVA_SRC}"/$d
done

echo -e "Done!"

####################################################################
# Finish by telling the user how to continue
echo -e "\n---------------------------------------------------------------------\n"
echo -e "If everything did run correctly, you should now have updated configuration" \
	"files in the Geneva source tree. Please consider checking them back into the" \
	"repository!\n\n"
echo -e "\n---------------------------------------------------------------------\n"

####################################################################
# Done
