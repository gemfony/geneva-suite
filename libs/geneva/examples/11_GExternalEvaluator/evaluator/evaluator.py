#!/usr/bin/env python3

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

# The data exchanged with Geneva's GExternalEvaluatorIndividual is JSON. The three document
# shapes are:
#   setup_data     (this script -> Geneva): the problem structure (bounds, initial values)
#   run_parameters (Geneva -> this script): the parameters to evaluate
#   run_results    (this script -> Geneva): the computed result(s)
# See the batch schema below; each uses a top-level object with an "individuals" array.

import argparse
import json
import os
import sys


def init():
    """
    Perform initialization work, for instance to set up the optimization environment.
    """
    print("Initializing the optimization environment...")
    # Nothing to do...

def finalize():
    """
    Perform finalization work, for instance to clean up the optimization environment.
    """
    print("Cleaning up the optimization environment...")
    # Nothing to do...

def setup(setup_file, initial_values):
    """
    Provide the problem setup data inside a JSON file of the given name.
    """

    if initial_values == "min":
        value, init_random = -10.0, False
    elif initial_values == "max":
        value, init_random = 10.0, False
    else:
        value, init_random = 0.0, True

    variables = []
    for i in range(4):
        variables.append({
            "name": "coord_" + str(i),
            "type": "GConstrainedDoubleObject",
            "baseType": "double",
            "isLeaf": True,
            "nVals": 1,
            "values": [value],
            "lowerBoundary": -10.0,
            "upperBoundary": 10.0,
            "initRandom": init_random,
        })

    document = {
        "dataType": "setup_data",
        "run_id": "0",
        "n_individuals": 1,
        "individuals": [
            {
                "type": "GGenome",
                "nVars": 4,
                "n_results": 1,
                "nBounds": 0,
                "vars": variables,
            }
        ],
    }

    with open(setup_file, 'w') as infile:
        json.dump(document, infile, indent=4)

def evaluate(in_file, out_file):
    """
    Evaluate the result for the given input parameters, and
    generate an output file.
    """
    (iteration, it_id, result) = process_input(in_file)
    write_output(out_file, iteration, it_id, result)

def archive(in_file):
    """
    Evaluate the result for the given input parameters, and
    archive the results.
    """
    (iteration, it_id, result) = process_input(in_file)
    archive_output(iteration, it_id, result)

def process_input(in_file):
    """
    Read the input data and process it to get the results.
    """
    (iteration, it_id, x, y, z, w) = read_input(in_file)

    # This is an example, we just calculate a 4D parabola...
    result = x**2 + y**2 + w**2 + z**2

    return (iteration, it_id, result)

def write_output(out_file, iteration, it_id, result):
    """
    Write the JSON output file containing the calculated results.
    """
    document = {
        "dataType": "run_results",
        "run_id": "0",
        "n_individuals": 1,
        "individuals": [
            {
                "iteration": iteration,
                "id": str(it_id),
                "isValid": True,
                "isDirty": False,
                "n_results": 1,
                "results": [
                    {"rawResult": result}
                ],
            }
        ],
    }
    with open(out_file, 'w') as ofile:
        json.dump(document, ofile, indent=4)

def archive_output(iteration, it_id, result):
    """
    Archive the calculated results (for instance, this could mean
    to store the results in a DB).
    """
    print("Archiving the results...")
    # Nothing to do at the moment

def read_input(in_file):
    """
    Read the JSON input file and extract the parameters for the run.
    """

    try:
        with open(in_file) as ifile:
            document = json.load(ifile)
    except (OSError, ValueError) as e:
        sys.exit("\nERROR: parsing error on input file '" + str(in_file) + "':\n\t" + str(e))

    #
    # More error handling would be needed here in case the JSON file
    # doesn't have the expected structure.
    #
    ind = document["individuals"][0]

    try:
        iteration = int(ind["iteration"])
    except (KeyError, ValueError, TypeError):
        iteration = -1

    try:
        it_id = ind["id"]
    except KeyError:
        it_id = "UNKNOWN_ID"

    nr_params = int(ind["nVars"])
    if nr_params != 4:
        sys.exit("\nERROR: unexpected parameter: nVars=" + str(nr_params) + " (expected 4)!")

    # The flat genome (GGenome::toJSON) writes one scalar "value" per entry of the "vars" array.
    params = ind["vars"]
    if len(params) != nr_params:
        sys.exit("\nERROR: inconsistent data in input file: nVars=" + str(nr_params)
                 + ", but found " + str(len(params)) + " parameters!")

    x = float(params[0]["value"])
    y = float(params[1]["value"])
    z = float(params[2]["value"])
    w = float(params[3]["value"])

    return (iteration, it_id, x, y, z, w)


def main(argv):
    """
    This script encapsulates all the problem-specific information for an optimization
    problem. It is called by the optimizer to evaluate some parameter set, and provides
    the results in an output file.
    """

    parser = argparse.ArgumentParser(description="External evaluator")

    parser.add_argument("--initvalues", help="Start with the given initial values,"
                        " either 'min' or 'max' (default: 'random')")

    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--init", help="Perform initialization work",
                       action="store_true")
    group.add_argument("--finalize", help="Perform finalization work",
                       action="store_true")
    group.add_argument("--setup", help="Provide initialization data to the caller",
                       action="store_true")
    group.add_argument("--evaluate", help="Evaluate the parameters from the input file",
                       action="store_true")
    group.add_argument("--archive", help="Archive the results",
                       action="store_true")

    parser.add_argument("--input", help="Read the input data from the given file"
                        " (default: 'input.json')",
                        metavar='IN_FILE', default="input.json")
    parser.add_argument("--output", help="Write the results to the given file"
                        " (default: 'output.json')",
                        metavar='OUT_FILE', default="output.json")

    args = parser.parse_args()

    # These options checks are not supported by argparse (in Python 2.7 at least)
    if args.initvalues and not args.setup:
        sys.exit('The option --initvalues may only be used with --setup! Aborting...')

    initial_values = args.initvalues or "random"
    if initial_values not in set(["min", "max", "random"]):
        sys.exit("Wrong --initvalues value, use either 'min' or 'max'! Aborting...")

    # Now do our job
    if args.init:
        init()
    elif args.finalize:
        finalize()
    elif args.setup:
        setup(args.output, initial_values)
    elif args.evaluate:
        evaluate(args.input, args.output)
    elif args.archive:
        archive(args.input)
    else:
        sys.exit("ERROR: inconsistent options! why are we here?")


def command_name():
    """Return a cleaned up name of this program."""
    return os.path.basename(sys.argv[0])

if __name__ == "__main__":
    main(sys.argv[1:])
