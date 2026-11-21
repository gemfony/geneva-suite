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

# This script repeatedly runs a command and saves all its output to a file.
# This is useful if e.g. trying to statistically verify that a non-deterministic error is fixed.

command=$1
count=$2
result_file=$3
timestamp=`date`

echo "running \`$command\` $count times and saving output to \`$result_file\`."

printf "\n------------- timestamp: $timestamp -------------\n" >> "$result_file"

# run many times and save stdout and stderr to a file
for i in $(seq "$count"); do
    echo "starting iteration $i/$count..."
    printf "\n\n------------- running \`$command\` || iteration $i/$count -------------\n\n" >> "$result_file"
    $command >> "$result_file" 2>&1
done

echo "finished all $count iterations."